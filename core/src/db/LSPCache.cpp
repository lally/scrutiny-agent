// LSPCache.cpp
// SQLite-based cache for LSP query results

#include "db/LSPCache.hpp"
#include <iostream>
#include <sstream>

namespace gitreview {
namespace db {

static const char* SCHEMA = R"(
-- Version tracking for schema migrations
CREATE TABLE IF NOT EXISTS schema_version (
    version INTEGER PRIMARY KEY
);

-- Definitions: where symbols are defined
CREATE TABLE IF NOT EXISTS definitions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    source_file TEXT NOT NULL,
    source_line INTEGER NOT NULL,
    source_character INTEGER NOT NULL,
    target_file TEXT NOT NULL,
    target_line INTEGER NOT NULL,
    target_character INTEGER NOT NULL,
    target_end_line INTEGER NOT NULL,
    target_end_character INTEGER NOT NULL,
    git_sha TEXT NOT NULL,
    created_at INTEGER DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_definitions_source
    ON definitions(source_file, source_line, source_character, git_sha);

-- References: where symbols are used
CREATE TABLE IF NOT EXISTS references_ (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    symbol_file TEXT NOT NULL,
    symbol_line INTEGER NOT NULL,
    symbol_character INTEGER NOT NULL,
    reference_file TEXT NOT NULL,
    reference_line INTEGER NOT NULL,
    reference_character INTEGER NOT NULL,
    reference_end_line INTEGER NOT NULL,
    reference_end_character INTEGER NOT NULL,
    git_sha TEXT NOT NULL,
    created_at INTEGER DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_references_symbol
    ON references_(symbol_file, symbol_line, symbol_character, git_sha);

-- Hover information
CREATE TABLE IF NOT EXISTS hovers (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_path TEXT NOT NULL,
    line INTEGER NOT NULL,
    character INTEGER NOT NULL,
    contents TEXT NOT NULL,
    git_sha TEXT NOT NULL,
    created_at INTEGER DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_hovers_location
    ON hovers(file_path, line, character, git_sha);

-- Document symbols (outline/structure)
CREATE TABLE IF NOT EXISTS document_symbols (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_path TEXT NOT NULL,
    name TEXT NOT NULL,
    kind TEXT NOT NULL,
    detail TEXT,
    line INTEGER NOT NULL,
    character INTEGER NOT NULL,
    end_line INTEGER NOT NULL,
    end_character INTEGER NOT NULL,
    parent_id INTEGER,
    git_sha TEXT NOT NULL,
    created_at INTEGER DEFAULT (strftime('%s', 'now')),
    FOREIGN KEY (parent_id) REFERENCES document_symbols(id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_document_symbols_file
    ON document_symbols(file_path, git_sha);

-- Workspace symbols (global symbol search)
CREATE TABLE IF NOT EXISTS workspace_symbols (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    query TEXT NOT NULL,
    file_path TEXT NOT NULL,
    name TEXT NOT NULL,
    kind TEXT NOT NULL,
    container_name TEXT,
    line INTEGER NOT NULL,
    character INTEGER NOT NULL,
    end_line INTEGER NOT NULL,
    end_character INTEGER NOT NULL,
    git_sha TEXT NOT NULL,
    created_at INTEGER DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_workspace_symbols_query
    ON workspace_symbols(query, git_sha);

-- File cache status: tracks which files have been fully indexed
CREATE TABLE IF NOT EXISTS file_cache_status (
    file_path TEXT PRIMARY KEY,
    git_sha TEXT NOT NULL,
    indexed_at INTEGER DEFAULT (strftime('%s', 'now'))
);

-- Full index: all definitions in the codebase
CREATE TABLE IF NOT EXISTS index_definitions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    file_uri TEXT NOT NULL,
    kind INTEGER NOT NULL,
    start_line INTEGER NOT NULL,
    start_char INTEGER NOT NULL,
    end_line INTEGER NOT NULL,
    end_char INTEGER NOT NULL,
    container_name TEXT,
    git_sha TEXT NOT NULL,
    created_at INTEGER DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_index_definitions_file
    ON index_definitions(file_uri, git_sha);
CREATE INDEX IF NOT EXISTS idx_index_definitions_name
    ON index_definitions(name, git_sha);

-- Full index: all references in the codebase
CREATE TABLE IF NOT EXISTS index_references (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    symbol_name TEXT NOT NULL,
    from_file_uri TEXT NOT NULL,
    from_start_line INTEGER NOT NULL,
    from_start_char INTEGER NOT NULL,
    from_end_line INTEGER NOT NULL,
    from_end_char INTEGER NOT NULL,
    to_file_uri TEXT NOT NULL,
    to_start_line INTEGER NOT NULL,
    to_start_char INTEGER NOT NULL,
    to_end_line INTEGER NOT NULL,
    to_end_char INTEGER NOT NULL,
    git_sha TEXT NOT NULL,
    created_at INTEGER DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_index_references_from
    ON index_references(from_file_uri, git_sha);
CREATE INDEX IF NOT EXISTS idx_index_references_to
    ON index_references(to_file_uri, git_sha);
CREATE INDEX IF NOT EXISTS idx_index_references_symbol
    ON index_references(symbol_name, git_sha);

-- Tracks which files have been fully indexed
CREATE TABLE IF NOT EXISTS indexed_files (
    file_uri TEXT PRIMARY KEY,
    git_sha TEXT NOT NULL,
    indexed_at INTEGER DEFAULT (strftime('%s', 'now'))
);

-- Folding ranges: code folding information
CREATE TABLE IF NOT EXISTS folding_ranges (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    file_path TEXT NOT NULL,
    start_line INTEGER NOT NULL,
    end_line INTEGER NOT NULL,
    kind TEXT,
    git_sha TEXT NOT NULL,
    created_at INTEGER DEFAULT (strftime('%s', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_folding_ranges_file
    ON folding_ranges(file_path, git_sha);
)";

LSPCache::LSPCache(const std::string& dbPath) {
    db_ = std::make_unique<Database>(dbPath);
    if (db_->isOpen()) {
        initializeSchema();
    }
}

LSPCache::~LSPCache() = default;

bool LSPCache::isOpen() const {
    return db_ && db_->isOpen();
}

void LSPCache::initializeSchema() {
    db_->execute(SCHEMA);

    // Check/set schema version
    auto stmt = db_->prepare("SELECT version FROM schema_version LIMIT 1");
    if (stmt && !stmt->step()) {
        // No version set, insert initial version
        db_->execute("INSERT INTO schema_version (version) VALUES (1)");
    }
}

void LSPCache::setCurrentGitSha(const std::string& sha) {
    currentGitSha_ = sha;
}

std::string LSPCache::normalizeFilePath(const std::string& path) const {
    // Remove file:// prefix if present
    if (path.substr(0, 7) == "file://") {
        return path.substr(7);
    }
    return path;
}

bool LSPCache::isCacheValid(const std::string& filePath) const {
    if (!isOpen() || currentGitSha_.empty()) return false;

    auto stmt = db_->prepare(
        "SELECT 1 FROM file_cache_status WHERE file_path = ? AND git_sha = ?"
    );
    if (!stmt) return false;

    stmt->bindText(1, normalizeFilePath(filePath));
    stmt->bindText(2, currentGitSha_);

    return stmt->step();
}

void LSPCache::invalidateFile(const std::string& filePath) {
    if (!isOpen()) return;

    std::string normalized = normalizeFilePath(filePath);

    db_->beginTransaction();

    // Delete from all tables
    auto tables = {"definitions", "references_", "hovers", "document_symbols"};
    for (const char* table : tables) {
        std::string sql = "DELETE FROM " + std::string(table) +
                          " WHERE source_file = ? OR target_file = ? OR file_path = ? OR symbol_file = ? OR reference_file = ?";
        auto stmt = db_->prepare(sql);
        if (stmt) {
            stmt->bindText(1, normalized);
            stmt->bindText(2, normalized);
            stmt->bindText(3, normalized);
            stmt->bindText(4, normalized);
            stmt->bindText(5, normalized);
            stmt->step();
        }
    }

    // Remove from cache status
    auto stmt = db_->prepare("DELETE FROM file_cache_status WHERE file_path = ?");
    if (stmt) {
        stmt->bindText(1, normalized);
        stmt->step();
    }

    db_->commit();
}

void LSPCache::invalidateAllForSha(const std::string& sha) {
    if (!isOpen()) return;

    db_->beginTransaction();

    auto tables = {"definitions", "references_", "hovers", "document_symbols", "workspace_symbols"};
    for (const char* table : tables) {
        std::string sql = "DELETE FROM " + std::string(table) + " WHERE git_sha != ?";
        auto stmt = db_->prepare(sql);
        if (stmt) {
            stmt->bindText(1, sha);
            stmt->step();
        }
    }

    auto stmt = db_->prepare("DELETE FROM file_cache_status WHERE git_sha != ?");
    if (stmt) {
        stmt->bindText(1, sha);
        stmt->step();
    }

    db_->commit();
}

// ============================================================================
// Definition caching
// ============================================================================

void LSPCache::cacheDefinition(const std::string& sourceFile,
                               int sourceLine, int sourceChar,
                               const lsp::LSPLocation& target) {
    if (!isOpen() || currentGitSha_.empty()) return;

    auto stmt = db_->prepare(
        "INSERT INTO definitions "
        "(source_file, source_line, source_character, target_file, "
        "target_line, target_character, target_end_line, target_end_character, git_sha) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
    );
    if (!stmt) return;

    stmt->bindText(1, normalizeFilePath(sourceFile));
    stmt->bindInt(2, sourceLine);
    stmt->bindInt(3, sourceChar);
    stmt->bindText(4, normalizeFilePath(target.uri));
    stmt->bindInt(5, target.range.start.line);
    stmt->bindInt(6, target.range.start.character);
    stmt->bindInt(7, target.range.end.line);
    stmt->bindInt(8, target.range.end.character);
    stmt->bindText(9, currentGitSha_);

    stmt->step();
}

std::optional<std::vector<lsp::LSPLocation>>
LSPCache::getCachedDefinitions(const std::string& sourceFile,
                               int sourceLine, int sourceChar) {
    if (!isOpen() || currentGitSha_.empty()) return std::nullopt;

    auto stmt = db_->prepare(
        "SELECT target_file, target_line, target_character, "
        "target_end_line, target_end_character "
        "FROM definitions "
        "WHERE source_file = ? AND source_line = ? AND source_character = ? AND git_sha = ?"
    );
    if (!stmt) return std::nullopt;

    stmt->bindText(1, normalizeFilePath(sourceFile));
    stmt->bindInt(2, sourceLine);
    stmt->bindInt(3, sourceChar);
    stmt->bindText(4, currentGitSha_);

    std::vector<lsp::LSPLocation> results;
    while (stmt->step()) {
        lsp::LSPLocation loc;
        loc.uri = "file://" + stmt->columnText(0);
        loc.range.start.line = stmt->columnInt(1);
        loc.range.start.character = stmt->columnInt(2);
        loc.range.end.line = stmt->columnInt(3);
        loc.range.end.character = stmt->columnInt(4);
        results.push_back(loc);
    }

    if (results.empty()) return std::nullopt;
    return results;
}

// ============================================================================
// Reference caching
// ============================================================================

void LSPCache::cacheReferences(const std::string& symbolFile,
                               int symbolLine, int symbolChar,
                               const std::vector<lsp::LSPLocation>& references) {
    if (!isOpen() || currentGitSha_.empty()) return;

    db_->beginTransaction();

    for (const auto& ref : references) {
        auto stmt = db_->prepare(
            "INSERT INTO references_ "
            "(symbol_file, symbol_line, symbol_character, reference_file, "
            "reference_line, reference_character, reference_end_line, reference_end_character, git_sha) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
        );
        if (!stmt) continue;

        stmt->bindText(1, normalizeFilePath(symbolFile));
        stmt->bindInt(2, symbolLine);
        stmt->bindInt(3, symbolChar);
        stmt->bindText(4, normalizeFilePath(ref.uri));
        stmt->bindInt(5, ref.range.start.line);
        stmt->bindInt(6, ref.range.start.character);
        stmt->bindInt(7, ref.range.end.line);
        stmt->bindInt(8, ref.range.end.character);
        stmt->bindText(9, currentGitSha_);

        stmt->step();
    }

    db_->commit();
}

std::optional<std::vector<lsp::LSPLocation>>
LSPCache::getCachedReferences(const std::string& symbolFile,
                              int symbolLine, int symbolChar) {
    if (!isOpen() || currentGitSha_.empty()) return std::nullopt;

    auto stmt = db_->prepare(
        "SELECT reference_file, reference_line, reference_character, "
        "reference_end_line, reference_end_character "
        "FROM references_ "
        "WHERE symbol_file = ? AND symbol_line = ? AND symbol_character = ? AND git_sha = ?"
    );
    if (!stmt) return std::nullopt;

    stmt->bindText(1, normalizeFilePath(symbolFile));
    stmt->bindInt(2, symbolLine);
    stmt->bindInt(3, symbolChar);
    stmt->bindText(4, currentGitSha_);

    std::vector<lsp::LSPLocation> results;
    while (stmt->step()) {
        lsp::LSPLocation loc;
        loc.uri = "file://" + stmt->columnText(0);
        loc.range.start.line = stmt->columnInt(1);
        loc.range.start.character = stmt->columnInt(2);
        loc.range.end.line = stmt->columnInt(3);
        loc.range.end.character = stmt->columnInt(4);
        results.push_back(loc);
    }

    if (results.empty()) return std::nullopt;
    return results;
}

// ============================================================================
// Hover caching
// ============================================================================

void LSPCache::cacheHover(const std::string& filePath,
                          int line, int character,
                          const std::string& contents) {
    if (!isOpen() || currentGitSha_.empty()) return;

    auto stmt = db_->prepare(
        "INSERT INTO hovers (file_path, line, character, contents, git_sha) "
        "VALUES (?, ?, ?, ?, ?)"
    );
    if (!stmt) return;

    stmt->bindText(1, normalizeFilePath(filePath));
    stmt->bindInt(2, line);
    stmt->bindInt(3, character);
    stmt->bindText(4, contents);
    stmt->bindText(5, currentGitSha_);

    stmt->step();
}

std::optional<std::string>
LSPCache::getCachedHover(const std::string& filePath,
                         int line, int character) {
    if (!isOpen() || currentGitSha_.empty()) return std::nullopt;

    auto stmt = db_->prepare(
        "SELECT contents FROM hovers "
        "WHERE file_path = ? AND line = ? AND character = ? AND git_sha = ?"
    );
    if (!stmt) return std::nullopt;

    stmt->bindText(1, normalizeFilePath(filePath));
    stmt->bindInt(2, line);
    stmt->bindInt(3, character);
    stmt->bindText(4, currentGitSha_);

    if (stmt->step()) {
        return stmt->columnText(0);
    }

    return std::nullopt;
}

// ============================================================================
// Document symbols caching
// ============================================================================

void LSPCache::cacheDocumentSymbols(const std::string& filePath,
                                    const std::vector<CachedDocumentSymbol>& symbols) {
    if (!isOpen() || currentGitSha_.empty()) return;

    std::string normalized = normalizeFilePath(filePath);

    db_->beginTransaction();

    // First, delete existing symbols for this file
    auto delStmt = db_->prepare(
        "DELETE FROM document_symbols WHERE file_path = ? AND git_sha = ?"
    );
    if (delStmt) {
        delStmt->bindText(1, normalized);
        delStmt->bindText(2, currentGitSha_);
        delStmt->step();
    }

    // Insert new symbols
    for (const auto& sym : symbols) {
        auto stmt = db_->prepare(
            "INSERT INTO document_symbols "
            "(file_path, name, kind, detail, line, character, end_line, end_character, parent_id, git_sha) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
        );
        if (!stmt) continue;

        stmt->bindText(1, normalized);
        stmt->bindText(2, sym.name);
        stmt->bindText(3, sym.kind);
        stmt->bindText(4, sym.detail);
        stmt->bindInt(5, sym.line);
        stmt->bindInt(6, sym.character);
        stmt->bindInt(7, sym.endLine);
        stmt->bindInt(8, sym.endCharacter);
        if (sym.parentId > 0) {
            stmt->bindInt64(9, sym.parentId);
        } else {
            stmt->bindNull(9);
        }
        stmt->bindText(10, currentGitSha_);

        stmt->step();
    }

    // Update cache status
    auto statusStmt = db_->prepare(
        "INSERT OR REPLACE INTO file_cache_status (file_path, git_sha) VALUES (?, ?)"
    );
    if (statusStmt) {
        statusStmt->bindText(1, normalized);
        statusStmt->bindText(2, currentGitSha_);
        statusStmt->step();
    }

    db_->commit();
}

std::optional<std::vector<CachedDocumentSymbol>>
LSPCache::getCachedDocumentSymbols(const std::string& filePath) {
    if (!isOpen() || currentGitSha_.empty()) return std::nullopt;

    auto stmt = db_->prepare(
        "SELECT id, name, kind, detail, line, character, end_line, end_character, parent_id "
        "FROM document_symbols "
        "WHERE file_path = ? AND git_sha = ? "
        "ORDER BY line, character"
    );
    if (!stmt) return std::nullopt;

    stmt->bindText(1, normalizeFilePath(filePath));
    stmt->bindText(2, currentGitSha_);

    std::vector<CachedDocumentSymbol> results;
    while (stmt->step()) {
        CachedDocumentSymbol sym;
        sym.id = stmt->columnInt64(0);
        sym.filePath = filePath;
        sym.name = stmt->columnText(1);
        sym.kind = stmt->columnText(2);
        sym.detail = stmt->columnText(3);
        sym.line = stmt->columnInt(4);
        sym.character = stmt->columnInt(5);
        sym.endLine = stmt->columnInt(6);
        sym.endCharacter = stmt->columnInt(7);
        sym.parentId = stmt->columnIsNull(8) ? 0 : stmt->columnInt64(8);
        sym.gitSha = currentGitSha_;
        results.push_back(sym);
    }

    if (results.empty()) return std::nullopt;
    return results;
}

// ============================================================================
// Workspace symbols caching
// ============================================================================

void LSPCache::cacheWorkspaceSymbols(const std::string& query,
                                     const std::vector<CachedSymbol>& symbols) {
    if (!isOpen() || currentGitSha_.empty()) return;

    db_->beginTransaction();

    // Delete existing results for this query
    auto delStmt = db_->prepare(
        "DELETE FROM workspace_symbols WHERE query = ? AND git_sha = ?"
    );
    if (delStmt) {
        delStmt->bindText(1, query);
        delStmt->bindText(2, currentGitSha_);
        delStmt->step();
    }

    for (const auto& sym : symbols) {
        auto stmt = db_->prepare(
            "INSERT INTO workspace_symbols "
            "(query, file_path, name, kind, container_name, line, character, end_line, end_character, git_sha) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
        );
        if (!stmt) continue;

        stmt->bindText(1, query);
        stmt->bindText(2, normalizeFilePath(sym.filePath));
        stmt->bindText(3, sym.name);
        stmt->bindText(4, sym.kind);
        stmt->bindText(5, sym.containerName);
        stmt->bindInt(6, sym.line);
        stmt->bindInt(7, sym.character);
        stmt->bindInt(8, sym.endLine);
        stmt->bindInt(9, sym.endCharacter);
        stmt->bindText(10, currentGitSha_);

        stmt->step();
    }

    db_->commit();
}

std::optional<std::vector<CachedSymbol>>
LSPCache::getCachedWorkspaceSymbols(const std::string& query) {
    if (!isOpen() || currentGitSha_.empty()) return std::nullopt;

    auto stmt = db_->prepare(
        "SELECT file_path, name, kind, container_name, line, character, end_line, end_character "
        "FROM workspace_symbols "
        "WHERE query = ? AND git_sha = ?"
    );
    if (!stmt) return std::nullopt;

    stmt->bindText(1, query);
    stmt->bindText(2, currentGitSha_);

    std::vector<CachedSymbol> results;
    while (stmt->step()) {
        CachedSymbol sym;
        sym.filePath = stmt->columnText(0);
        sym.name = stmt->columnText(1);
        sym.kind = stmt->columnText(2);
        sym.containerName = stmt->columnText(3);
        sym.line = stmt->columnInt(4);
        sym.character = stmt->columnInt(5);
        sym.endLine = stmt->columnInt(6);
        sym.endCharacter = stmt->columnInt(7);
        sym.gitSha = currentGitSha_;
        results.push_back(sym);
    }

    if (results.empty()) return std::nullopt;
    return results;
}

// ============================================================================
// Folding Ranges Caching
// ============================================================================

void LSPCache::cacheFoldingRanges(const std::string& filePath,
                                    const std::vector<CachedFoldingRange>& ranges) {
    if (!isOpen() || currentGitSha_.empty()) return;

    std::string normalizedPath = normalizeFilePath(filePath);

    // Delete existing ranges for this file
    auto deleteStmt = db_->prepare(
        "DELETE FROM folding_ranges WHERE file_path = ? AND git_sha = ?"
    );
    if (deleteStmt) {
        deleteStmt->bindText(1, normalizedPath);
        deleteStmt->bindText(2, currentGitSha_);
        deleteStmt->step();
    }

    // Insert new ranges
    auto insertStmt = db_->prepare(
        "INSERT INTO folding_ranges (file_path, start_line, end_line, kind, git_sha) "
        "VALUES (?, ?, ?, ?, ?)"
    );
    if (!insertStmt) return;

    for (const auto& range : ranges) {
        insertStmt->reset();
        insertStmt->bindText(1, normalizedPath);
        insertStmt->bindInt(2, range.startLine);
        insertStmt->bindInt(3, range.endLine);
        insertStmt->bindText(4, range.kind);
        insertStmt->bindText(5, currentGitSha_);
        insertStmt->step();
    }
}

std::optional<std::vector<CachedFoldingRange>>
LSPCache::getCachedFoldingRanges(const std::string& filePath) {
    if (!isOpen() || currentGitSha_.empty()) return std::nullopt;

    std::string normalizedPath = normalizeFilePath(filePath);

    auto stmt = db_->prepare(
        "SELECT start_line, end_line, kind "
        "FROM folding_ranges "
        "WHERE file_path = ? AND git_sha = ? "
        "ORDER BY start_line"
    );
    if (!stmt) return std::nullopt;

    stmt->bindText(1, normalizedPath);
    stmt->bindText(2, currentGitSha_);

    std::vector<CachedFoldingRange> results;
    while (stmt->step()) {
        CachedFoldingRange range;
        range.filePath = filePath;
        range.startLine = stmt->columnInt(0);
        range.endLine = stmt->columnInt(1);
        range.kind = stmt->columnText(2);
        range.gitSha = currentGitSha_;
        results.push_back(range);
    }

    if (results.empty()) return std::nullopt;
    return results;
}

// ============================================================================
// Statistics & Maintenance
// ============================================================================

LSPCache::CacheStats LSPCache::getStats() const {
    CacheStats stats;
    if (!isOpen()) return stats;

    auto countQuery = [this](const char* table) -> int64_t {
        auto stmt = db_->prepare(std::string("SELECT COUNT(*) FROM ") + table);
        if (stmt && stmt->step()) {
            return stmt->columnInt64(0);
        }
        return 0;
    };

    stats.definitionCount = countQuery("definitions");
    stats.referenceCount = countQuery("references_");
    stats.hoverCount = countQuery("hovers");
    stats.symbolCount = countQuery("document_symbols") + countQuery("workspace_symbols");

    // Get database file size (approximate via page count)
    auto stmt = db_->prepare("PRAGMA page_count");
    int64_t pageCount = 0;
    if (stmt && stmt->step()) {
        pageCount = stmt->columnInt64(0);
    }

    stmt = db_->prepare("PRAGMA page_size");
    int64_t pageSize = 4096;
    if (stmt && stmt->step()) {
        pageSize = stmt->columnInt64(0);
    }

    stats.totalSize = pageCount * pageSize;

    return stats;
}

void LSPCache::vacuum() {
    if (!isOpen()) return;
    db_->execute("VACUUM");
}

void LSPCache::clearAll() {
    if (!isOpen()) return;

    db_->beginTransaction();
    db_->execute("DELETE FROM definitions");
    db_->execute("DELETE FROM references_");
    db_->execute("DELETE FROM hovers");
    db_->execute("DELETE FROM document_symbols");
    db_->execute("DELETE FROM workspace_symbols");
    db_->execute("DELETE FROM file_cache_status");
    db_->execute("DELETE FROM index_definitions");
    db_->execute("DELETE FROM index_references");
    db_->execute("DELETE FROM indexed_files");
    db_->commit();

    vacuum();
}

int64_t LSPCache::pruneOlderThan(int days) {
    if (!isOpen()) return 0;

    // Cutoff in SQLite's own clock so it is byte-consistent with how
    // created_at is generated (strftime('%s','now'), UTC unix secs).
    // days<=0 -> offset<=0 -> cutoff = now + |offset| (future) ->
    // every on-demand row (created_at <= now) is deleted == force
    // evict, matching DiffCache.pruneOlderThan(-1) semantics.
    const long long offset = static_cast<long long>(days) * 86400LL;

    // On-demand query cache ONLY. The durable index tables
    // (index_definitions/index_references/indexed_files) are
    // deliberately excluded -- they are regenerable and must not be
    // age-evicted (Phase-5 judgement call #3).
    const char* tables[] = {"definitions", "references_", "hovers",
                            "document_symbols", "workspace_symbols",
                            "folding_ranges"};
    int64_t removed = 0;
    db_->beginTransaction();
    for (const char* t : tables) {
        auto stmt = db_->prepare(
            "DELETE FROM " + std::string(t) +
            " WHERE created_at < (strftime('%s','now') - ?)");
        if (stmt) {
            stmt->bindInt64(1, offset);
            stmt->step();
            removed += db_->changesCount();
        }
    }
    // Expire stale per-file validity markers consistently so
    // isCacheValid() does not over-report for pruned files.
    if (auto s = db_->prepare(
            "DELETE FROM file_cache_status "
            "WHERE indexed_at < (strftime('%s','now') - ?)")) {
        s->bindInt64(1, offset);
        s->step();
        removed += db_->changesCount();
    }
    db_->commit();
    return removed;
}

// ============================================================================
// Full Index API Implementation
// ============================================================================

void LSPCache::storeIndexDefinition(const IndexedDefinition& def) {
    if (!isOpen() || currentGitSha_.empty()) return;

    auto stmt = db_->prepare(
        "INSERT INTO index_definitions "
        "(name, file_uri, kind, start_line, start_char, end_line, end_char, container_name, git_sha) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"
    );
    if (!stmt) return;

    stmt->bindText(1, def.name);
    stmt->bindText(2, normalizeFilePath(def.fileUri));
    stmt->bindInt(3, def.kind);
    stmt->bindInt(4, def.startLine);
    stmt->bindInt(5, def.startChar);
    stmt->bindInt(6, def.endLine);
    stmt->bindInt(7, def.endChar);
    stmt->bindText(8, def.containerName);
    stmt->bindText(9, currentGitSha_);

    stmt->step();
}

void LSPCache::storeIndexReference(const IndexedReference& ref) {
    if (!isOpen() || currentGitSha_.empty()) return;

    auto stmt = db_->prepare(
        "INSERT INTO index_references "
        "(symbol_name, from_file_uri, from_start_line, from_start_char, from_end_line, from_end_char, "
        "to_file_uri, to_start_line, to_start_char, to_end_line, to_end_char, git_sha) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
    );
    if (!stmt) return;

    stmt->bindText(1, ref.symbolName);
    stmt->bindText(2, normalizeFilePath(ref.fromFileUri));
    stmt->bindInt(3, ref.fromStartLine);
    stmt->bindInt(4, ref.fromStartChar);
    stmt->bindInt(5, ref.fromEndLine);
    stmt->bindInt(6, ref.fromEndChar);
    stmt->bindText(7, normalizeFilePath(ref.toFileUri));
    stmt->bindInt(8, ref.toStartLine);
    stmt->bindInt(9, ref.toStartChar);
    stmt->bindInt(10, ref.toEndLine);
    stmt->bindInt(11, ref.toEndChar);
    stmt->bindText(12, currentGitSha_);

    stmt->step();
}

void LSPCache::markFileIndexed(const std::string& fileUri) {
    if (!isOpen() || currentGitSha_.empty()) return;

    auto stmt = db_->prepare(
        "INSERT OR REPLACE INTO indexed_files (file_uri, git_sha) VALUES (?, ?)"
    );
    if (!stmt) return;

    stmt->bindText(1, normalizeFilePath(fileUri));
    stmt->bindText(2, currentGitSha_);
    stmt->step();
}

bool LSPCache::isFileIndexed(const std::string& fileUri) const {
    if (!isOpen() || currentGitSha_.empty()) return false;

    auto stmt = db_->prepare(
        "SELECT 1 FROM indexed_files WHERE file_uri = ? AND git_sha = ?"
    );
    if (!stmt) return false;

    stmt->bindText(1, normalizeFilePath(fileUri));
    stmt->bindText(2, currentGitSha_);

    return stmt->step();
}

std::vector<LSPCache::IndexedDefinition> LSPCache::getAllIndexDefinitions() const {
    std::vector<IndexedDefinition> results;
    if (!isOpen() || currentGitSha_.empty()) return results;

    auto stmt = db_->prepare(
        "SELECT id, name, file_uri, kind, start_line, start_char, end_line, end_char, container_name "
        "FROM index_definitions WHERE git_sha = ?"
    );
    if (!stmt) return results;

    stmt->bindText(1, currentGitSha_);

    while (stmt->step()) {
        IndexedDefinition def;
        def.id = stmt->columnInt64(0);
        def.name = stmt->columnText(1);
        def.fileUri = "file://" + stmt->columnText(2);
        def.kind = stmt->columnInt(3);
        def.startLine = stmt->columnInt(4);
        def.startChar = stmt->columnInt(5);
        def.endLine = stmt->columnInt(6);
        def.endChar = stmt->columnInt(7);
        def.containerName = stmt->columnText(8);
        def.gitSha = currentGitSha_;
        results.push_back(def);
    }

    return results;
}

std::vector<LSPCache::IndexedReference> LSPCache::getAllIndexReferences() const {
    std::vector<IndexedReference> results;
    if (!isOpen() || currentGitSha_.empty()) return results;

    auto stmt = db_->prepare(
        "SELECT id, symbol_name, from_file_uri, from_start_line, from_start_char, from_end_line, from_end_char, "
        "to_file_uri, to_start_line, to_start_char, to_end_line, to_end_char "
        "FROM index_references WHERE git_sha = ?"
    );
    if (!stmt) return results;

    stmt->bindText(1, currentGitSha_);

    while (stmt->step()) {
        IndexedReference ref;
        ref.id = stmt->columnInt64(0);
        ref.symbolName = stmt->columnText(1);
        ref.fromFileUri = "file://" + stmt->columnText(2);
        ref.fromStartLine = stmt->columnInt(3);
        ref.fromStartChar = stmt->columnInt(4);
        ref.fromEndLine = stmt->columnInt(5);
        ref.fromEndChar = stmt->columnInt(6);
        ref.toFileUri = "file://" + stmt->columnText(7);
        ref.toStartLine = stmt->columnInt(8);
        ref.toStartChar = stmt->columnInt(9);
        ref.toEndLine = stmt->columnInt(10);
        ref.toEndChar = stmt->columnInt(11);
        ref.gitSha = currentGitSha_;
        results.push_back(ref);
    }

    return results;
}

std::vector<LSPCache::IndexedDefinition> LSPCache::getIndexDefinitionsForFile(const std::string& fileUri) const {
    std::vector<IndexedDefinition> results;
    if (!isOpen() || currentGitSha_.empty()) return results;

    auto stmt = db_->prepare(
        "SELECT id, name, file_uri, kind, start_line, start_char, end_line, end_char, container_name "
        "FROM index_definitions WHERE file_uri = ? AND git_sha = ?"
    );
    if (!stmt) return results;

    stmt->bindText(1, normalizeFilePath(fileUri));
    stmt->bindText(2, currentGitSha_);

    while (stmt->step()) {
        IndexedDefinition def;
        def.id = stmt->columnInt64(0);
        def.name = stmt->columnText(1);
        def.fileUri = "file://" + stmt->columnText(2);
        def.kind = stmt->columnInt(3);
        def.startLine = stmt->columnInt(4);
        def.startChar = stmt->columnInt(5);
        def.endLine = stmt->columnInt(6);
        def.endChar = stmt->columnInt(7);
        def.containerName = stmt->columnText(8);
        def.gitSha = currentGitSha_;
        results.push_back(def);
    }

    return results;
}

std::vector<LSPCache::IndexedReference> LSPCache::getIndexReferencesFromFile(const std::string& fileUri) const {
    std::vector<IndexedReference> results;
    if (!isOpen() || currentGitSha_.empty()) return results;

    auto stmt = db_->prepare(
        "SELECT id, symbol_name, from_file_uri, from_start_line, from_start_char, from_end_line, from_end_char, "
        "to_file_uri, to_start_line, to_start_char, to_end_line, to_end_char "
        "FROM index_references WHERE from_file_uri = ? AND git_sha = ?"
    );
    if (!stmt) return results;

    stmt->bindText(1, normalizeFilePath(fileUri));
    stmt->bindText(2, currentGitSha_);

    while (stmt->step()) {
        IndexedReference ref;
        ref.id = stmt->columnInt64(0);
        ref.symbolName = stmt->columnText(1);
        ref.fromFileUri = "file://" + stmt->columnText(2);
        ref.fromStartLine = stmt->columnInt(3);
        ref.fromStartChar = stmt->columnInt(4);
        ref.fromEndLine = stmt->columnInt(5);
        ref.fromEndChar = stmt->columnInt(6);
        ref.toFileUri = "file://" + stmt->columnText(7);
        ref.toStartLine = stmt->columnInt(8);
        ref.toStartChar = stmt->columnInt(9);
        ref.toEndLine = stmt->columnInt(10);
        ref.toEndChar = stmt->columnInt(11);
        ref.gitSha = currentGitSha_;
        results.push_back(ref);
    }

    return results;
}

std::vector<LSPCache::IndexedReference> LSPCache::getIndexReferencesToFile(const std::string& fileUri) const {
    std::vector<IndexedReference> results;
    if (!isOpen() || currentGitSha_.empty()) return results;

    auto stmt = db_->prepare(
        "SELECT id, symbol_name, from_file_uri, from_start_line, from_start_char, from_end_line, from_end_char, "
        "to_file_uri, to_start_line, to_start_char, to_end_line, to_end_char "
        "FROM index_references WHERE to_file_uri = ? AND git_sha = ?"
    );
    if (!stmt) return results;

    stmt->bindText(1, normalizeFilePath(fileUri));
    stmt->bindText(2, currentGitSha_);

    while (stmt->step()) {
        IndexedReference ref;
        ref.id = stmt->columnInt64(0);
        ref.symbolName = stmt->columnText(1);
        ref.fromFileUri = "file://" + stmt->columnText(2);
        ref.fromStartLine = stmt->columnInt(3);
        ref.fromStartChar = stmt->columnInt(4);
        ref.fromEndLine = stmt->columnInt(5);
        ref.fromEndChar = stmt->columnInt(6);
        ref.toFileUri = "file://" + stmt->columnText(7);
        ref.toStartLine = stmt->columnInt(8);
        ref.toStartChar = stmt->columnInt(9);
        ref.toEndLine = stmt->columnInt(10);
        ref.toEndChar = stmt->columnInt(11);
        ref.gitSha = currentGitSha_;
        results.push_back(ref);
    }

    return results;
}

void LSPCache::clearIndex() {
    if (!isOpen()) return;

    db_->beginTransaction();
    db_->execute("DELETE FROM index_definitions");
    db_->execute("DELETE FROM index_references");
    db_->execute("DELETE FROM indexed_files");
    db_->commit();
}

LSPCache::IndexStats LSPCache::getIndexStats() const {
    IndexStats stats;
    if (!isOpen() || currentGitSha_.empty()) return stats;

    auto countQuery = [this](const char* table) -> int64_t {
        std::string sql = std::string("SELECT COUNT(*) FROM ") + table + " WHERE git_sha = ?";
        auto stmt = db_->prepare(sql);
        if (stmt) {
            stmt->bindText(1, currentGitSha_);
            if (stmt->step()) {
                return stmt->columnInt64(0);
            }
        }
        return 0;
    };

    stats.definitionCount = countQuery("index_definitions");
    stats.referenceCount = countQuery("index_references");

    auto filesStmt = db_->prepare("SELECT COUNT(*) FROM indexed_files WHERE git_sha = ?");
    if (filesStmt) {
        filesStmt->bindText(1, currentGitSha_);
        if (filesStmt->step()) {
            stats.filesIndexed = filesStmt->columnInt64(0);
        }
    }

    return stats;
}

} // namespace db
} // namespace gitreview
