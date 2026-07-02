// GitReviewCore.cpp
// C API implementation for the Git Review core library
// Uses C++20 features with smart pointers and containers

#include "GitReviewCore.h"
#include "lsp/LSPClient.hpp"
#include "db/LSPCache.hpp"
#include "indexer/Indexer.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <cstring>
#include <memory>
#include <vector>
#include <string>
#include <unistd.h>  // for access()
#include <cstdlib>   // for getenv()

// Use fully qualified names to avoid collisions
namespace impl_lsp = gitreview::lsp;
namespace impl_db = gitreview::db;
namespace impl_indexer = gitreview::indexer;

// ============================================================================
// Logging initialization
// ============================================================================

namespace {
    struct LogInitializer {
        LogInitializer() {
            try {
                // Set up file logging
                auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                    "/tmp/gitreviewcore.log", true);
                auto logger = std::make_shared<spdlog::logger>("gitreview", file_sink);
                logger->set_level(spdlog::level::debug);
                logger->flush_on(spdlog::level::debug);
                spdlog::set_default_logger(logger);
                spdlog::info("[GitReviewCore] Logging initialized");
            } catch (const std::exception& e) {
                // Fall back to stderr
                spdlog::set_level(spdlog::level::debug);
            }
        }
    };
    static LogInitializer logInit;
}

// ============================================================================
// Handle Helpers
// ============================================================================

static impl_lsp::LSPClient* getClient(GRCLSPClient* handle) {
    return handle ? static_cast<impl_lsp::LSPClient*>(handle->_opaque) : nullptr;
}

static impl_db::LSPCache* getCache(GRCLSPCache* handle) {
    return handle ? static_cast<impl_db::LSPCache*>(handle->_opaque) : nullptr;
}

static impl_indexer::Indexer* getIndexer(GRCIndexer* handle) {
    return handle ? static_cast<impl_indexer::Indexer*>(handle->_opaque) : nullptr;
}

// Version
static constexpr const char* VERSION = "0.1.0";

// ============================================================================
// Helper conversions
// ============================================================================

static impl_lsp::Language toLanguage(GRCLanguage lang) {
    switch (lang) {
        case GRC_LANG_RUST:       return impl_lsp::Language::Rust;
        case GRC_LANG_PYTHON:     return impl_lsp::Language::Python;
        case GRC_LANG_JAVASCRIPT: return impl_lsp::Language::JavaScript;
        case GRC_LANG_TYPESCRIPT: return impl_lsp::Language::TypeScript;
        case GRC_LANG_GO:         return impl_lsp::Language::Go;
        case GRC_LANG_CPP:        return impl_lsp::Language::Cpp;
        case GRC_LANG_C:          return impl_lsp::Language::C;
        case GRC_LANG_SWIFT:      return impl_lsp::Language::Swift;
        default:                  return impl_lsp::Language::Unknown;
    }
}

static const char* languageName(GRCLanguage lang) {
    switch (lang) {
        case GRC_LANG_RUST:       return "Rust";
        case GRC_LANG_PYTHON:     return "Python";
        case GRC_LANG_JAVASCRIPT: return "JavaScript";
        case GRC_LANG_TYPESCRIPT: return "TypeScript";
        case GRC_LANG_GO:         return "Go";
        case GRC_LANG_CPP:        return "C++";
        case GRC_LANG_C:          return "C";
        case GRC_LANG_SWIFT:      return "Swift";
        default:                  return "Unknown";
    }
}

static GRCError toGRCError(impl_lsp::LSPError error) {
    if (!error) return GRC_SUCCESS;

    switch (error.code) {
        case impl_lsp::LSPErrorCode::ServerNotInitialized:
            return GRC_ERROR_NOT_INITIALIZED;
        case impl_lsp::LSPErrorCode::MethodNotFound:
            return GRC_ERROR_NO_LANGUAGE_SERVER;
        case impl_lsp::LSPErrorCode::InternalError:
            return GRC_ERROR_LSP_ERROR;
        default:
            return GRC_ERROR_LSP_ERROR;
    }
}

static impl_lsp::LSPPosition toLSPPosition(GRCPosition pos) {
    return impl_lsp::LSPPosition{pos.line, pos.character};
}

static GRCPosition toGRCPosition(impl_lsp::LSPPosition pos) {
    return GRCPosition{pos.line, pos.character};
}

static GRCRange toGRCRange(impl_lsp::LSPRange range) {
    return GRCRange{
        toGRCPosition(range.start),
        toGRCPosition(range.end)
    };
}

static GRCSymbolKind toGRCSymbolKind(impl_lsp::SymbolKind kind) {
    return static_cast<GRCSymbolKind>(static_cast<int>(kind));
}

static char* duplicateString(const std::string& str) {
    auto result = std::make_unique<char[]>(str.size() + 1);
    std::memcpy(result.get(), str.c_str(), str.size() + 1);
    return result.release();
}

// Helper to convert location arrays
static bool convertLocations(const std::vector<impl_lsp::LSPLocation>& locations,
                             GRCLocationArray* out) {
    out->count = static_cast<int32_t>(locations.size());
    if (locations.empty()) {
        out->locations = nullptr;
        return true;
    }

    out->locations = new GRCLocation[locations.size()];
    for (size_t i = 0; i < locations.size(); ++i) {
        out->locations[i].uri = duplicateString(locations[i].uri);
        out->locations[i].range = toGRCRange(locations[i].range);
    }
    return true;
}

// Helper to convert document symbols recursively
static GRCDocumentSymbol convertDocumentSymbol(const impl_lsp::LSPDocumentSymbol& sym);

static bool convertDocumentSymbols(const std::vector<impl_lsp::LSPDocumentSymbol>& symbols,
                                   GRCDocumentSymbolArray* out) {
    out->count = static_cast<int32_t>(symbols.size());
    if (symbols.empty()) {
        out->symbols = nullptr;
        return true;
    }

    out->symbols = new GRCDocumentSymbol[symbols.size()];
    for (size_t i = 0; i < symbols.size(); ++i) {
        out->symbols[i] = convertDocumentSymbol(symbols[i]);
    }
    return true;
}

static GRCDocumentSymbol convertDocumentSymbol(const impl_lsp::LSPDocumentSymbol& sym) {
    GRCDocumentSymbol result{};
    result.name = duplicateString(sym.name);
    result.detail = duplicateString(sym.detail);
    result.kind = toGRCSymbolKind(sym.kind);
    result.range = toGRCRange(sym.range);
    result.selection_range = toGRCRange(sym.selectionRange);

    if (sym.children.empty()) {
        result.children = nullptr;
        result.children_count = 0;
    } else {
        result.children_count = static_cast<int32_t>(sym.children.size());
        result.children = new GRCDocumentSymbol[sym.children.size()];
        for (size_t i = 0; i < sym.children.size(); ++i) {
            result.children[i] = convertDocumentSymbol(sym.children[i]);
        }
    }
    return result;
}

static bool convertSymbolInformation(const std::vector<impl_lsp::LSPSymbolInformation>& symbols,
                                     GRCSymbolInformationArray* out) {
    out->count = static_cast<int32_t>(symbols.size());
    if (symbols.empty()) {
        out->symbols = nullptr;
        return true;
    }

    out->symbols = new GRCSymbolInformation[symbols.size()];
    for (size_t i = 0; i < symbols.size(); ++i) {
        out->symbols[i].name = duplicateString(symbols[i].name);
        out->symbols[i].kind = toGRCSymbolKind(symbols[i].kind);
        out->symbols[i].location.uri = duplicateString(symbols[i].location.uri);
        out->symbols[i].location.range = toGRCRange(symbols[i].location.range);
        out->symbols[i].container_name = duplicateString(symbols[i].containerName);
    }
    return true;
}

// ============================================================================
// LSP Cache API
// ============================================================================

extern "C" {

GRCLSPCache* grc_cache_create(const char* db_path, GRCError* out_error) {
    spdlog::info("[GRC] grc_cache_create called with path: {}", db_path ? db_path : "(null)");

    if (!db_path) {
        spdlog::error("[GRC] db_path is null");
        if (out_error) *out_error = GRC_ERROR_INVALID_ARGUMENT;
        return nullptr;
    }

    try {
        auto cache = std::make_unique<impl_db::LSPCache>(db_path);
        if (!cache->isOpen()) {
            spdlog::error("[GRC] Failed to open database at {}", db_path);
            if (out_error) *out_error = GRC_ERROR_DATABASE_ERROR;
            return nullptr;
        }

        auto* handle = new GRCLSPCache;
        handle->_opaque = cache.release();

        spdlog::info("[GRC] Cache created successfully");
        if (out_error) *out_error = GRC_SUCCESS;
        return handle;
    } catch (const std::exception& e) {
        spdlog::info("[GRC] EXCEPTION in grc_cache_create: {}", e.what());
        if (out_error) *out_error = GRC_ERROR_ALLOCATION_FAILED;
        return nullptr;
    }
}

void grc_cache_destroy(GRCLSPCache* cache) {
    spdlog::info("[GRC] grc_cache_destroy called");

    if (cache) {
        delete getCache(cache);
        delete cache;
        spdlog::info("[GRC] Cache destroyed");
    }
}

void grc_cache_set_git_sha(GRCLSPCache* cache, const char* sha) {
    if (auto* impl = getCache(cache)) {
        spdlog::info("[GRC] Setting git SHA: {}", sha ? sha : "(null)");
        impl->setCurrentGitSha(sha ? sha : "");
    }
}

bool grc_cache_is_valid(GRCLSPCache* cache, const char* file_path) {
    if (auto* impl = getCache(cache)) {
        return impl->isCacheValid(file_path ? file_path : "");
    }
    return false;
}

void grc_cache_invalidate_file(GRCLSPCache* cache, const char* file_path) {
    if (auto* impl = getCache(cache)) {
        impl->invalidateFile(file_path ? file_path : "");
    }
}

void grc_cache_clear_all(GRCLSPCache* cache) {
    if (auto* impl = getCache(cache)) {
        spdlog::info("[GRC] Clearing all cache data");
        impl->clearAll();
    }
}

GRCCacheStats grc_cache_get_stats(GRCLSPCache* cache) {
    GRCCacheStats stats = {0, 0, 0, 0, 0};
    if (auto* impl = getCache(cache)) {
        auto cppStats = impl->getStats();
        stats.definition_count = cppStats.definitionCount;
        stats.reference_count = cppStats.referenceCount;
        stats.hover_count = cppStats.hoverCount;
        stats.symbol_count = cppStats.symbolCount;
        stats.total_size = cppStats.totalSize;
    }
    return stats;
}

void grc_cache_vacuum(GRCLSPCache* cache) {
    if (auto* impl = getCache(cache)) {
        spdlog::info("[GRC] Vacuuming cache database");
        impl->vacuum();
    }
}

int64_t grc_cache_prune(GRCLSPCache* cache, int days) {
    if (auto* impl = getCache(cache)) {
        spdlog::info("[GRC] Pruning on-demand cache entries older than {} days", days);
        return impl->pruneOlderThan(days);
    }
    return 0;
}

// ============================================================================
// LSP Client API
// ============================================================================

GRCLSPClient* grc_lsp_client_create(
    const char* workspace_path,
    GRCLanguage language,
    GRCError* out_error
) {
    spdlog::info("[GRC] grc_lsp_client_create called: workspace={}, language={}",
                workspace_path ? workspace_path : "(null)", languageName(language));

    if (!workspace_path) {
        spdlog::error("[GRC] workspace_path is null");
        if (out_error) *out_error = GRC_ERROR_INVALID_ARGUMENT;
        return nullptr;
    }

    try {
        auto client = std::make_unique<impl_lsp::LSPClient>(workspace_path, toLanguage(language));

        auto* handle = new GRCLSPClient;
        handle->_opaque = client.release();

        spdlog::info("[GRC] LSP client created");
        if (out_error) *out_error = GRC_SUCCESS;
        return handle;
    } catch (const std::exception& e) {
        spdlog::info("[GRC] EXCEPTION in grc_lsp_client_create: {}", e.what());
        if (out_error) *out_error = GRC_ERROR_ALLOCATION_FAILED;
        return nullptr;
    }
}

void grc_lsp_client_destroy(GRCLSPClient* client) {
    spdlog::info("[GRC] grc_lsp_client_destroy called");

    if (client) {
        if (auto* impl = getClient(client)) {
            spdlog::info("[GRC] Stopping LSP client before destroy");
            impl->stop();
            delete impl;
        }
        delete client;
        spdlog::info("[GRC] LSP client destroyed");
    }
}

void grc_lsp_client_set_cache(GRCLSPClient* client, GRCLSPCache* cache) {
    auto* clientImpl = getClient(client);
    auto* cacheImpl = getCache(cache);

    if (clientImpl && cacheImpl) {
        spdlog::info("[GRC] Attaching cache to LSP client");
        // Create a shared_ptr that doesn't own the cache (caller manages lifetime)
        clientImpl->setCache(std::shared_ptr<impl_db::LSPCache>(cacheImpl, [](impl_db::LSPCache*){}));
    }
}

void grc_lsp_client_set_git_sha(GRCLSPClient* client, const char* sha) {
    if (auto* impl = getClient(client)) {
        spdlog::info("[GRC] Setting client git SHA: {}", sha ? sha : "(null)");
        impl->setGitSha(sha ? sha : "");
    }
}

GRCError grc_lsp_client_start(GRCLSPClient* client) {
    spdlog::info("[GRC] grc_lsp_client_start called");

    auto* impl = getClient(client);
    if (!impl) {
        spdlog::error("[GRC] Invalid client handle");
        return GRC_ERROR_INVALID_ARGUMENT;
    }

    spdlog::info("[GRC] Starting LSP server process...");
    auto error = impl->start();

    if (error) {
        spdlog::error("[GRC] LSP start failed with code {}", static_cast<int>(error.code));
        return toGRCError(error);
    }

    spdlog::info("[GRC] LSP server started successfully");
    return GRC_SUCCESS;
}

void grc_lsp_client_stop(GRCLSPClient* client) {
    spdlog::info("[GRC] grc_lsp_client_stop called");

    if (auto* impl = getClient(client)) {
        impl->stop();
        spdlog::info("[GRC] LSP server stopped");
    }
}

bool grc_lsp_client_is_ready(GRCLSPClient* client) {
    if (auto* impl = getClient(client)) {
        bool ready = impl->isReady();
        spdlog::info("[GRC] LSP client is_ready: {}", ready);
        return ready;
    }
    return false;
}

// ============================================================================
// Document Management
// ============================================================================

GRCError grc_lsp_client_did_open(
    GRCLSPClient* client,
    const char* file_uri,
    const char* language_id,
    int32_t version,
    const char* content
) {
    spdlog::info("[GRC] grc_lsp_client_did_open: uri={}, lang={}, version={}",
                file_uri ? file_uri : "(null)",
                language_id ? language_id : "(null)",
                version);

    auto* impl = getClient(client);
    if (!impl || !file_uri || !language_id || !content) {
        spdlog::error("[GRC] Invalid arguments to did_open");
        return GRC_ERROR_INVALID_ARGUMENT;
    }

    auto error = impl->didOpen(file_uri, language_id, version, content);
    if (error) {
        spdlog::error("[GRC] did_open failed with code {}", static_cast<int>(error.code));
    }
    return toGRCError(error);
}

GRCError grc_lsp_client_did_close(GRCLSPClient* client, const char* file_uri) {
    spdlog::info("[GRC] grc_lsp_client_did_close: uri={}", file_uri ? file_uri : "(null)");

    auto* impl = getClient(client);
    if (!impl || !file_uri) {
        return GRC_ERROR_INVALID_ARGUMENT;
    }
    return toGRCError(impl->didClose(file_uri));
}

GRCError grc_lsp_client_did_change(
    GRCLSPClient* client,
    const char* file_uri,
    int32_t version,
    const char* content
) {
    auto* impl = getClient(client);
    if (!impl || !file_uri || !content) {
        return GRC_ERROR_INVALID_ARGUMENT;
    }
    return toGRCError(impl->didChange(file_uri, version, content));
}

// ============================================================================
// LSP Operations (Synchronous)
// ============================================================================

GRCError grc_lsp_client_goto_definition(
    GRCLSPClient* client,
    const char* file_uri,
    GRCPosition position,
    GRCLocationArray* out_locations
) {
    spdlog::info("[GRC] grc_lsp_client_goto_definition: uri={}, line={}, char={}",
                file_uri ? file_uri : "(null)", position.line, position.character);

    auto* impl = getClient(client);
    if (!impl || !file_uri || !out_locations) {
        spdlog::error("[GRC] Invalid arguments");
        return GRC_ERROR_INVALID_ARGUMENT;
    }

    std::vector<impl_lsp::LSPLocation> locations;
    auto error = impl->gotoDefinition(file_uri, toLSPPosition(position), locations);

    if (error) {
        spdlog::error("[GRC] gotoDefinition failed with code {}", static_cast<int>(error.code));
        return toGRCError(error);
    }

    spdlog::info("[GRC] gotoDefinition returned {} locations", locations.size());
    convertLocations(locations, out_locations);
    return GRC_SUCCESS;
}

GRCError grc_lsp_client_find_references(
    GRCLSPClient* client,
    const char* file_uri,
    GRCPosition position,
    bool include_declaration,
    GRCLocationArray* out_locations
) {
    auto* impl = getClient(client);
    if (!impl || !file_uri || !out_locations) {
        return GRC_ERROR_INVALID_ARGUMENT;
    }

    std::vector<impl_lsp::LSPLocation> locations;
    auto error = impl->findReferences(file_uri, toLSPPosition(position), include_declaration, locations);
    if (error) return toGRCError(error);

    convertLocations(locations, out_locations);
    return GRC_SUCCESS;
}

GRCError grc_lsp_client_hover(
    GRCLSPClient* client,
    const char* file_uri,
    GRCPosition position,
    GRCHover* out_hover
) {
    auto* impl = getClient(client);
    if (!impl || !file_uri || !out_hover) {
        return GRC_ERROR_INVALID_ARGUMENT;
    }

    std::optional<impl_lsp::LSPHover> hover;
    auto error = impl->hover(file_uri, toLSPPosition(position), hover);
    if (error) return toGRCError(error);

    if (hover) {
        out_hover->contents = duplicateString(hover->contents);
        out_hover->has_range = hover->range.has_value();
        if (hover->range) {
            out_hover->range = toGRCRange(*hover->range);
        }
    } else {
        out_hover->contents = nullptr;
        out_hover->has_range = false;
    }
    return GRC_SUCCESS;
}

GRCError grc_lsp_client_document_symbols(
    GRCLSPClient* client,
    const char* file_uri,
    GRCDocumentSymbolArray* out_symbols
) {
    auto* impl = getClient(client);
    if (!impl || !file_uri || !out_symbols) {
        return GRC_ERROR_INVALID_ARGUMENT;
    }

    std::vector<impl_lsp::LSPDocumentSymbol> symbols;
    auto error = impl->documentSymbols(file_uri, symbols);
    if (error) return toGRCError(error);

    convertDocumentSymbols(symbols, out_symbols);
    return GRC_SUCCESS;
}

GRCError grc_lsp_client_workspace_symbols(
    GRCLSPClient* client,
    const char* query,
    GRCSymbolInformationArray* out_symbols
) {
    auto* impl = getClient(client);
    if (!impl || !query || !out_symbols) {
        return GRC_ERROR_INVALID_ARGUMENT;
    }

    std::vector<impl_lsp::LSPSymbolInformation> symbols;
    auto error = impl->workspaceSymbols(query, symbols);
    if (error) return toGRCError(error);

    convertSymbolInformation(symbols, out_symbols);
    return GRC_SUCCESS;
}

// ============================================================================
// LSP Operations (Asynchronous)
// ============================================================================

void grc_lsp_client_goto_definition_async(
    GRCLSPClient* client,
    const char* file_uri,
    GRCPosition position,
    GRCLocationCallback callback,
    void* context
) {
    auto* impl = getClient(client);
    if (!impl || !file_uri || !callback) {
        if (callback) callback(GRC_ERROR_INVALID_ARGUMENT, nullptr, context);
        return;
    }

    impl->gotoDefinitionAsync(
        file_uri,
        toLSPPosition(position),
        [callback, context](impl_lsp::LSPError error, std::vector<impl_lsp::LSPLocation> locations) {
            if (error) {
                callback(toGRCError(error), nullptr, context);
                return;
            }

            auto result = std::make_unique<GRCLocationArray>();
            convertLocations(locations, result.get());
            callback(GRC_SUCCESS, result.release(), context);
        }
    );
}

void grc_lsp_client_find_references_async(
    GRCLSPClient* client,
    const char* file_uri,
    GRCPosition position,
    bool include_declaration,
    GRCLocationCallback callback,
    void* context
) {
    auto* impl = getClient(client);
    if (!impl || !file_uri || !callback) {
        if (callback) callback(GRC_ERROR_INVALID_ARGUMENT, nullptr, context);
        return;
    }

    impl->findReferencesAsync(
        file_uri,
        toLSPPosition(position),
        include_declaration,
        [callback, context](impl_lsp::LSPError error, std::vector<impl_lsp::LSPLocation> locations) {
            if (error) {
                callback(toGRCError(error), nullptr, context);
                return;
            }

            auto result = std::make_unique<GRCLocationArray>();
            convertLocations(locations, result.get());
            callback(GRC_SUCCESS, result.release(), context);
        }
    );
}

void grc_lsp_client_hover_async(
    GRCLSPClient* client,
    const char* file_uri,
    GRCPosition position,
    GRCHoverCallback callback,
    void* context
) {
    auto* impl = getClient(client);
    if (!impl || !file_uri || !callback) {
        if (callback) callback(GRC_ERROR_INVALID_ARGUMENT, nullptr, context);
        return;
    }

    impl->hoverAsync(
        file_uri,
        toLSPPosition(position),
        [callback, context](impl_lsp::LSPError error, std::optional<impl_lsp::LSPHover> hover) {
            if (error) {
                callback(toGRCError(error), nullptr, context);
                return;
            }

            auto result = std::make_unique<GRCHover>();
            if (hover) {
                result->contents = duplicateString(hover->contents);
                result->has_range = hover->range.has_value();
                if (hover->range) {
                    result->range = toGRCRange(*hover->range);
                }
            } else {
                result->contents = nullptr;
                result->has_range = false;
            }
            callback(GRC_SUCCESS, result.release(), context);
        }
    );
}

void grc_lsp_client_document_symbols_async(
    GRCLSPClient* client,
    const char* file_uri,
    GRCDocumentSymbolCallback callback,
    void* context
) {
    auto* impl = getClient(client);
    if (!impl || !file_uri || !callback) {
        if (callback) callback(GRC_ERROR_INVALID_ARGUMENT, nullptr, context);
        return;
    }

    impl->documentSymbolsAsync(
        file_uri,
        [callback, context](impl_lsp::LSPError error, std::vector<impl_lsp::LSPDocumentSymbol> symbols) {
            if (error) {
                callback(toGRCError(error), nullptr, context);
                return;
            }

            auto result = std::make_unique<GRCDocumentSymbolArray>();
            convertDocumentSymbols(symbols, result.get());
            callback(GRC_SUCCESS, result.release(), context);
        }
    );
}

void grc_lsp_client_workspace_symbols_async(
    GRCLSPClient* client,
    const char* query,
    GRCSymbolInfoCallback callback,
    void* context
) {
    auto* impl = getClient(client);
    if (!impl || !query || !callback) {
        if (callback) callback(GRC_ERROR_INVALID_ARGUMENT, nullptr, context);
        return;
    }

    impl->workspaceSymbolsAsync(
        query,
        [callback, context](impl_lsp::LSPError error, std::vector<impl_lsp::LSPSymbolInformation> symbols) {
            if (error) {
                callback(toGRCError(error), nullptr, context);
                return;
            }

            auto result = std::make_unique<GRCSymbolInformationArray>();
            convertSymbolInformation(symbols, result.get());
            callback(GRC_SUCCESS, result.release(), context);
        }
    );
}

// ============================================================================
// Folding Range
// ============================================================================

static GRCFoldingRangeKind convertFoldingRangeKind(impl_lsp::FoldingRangeKind kind) {
    switch (kind) {
        case impl_lsp::FoldingRangeKind::Comment: return GRC_FOLD_COMMENT;
        case impl_lsp::FoldingRangeKind::Imports: return GRC_FOLD_IMPORTS;
        case impl_lsp::FoldingRangeKind::Region: return GRC_FOLD_REGION;
        default: return GRC_FOLD_REGION;
    }
}

static void convertFoldingRanges(
    const std::vector<impl_lsp::LSPFoldingRange>& ranges,
    GRCFoldingRangeArray* out
) {
    out->count = static_cast<int32_t>(ranges.size());
    out->ranges = new GRCFoldingRange[ranges.size()];

    for (size_t i = 0; i < ranges.size(); ++i) {
        out->ranges[i].start_line = ranges[i].startLine;
        out->ranges[i].end_line = ranges[i].endLine;
        out->ranges[i].has_kind = ranges[i].kind.has_value();
        if (ranges[i].kind) {
            out->ranges[i].kind = convertFoldingRangeKind(*ranges[i].kind);
        }
    }
}

GRCError grc_lsp_client_folding_range(
    GRCLSPClient* client,
    const char* file_uri,
    GRCFoldingRangeArray* out_ranges
) {
    auto* impl = getClient(client);
    if (!impl || !file_uri || !out_ranges) {
        return GRC_ERROR_INVALID_ARGUMENT;
    }

    std::vector<impl_lsp::LSPFoldingRange> ranges;
    impl_lsp::LSPError error = impl->foldingRange(file_uri, ranges);

    if (error) {
        return toGRCError(error);
    }

    convertFoldingRanges(ranges, out_ranges);
    return GRC_SUCCESS;
}

void grc_lsp_client_folding_range_async(
    GRCLSPClient* client,
    const char* file_uri,
    GRCFoldingRangeCallback callback,
    void* context
) {
    auto* impl = getClient(client);
    if (!impl || !file_uri || !callback) {
        if (callback) {
            GRCFoldingRangeArray empty = {nullptr, 0};
            callback(GRC_ERROR_INVALID_ARGUMENT, empty, context);
        }
        return;
    }

    impl->foldingRangeAsync(
        file_uri,
        [callback, context](impl_lsp::LSPError error, std::vector<impl_lsp::LSPFoldingRange> ranges) {
            if (error) {
                GRCFoldingRangeArray empty = {nullptr, 0};
                callback(toGRCError(error), empty, context);
                return;
            }

            GRCFoldingRangeArray result;
            convertFoldingRanges(ranges, &result);
            callback(GRC_SUCCESS, result, context);
        }
    );
}

// ============================================================================
// Memory Management
// ============================================================================

void grc_free_string(char* str) {
    delete[] str;
}

void grc_free_locations(GRCLocationArray* locations) {
    if (locations) {
        if (locations->locations) {
            for (int32_t i = 0; i < locations->count; ++i) {
                delete[] locations->locations[i].uri;
            }
            delete[] locations->locations;
            locations->locations = nullptr;
        }
        locations->count = 0;
        // Note: We don't delete the GRCLocationArray struct itself
        // because it may be stack-allocated by the caller
    }
}

void grc_free_hover(GRCHover* hover) {
    if (hover) {
        delete[] hover->contents;
        hover->contents = nullptr;
        hover->has_range = false;
        // Note: We don't delete the GRCHover struct itself
        // because it may be stack-allocated by the caller
    }
}

static void freeDocumentSymbol(GRCDocumentSymbol* sym) {
    if (!sym) return;
    delete[] sym->name;
    delete[] sym->detail;
    if (sym->children) {
        for (int32_t i = 0; i < sym->children_count; ++i) {
            freeDocumentSymbol(&sym->children[i]);
        }
        delete[] sym->children;
    }
}

void grc_free_document_symbols(GRCDocumentSymbolArray* symbols) {
    if (symbols) {
        if (symbols->symbols) {
            for (int32_t i = 0; i < symbols->count; ++i) {
                freeDocumentSymbol(&symbols->symbols[i]);
            }
            delete[] symbols->symbols;
            symbols->symbols = nullptr;
        }
        symbols->count = 0;
        // Note: We don't delete the GRCDocumentSymbolArray struct itself
        // because it may be stack-allocated by the caller
    }
}

void grc_free_symbol_information(GRCSymbolInformationArray* symbols) {
    if (symbols) {
        if (symbols->symbols) {
            for (int32_t i = 0; i < symbols->count; ++i) {
                delete[] symbols->symbols[i].name;
                delete[] symbols->symbols[i].location.uri;
                delete[] symbols->symbols[i].container_name;
            }
            delete[] symbols->symbols;
            symbols->symbols = nullptr;
        }
        symbols->count = 0;
        // Note: We don't delete the GRCSymbolInformationArray struct itself
        // because it may be stack-allocated by the caller
    }
}

void grc_free_folding_ranges(GRCFoldingRangeArray* ranges) {
    if (ranges && ranges->ranges) {
        delete[] ranges->ranges;
        ranges->ranges = nullptr;
        ranges->count = 0;
        // Note: We don't delete the GRCFoldingRangeArray struct itself
        // because it may be stack-allocated by the caller
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

const char* grc_language_id(GRCLanguage language) {
    switch (language) {
        case GRC_LANG_RUST:       return "rust";
        case GRC_LANG_PYTHON:     return "python";
        case GRC_LANG_JAVASCRIPT: return "javascript";
        case GRC_LANG_TYPESCRIPT: return "typescript";
        case GRC_LANG_GO:         return "go";
        case GRC_LANG_CPP:        return "cpp";
        case GRC_LANG_C:          return "c";
        case GRC_LANG_SWIFT:      return "swift";
        default:                  return "plaintext";
    }
}

// Helper to find an executable in common paths
static std::string findExecutableC(const char* name) {
    // Common paths to search
    std::vector<std::string> searchPaths = {
        "/usr/local/bin",
        "/usr/bin",
        "/opt/homebrew/bin",
        std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.cargo/bin",
        std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.local/bin",
    };

    // Also check PATH environment variable
    if (const char* pathEnv = std::getenv("PATH")) {
        std::string pathStr(pathEnv);
        std::string::size_type start = 0;
        std::string::size_type pos;
        while ((pos = pathStr.find(':', start)) != std::string::npos) {
            std::string dir = pathStr.substr(start, pos - start);
            if (!dir.empty()) {
                searchPaths.push_back(dir);
            }
            start = pos + 1;
        }
        if (start < pathStr.length()) {
            searchPaths.push_back(pathStr.substr(start));
        }
    }

    // Search for the executable
    for (const auto& dir : searchPaths) {
        std::string fullPath = dir + "/" + name;
        if (access(fullPath.c_str(), X_OK) == 0) {
            return fullPath;
        }
    }

    // Fallback to just the name (rely on PATH)
    return name;
}

char* grc_language_server_path(GRCLanguage language) {
    const char* name = nullptr;

    switch (language) {
        case GRC_LANG_RUST:
            name = "rust-analyzer";
            break;
        case GRC_LANG_PYTHON:
            name = "pylsp";
            break;
        case GRC_LANG_GO:
            name = "gopls";
            break;
        case GRC_LANG_TYPESCRIPT:
        case GRC_LANG_JAVASCRIPT:
            name = "typescript-language-server";
            break;
        case GRC_LANG_CPP:
        case GRC_LANG_C:
            name = "clangd";
            break;
        case GRC_LANG_SWIFT:
            name = "sourcekit-lsp";
            break;
        default:
            return nullptr;
    }

    std::string path = findExecutableC(name);
    return duplicateString(path.c_str());
}

const char* grc_symbol_kind_name(GRCSymbolKind kind) {
    switch (kind) {
        case GRC_SYMBOL_FILE: return "file";
        case GRC_SYMBOL_MODULE: return "module";
        case GRC_SYMBOL_NAMESPACE: return "namespace";
        case GRC_SYMBOL_PACKAGE: return "package";
        case GRC_SYMBOL_CLASS: return "class";
        case GRC_SYMBOL_METHOD: return "method";
        case GRC_SYMBOL_PROPERTY: return "property";
        case GRC_SYMBOL_FIELD: return "field";
        case GRC_SYMBOL_CONSTRUCTOR: return "constructor";
        case GRC_SYMBOL_ENUM: return "enum";
        case GRC_SYMBOL_INTERFACE: return "interface";
        case GRC_SYMBOL_FUNCTION: return "function";
        case GRC_SYMBOL_VARIABLE: return "variable";
        case GRC_SYMBOL_CONSTANT: return "constant";
        case GRC_SYMBOL_STRING: return "string";
        case GRC_SYMBOL_NUMBER: return "number";
        case GRC_SYMBOL_BOOLEAN: return "boolean";
        case GRC_SYMBOL_ARRAY: return "array";
        case GRC_SYMBOL_OBJECT: return "object";
        case GRC_SYMBOL_KEY: return "key";
        case GRC_SYMBOL_NULL: return "null";
        case GRC_SYMBOL_ENUM_MEMBER: return "enumMember";
        case GRC_SYMBOL_STRUCT: return "struct";
        case GRC_SYMBOL_EVENT: return "event";
        case GRC_SYMBOL_OPERATOR: return "operator";
        case GRC_SYMBOL_TYPE_PARAMETER: return "typeParameter";
        default: return "unknown";
    }
}

const char* grc_version(void) {
    return VERSION;
}

// ============================================================================
// Full Index API Implementation
// ============================================================================

GRCIndexer* grc_indexer_create(
    const char* workspace_path,
    GRCLanguage language,
    GRCLSPCache* cache,
    GRCError* out_error
) {
    spdlog::info("[GRC] grc_indexer_create called for: {}", workspace_path);

    if (!workspace_path) {
        if (out_error) *out_error = GRC_ERROR_INVALID_ARGUMENT;
        return nullptr;
    }

    try {
        impl_lsp::Language lspLang = static_cast<impl_lsp::Language>(language);

        std::shared_ptr<impl_db::LSPCache> cachePtr;
        if (cache) {
            // Create a shared_ptr that doesn't own the cache (no deleter)
            cachePtr = std::shared_ptr<impl_db::LSPCache>(
                getCache(cache),
                [](impl_db::LSPCache*) {} // No-op deleter
            );
        }

        auto indexer = new impl_indexer::Indexer(workspace_path, lspLang, cachePtr);

        auto handle = new GRCIndexer();
        handle->_opaque = indexer;

        if (out_error) *out_error = GRC_SUCCESS;
        return handle;
    } catch (const std::exception& e) {
        spdlog::error("[GRC] Failed to create indexer: {}", e.what());
        if (out_error) *out_error = GRC_ERROR_ALLOCATION_FAILED;
        return nullptr;
    }
}

void grc_indexer_destroy(GRCIndexer* indexer) {
    if (!indexer) return;

    spdlog::info("[GRC] grc_indexer_destroy called");

    auto ptr = getIndexer(indexer);
    delete ptr;
    delete indexer;
}

void grc_indexer_set_include_patterns(
    GRCIndexer* indexer,
    const char** patterns
) {
    auto ptr = getIndexer(indexer);
    if (!ptr || !patterns) return;

    std::vector<std::string> patternVec;
    for (const char** p = patterns; *p != nullptr; ++p) {
        patternVec.push_back(*p);
    }
    ptr->setIncludePatterns(patternVec);
}

void grc_indexer_set_exclude_patterns(
    GRCIndexer* indexer,
    const char** patterns
) {
    auto ptr = getIndexer(indexer);
    if (!ptr || !patterns) return;

    std::vector<std::string> patternVec;
    for (const char** p = patterns; *p != nullptr; ++p) {
        patternVec.push_back(*p);
    }
    ptr->setExcludePatterns(patternVec);
}

GRCError grc_indexer_run(
    GRCIndexer* indexer,
    GRCIndexProgressCallback progress_callback,
    void* progress_context,
    int32_t* out_files_indexed,
    int32_t* out_definitions_found,
    int32_t* out_references_found
) {
    auto ptr = getIndexer(indexer);
    if (!ptr) return GRC_ERROR_INVALID_ARGUMENT;

    spdlog::info("[GRC] grc_indexer_run called");

    // Wrap the C callback
    impl_indexer::ProgressCallback cppCallback = nullptr;
    if (progress_callback) {
        cppCallback = [progress_callback, progress_context](
            const std::string& filePath, int current, int total
        ) -> bool {
            return progress_callback(filePath.c_str(), current, total, progress_context);
        };
    }

    auto result = ptr->run(cppCallback);

    if (out_files_indexed) *out_files_indexed = result.filesIndexed;
    if (out_definitions_found) *out_definitions_found = result.definitionsFound;
    if (out_references_found) *out_references_found = result.referencesFound;

    if (result.error) {
        spdlog::error("[GRC] Indexer error: {}", result.error.message);
        return toGRCError(result.error);
    }

    spdlog::info("[GRC] Indexing complete: {} files, {} definitions, {} references",
        result.filesIndexed, result.definitionsFound, result.referencesFound);

    return GRC_SUCCESS;
}

void grc_indexer_run_async(
    GRCIndexer* indexer,
    GRCIndexProgressCallback progress_callback,
    void* progress_context,
    GRCIndexCompleteCallback complete_callback,
    void* complete_context
) {
    auto ptr = getIndexer(indexer);
    if (!ptr) {
        if (complete_callback) {
            complete_callback(GRC_ERROR_INVALID_ARGUMENT, 0, 0, 0, complete_context);
        }
        return;
    }

    spdlog::info("[GRC] grc_indexer_run_async called");

    // Wrap callbacks
    impl_indexer::ProgressCallback cppProgress = nullptr;
    if (progress_callback) {
        cppProgress = [progress_callback, progress_context](
            const std::string& filePath, int current, int total
        ) -> bool {
            return progress_callback(filePath.c_str(), current, total, progress_context);
        };
    }

    impl_indexer::CompleteCallback cppComplete = nullptr;
    if (complete_callback) {
        cppComplete = [complete_callback, complete_context](
            impl_lsp::LSPError error, int filesIndexed, int definitionsFound, int referencesFound
        ) {
            GRCError grcError = error ? toGRCError(error) : GRC_SUCCESS;
            complete_callback(grcError, filesIndexed, definitionsFound, referencesFound, complete_context);
        };
    }

    ptr->runAsync(cppProgress, cppComplete);
}

void grc_indexer_cancel(GRCIndexer* indexer) {
    auto ptr = getIndexer(indexer);
    if (ptr) {
        ptr->cancel();
    }
}

bool grc_indexer_is_running(GRCIndexer* indexer) {
    auto ptr = getIndexer(indexer);
    return ptr ? ptr->isRunning() : false;
}

GRCError grc_indexer_get_definitions(
    GRCIndexer* indexer,
    GRCIndexDefinitionArray* out_definitions
) {
    auto ptr = getIndexer(indexer);
    if (!ptr || !out_definitions) return GRC_ERROR_INVALID_ARGUMENT;

    auto defs = ptr->getDefinitions();

    out_definitions->count = static_cast<int32_t>(defs.size());
    if (defs.empty()) {
        out_definitions->definitions = nullptr;
        return GRC_SUCCESS;
    }

    out_definitions->definitions = new GRCIndexDefinition[defs.size()];
    for (size_t i = 0; i < defs.size(); ++i) {
        out_definitions->definitions[i].name = duplicateString(defs[i].name);
        out_definitions->definitions[i].file_uri = duplicateString(defs[i].fileUri);
        out_definitions->definitions[i].kind = static_cast<GRCSymbolKind>(defs[i].kind);
        out_definitions->definitions[i].range = toGRCRange(defs[i].range);
        out_definitions->definitions[i].container_name = duplicateString(defs[i].containerName);
    }

    return GRC_SUCCESS;
}

GRCError grc_indexer_get_references(
    GRCIndexer* indexer,
    GRCIndexReferenceArray* out_references
) {
    auto ptr = getIndexer(indexer);
    if (!ptr || !out_references) return GRC_ERROR_INVALID_ARGUMENT;

    auto refs = ptr->getReferences();

    out_references->count = static_cast<int32_t>(refs.size());
    if (refs.empty()) {
        out_references->references = nullptr;
        return GRC_SUCCESS;
    }

    out_references->references = new GRCIndexReference[refs.size()];
    for (size_t i = 0; i < refs.size(); ++i) {
        out_references->references[i].from_file_uri = duplicateString(refs[i].fromFileUri);
        out_references->references[i].from_range = toGRCRange(refs[i].fromRange);
        out_references->references[i].to_file_uri = duplicateString(refs[i].toFileUri);
        out_references->references[i].to_range = toGRCRange(refs[i].toRange);
        out_references->references[i].symbol_name = duplicateString(refs[i].symbolName);
    }

    return GRC_SUCCESS;
}

GRCError grc_indexer_get_file_definitions(
    GRCIndexer* indexer,
    const char* file_uri,
    GRCIndexDefinitionArray* out_definitions
) {
    auto ptr = getIndexer(indexer);
    if (!ptr || !file_uri || !out_definitions) return GRC_ERROR_INVALID_ARGUMENT;

    auto defs = ptr->getFileDefinitions(file_uri);

    out_definitions->count = static_cast<int32_t>(defs.size());
    if (defs.empty()) {
        out_definitions->definitions = nullptr;
        return GRC_SUCCESS;
    }

    out_definitions->definitions = new GRCIndexDefinition[defs.size()];
    for (size_t i = 0; i < defs.size(); ++i) {
        out_definitions->definitions[i].name = duplicateString(defs[i].name);
        out_definitions->definitions[i].file_uri = duplicateString(defs[i].fileUri);
        out_definitions->definitions[i].kind = static_cast<GRCSymbolKind>(defs[i].kind);
        out_definitions->definitions[i].range = toGRCRange(defs[i].range);
        out_definitions->definitions[i].container_name = duplicateString(defs[i].containerName);
    }

    return GRC_SUCCESS;
}

GRCError grc_indexer_get_file_references(
    GRCIndexer* indexer,
    const char* file_uri,
    GRCIndexReferenceArray* out_references
) {
    auto ptr = getIndexer(indexer);
    if (!ptr || !file_uri || !out_references) return GRC_ERROR_INVALID_ARGUMENT;

    auto refs = ptr->getFileReferences(file_uri);

    out_references->count = static_cast<int32_t>(refs.size());
    if (refs.empty()) {
        out_references->references = nullptr;
        return GRC_SUCCESS;
    }

    out_references->references = new GRCIndexReference[refs.size()];
    for (size_t i = 0; i < refs.size(); ++i) {
        out_references->references[i].from_file_uri = duplicateString(refs[i].fromFileUri);
        out_references->references[i].from_range = toGRCRange(refs[i].fromRange);
        out_references->references[i].to_file_uri = duplicateString(refs[i].toFileUri);
        out_references->references[i].to_range = toGRCRange(refs[i].toRange);
        out_references->references[i].symbol_name = duplicateString(refs[i].symbolName);
    }

    return GRC_SUCCESS;
}

GRCError grc_indexer_get_references_to_file(
    GRCIndexer* indexer,
    const char* file_uri,
    GRCIndexReferenceArray* out_references
) {
    auto ptr = getIndexer(indexer);
    if (!ptr || !file_uri || !out_references) return GRC_ERROR_INVALID_ARGUMENT;

    auto refs = ptr->getReferencesToFile(file_uri);

    out_references->count = static_cast<int32_t>(refs.size());
    if (refs.empty()) {
        out_references->references = nullptr;
        return GRC_SUCCESS;
    }

    out_references->references = new GRCIndexReference[refs.size()];
    for (size_t i = 0; i < refs.size(); ++i) {
        out_references->references[i].from_file_uri = duplicateString(refs[i].fromFileUri);
        out_references->references[i].from_range = toGRCRange(refs[i].fromRange);
        out_references->references[i].to_file_uri = duplicateString(refs[i].toFileUri);
        out_references->references[i].to_range = toGRCRange(refs[i].toRange);
        out_references->references[i].symbol_name = duplicateString(refs[i].symbolName);
    }

    return GRC_SUCCESS;
}

void grc_free_index_definitions(GRCIndexDefinitionArray* definitions) {
    if (definitions) {
        if (definitions->definitions) {
            for (int32_t i = 0; i < definitions->count; ++i) {
                delete[] definitions->definitions[i].name;
                delete[] definitions->definitions[i].file_uri;
                delete[] definitions->definitions[i].container_name;
            }
            delete[] definitions->definitions;
            definitions->definitions = nullptr;
        }
        definitions->count = 0;
    }
}

void grc_free_index_references(GRCIndexReferenceArray* references) {
    if (references) {
        if (references->references) {
            for (int32_t i = 0; i < references->count; ++i) {
                delete[] references->references[i].from_file_uri;
                delete[] references->references[i].to_file_uri;
                delete[] references->references[i].symbol_name;
            }
            delete[] references->references;
            references->references = nullptr;
        }
        references->count = 0;
    }
}

} // extern "C"
