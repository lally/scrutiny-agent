// LSPCache.hpp
// SQLite-based cache for LSP query results

#ifndef LSP_CACHE_HPP
#define LSP_CACHE_HPP

#include "Database.hpp"
#include "lsp/LSPTypes.hpp"

#include <string>
#include <vector>
#include <optional>
#include <memory>

namespace gitreview {
namespace db {

// Represents a symbol in the database
struct CachedSymbol {
    int64_t id = 0;
    std::string filePath;
    std::string name;
    std::string kind;           // function, class, variable, etc.
    std::string containerName;  // enclosing class/namespace
    int line = 0;
    int character = 0;
    int endLine = 0;
    int endCharacter = 0;
    std::string gitSha;
};

// Represents a definition lookup result
struct CachedDefinition {
    int64_t id = 0;
    std::string sourceFile;
    int sourceLine = 0;
    int sourceCharacter = 0;
    std::string targetFile;
    int targetLine = 0;
    int targetCharacter = 0;
    int targetEndLine = 0;
    int targetEndCharacter = 0;
    std::string gitSha;
};

// Represents a reference lookup result
struct CachedReference {
    int64_t id = 0;
    std::string symbolFile;
    int symbolLine = 0;
    int symbolCharacter = 0;
    std::string referenceFile;
    int referenceLine = 0;
    int referenceCharacter = 0;
    int referenceEndLine = 0;
    int referenceEndCharacter = 0;
    std::string gitSha;
};

// Represents hover information
struct CachedHover {
    int64_t id = 0;
    std::string filePath;
    int line = 0;
    int character = 0;
    std::string contents;       // Markdown content
    std::string gitSha;
};

// Represents a document symbol (outline)
struct CachedDocumentSymbol {
    int64_t id = 0;
    std::string filePath;
    std::string name;
    std::string kind;
    std::string detail;
    int line = 0;
    int character = 0;
    int endLine = 0;
    int endCharacter = 0;
    int64_t parentId = 0;       // For nested symbols
    std::string gitSha;
};

// Represents a folding range
struct CachedFoldingRange {
    int64_t id = 0;
    std::string filePath;
    int startLine = 0;
    int endLine = 0;
    std::string kind;           // comment, imports, region, etc.
    std::string gitSha;
};

class LSPCache {
public:
    // Opens or creates a cache database at the given path
    explicit LSPCache(const std::string& dbPath);
    ~LSPCache();

    // Non-copyable
    LSPCache(const LSPCache&) = delete;
    LSPCache& operator=(const LSPCache&) = delete;

    bool isOpen() const;

    // Git SHA management
    void setCurrentGitSha(const std::string& sha);
    std::string currentGitSha() const { return currentGitSha_; }

    // Check if cache is valid for a file at current SHA
    bool isCacheValid(const std::string& filePath) const;

    // Invalidate cache for files that have changed
    void invalidateFile(const std::string& filePath);
    void invalidateAllForSha(const std::string& sha);

    // Definition caching
    void cacheDefinition(const std::string& sourceFile,
                         int sourceLine, int sourceChar,
                         const lsp::LSPLocation& target);

    std::optional<std::vector<lsp::LSPLocation>>
    getCachedDefinitions(const std::string& sourceFile,
                         int sourceLine, int sourceChar);

    // Reference caching
    void cacheReferences(const std::string& symbolFile,
                         int symbolLine, int symbolChar,
                         const std::vector<lsp::LSPLocation>& references);

    std::optional<std::vector<lsp::LSPLocation>>
    getCachedReferences(const std::string& symbolFile,
                        int symbolLine, int symbolChar);

    // Hover caching
    void cacheHover(const std::string& filePath,
                    int line, int character,
                    const std::string& contents);

    std::optional<std::string>
    getCachedHover(const std::string& filePath,
                   int line, int character);

    // Document symbols caching (outline)
    void cacheDocumentSymbols(const std::string& filePath,
                              const std::vector<CachedDocumentSymbol>& symbols);

    std::optional<std::vector<CachedDocumentSymbol>>
    getCachedDocumentSymbols(const std::string& filePath);

    // Workspace symbols caching
    void cacheWorkspaceSymbols(const std::string& query,
                               const std::vector<CachedSymbol>& symbols);

    std::optional<std::vector<CachedSymbol>>
    getCachedWorkspaceSymbols(const std::string& query);

    // Folding ranges caching
    void cacheFoldingRanges(const std::string& filePath,
                            const std::vector<CachedFoldingRange>& ranges);

    std::optional<std::vector<CachedFoldingRange>>
    getCachedFoldingRanges(const std::string& filePath);

    // ========================================================================
    // Full Index API - for storing complete codebase index
    // ========================================================================

    // Index definition: a symbol definition in the codebase
    struct IndexedDefinition {
        int64_t id = 0;
        std::string name;
        std::string fileUri;
        int kind = 0;  // GRCSymbolKind value
        int startLine = 0;
        int startChar = 0;
        int endLine = 0;
        int endChar = 0;
        std::string containerName;
        std::string gitSha;
    };

    // Index reference: a reference from one location to another
    struct IndexedReference {
        int64_t id = 0;
        std::string symbolName;
        std::string fromFileUri;
        int fromStartLine = 0;
        int fromStartChar = 0;
        int fromEndLine = 0;
        int fromEndChar = 0;
        std::string toFileUri;
        int toStartLine = 0;
        int toStartChar = 0;
        int toEndLine = 0;
        int toEndChar = 0;
        std::string gitSha;
    };

    // Store an indexed definition
    void storeIndexDefinition(const IndexedDefinition& def);

    // Store an indexed reference
    void storeIndexReference(const IndexedReference& ref);

    // Mark a file as fully indexed
    void markFileIndexed(const std::string& fileUri);

    // Check if a file has been fully indexed
    bool isFileIndexed(const std::string& fileUri) const;

    // Get all definitions in the index
    std::vector<IndexedDefinition> getAllIndexDefinitions() const;

    // Get all references in the index
    std::vector<IndexedReference> getAllIndexReferences() const;

    // Get definitions for a specific file
    std::vector<IndexedDefinition> getIndexDefinitionsForFile(const std::string& fileUri) const;

    // Get references from a specific file
    std::vector<IndexedReference> getIndexReferencesFromFile(const std::string& fileUri) const;

    // Get references pointing to a specific file
    std::vector<IndexedReference> getIndexReferencesToFile(const std::string& fileUri) const;

    // Clear all index data (but not the on-demand cache)
    void clearIndex();

    // Get index statistics
    struct IndexStats {
        int64_t definitionCount = 0;
        int64_t referenceCount = 0;
        int64_t filesIndexed = 0;
    };
    IndexStats getIndexStats() const;

    // Statistics
    struct CacheStats {
        int64_t definitionCount = 0;
        int64_t referenceCount = 0;
        int64_t hoverCount = 0;
        int64_t symbolCount = 0;
        int64_t totalSize = 0;  // bytes
    };
    CacheStats getStats() const;

    // Maintenance
    void vacuum();
    void clearAll();

    // TTL hygiene: delete on-demand query-cache entries whose
    // created_at is older than `days` days. The durable index tables
    // (index_definitions/index_references/indexed_files) are NOT
    // touched -- the index is regenerable and age-evicting it would
    // silently shrink a built index (Phase-5 judgement call #3).
    // `days` <= 0 prunes all on-demand entries (force-evict, mirrors
    // DiffCache.pruneOlderThan(-1)). Returns rows removed.
    int64_t pruneOlderThan(int days);

private:
    void initializeSchema();
    std::string normalizeFilePath(const std::string& path) const;

    std::unique_ptr<Database> db_;
    std::string currentGitSha_;
};

} // namespace db
} // namespace gitreview

#endif // LSP_CACHE_HPP
