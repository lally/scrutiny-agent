// GitReviewCore.h
// Main header for the Git Review C++ core library
// This file provides the C API that can be called from Swift

#ifndef GIT_REVIEW_CORE_H
#define GIT_REVIEW_CORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Opaque Handle Types
// ============================================================================

// Opaque handle type for LSP client
typedef struct GRCLSPClient {
    void* _opaque;
} GRCLSPClient;

// Opaque handle type for LSP cache
typedef struct GRCLSPCache {
    void* _opaque;
} GRCLSPCache;

// ============================================================================
// Error Codes
// ============================================================================

typedef enum {
    GRC_SUCCESS = 0,
    GRC_ERROR_INVALID_ARGUMENT = 1,
    GRC_ERROR_NO_LANGUAGE_SERVER = 2,
    GRC_ERROR_CONNECTION_FAILED = 3,
    GRC_ERROR_TIMEOUT = 4,
    GRC_ERROR_LSP_ERROR = 5,
    GRC_ERROR_NOT_INITIALIZED = 6,
    GRC_ERROR_ALLOCATION_FAILED = 7,
    GRC_ERROR_DATABASE_ERROR = 8,
} GRCError;

// ============================================================================
// Basic Types
// ============================================================================

// LSP Position
typedef struct {
    int32_t line;
    int32_t character;
} GRCPosition;

// LSP Range
typedef struct {
    GRCPosition start;
    GRCPosition end;
} GRCRange;

// LSP Location (caller must free uri with grc_free_string)
typedef struct {
    char* uri;
    GRCRange range;
} GRCLocation;

// Array of locations (caller must free with grc_free_locations)
typedef struct {
    GRCLocation* locations;
    int32_t count;
} GRCLocationArray;

// Hover result (caller must free contents with grc_free_string)
typedef struct {
    char* contents;  // Markdown content
    bool has_range;
    GRCRange range;
} GRCHover;

// Symbol kinds
typedef enum {
    GRC_SYMBOL_FILE = 1,
    GRC_SYMBOL_MODULE = 2,
    GRC_SYMBOL_NAMESPACE = 3,
    GRC_SYMBOL_PACKAGE = 4,
    GRC_SYMBOL_CLASS = 5,
    GRC_SYMBOL_METHOD = 6,
    GRC_SYMBOL_PROPERTY = 7,
    GRC_SYMBOL_FIELD = 8,
    GRC_SYMBOL_CONSTRUCTOR = 9,
    GRC_SYMBOL_ENUM = 10,
    GRC_SYMBOL_INTERFACE = 11,
    GRC_SYMBOL_FUNCTION = 12,
    GRC_SYMBOL_VARIABLE = 13,
    GRC_SYMBOL_CONSTANT = 14,
    GRC_SYMBOL_STRING = 15,
    GRC_SYMBOL_NUMBER = 16,
    GRC_SYMBOL_BOOLEAN = 17,
    GRC_SYMBOL_ARRAY = 18,
    GRC_SYMBOL_OBJECT = 19,
    GRC_SYMBOL_KEY = 20,
    GRC_SYMBOL_NULL = 21,
    GRC_SYMBOL_ENUM_MEMBER = 22,
    GRC_SYMBOL_STRUCT = 23,
    GRC_SYMBOL_EVENT = 24,
    GRC_SYMBOL_OPERATOR = 25,
    GRC_SYMBOL_TYPE_PARAMETER = 26,
} GRCSymbolKind;

// Document symbol (caller must free name/detail with grc_free_string)
typedef struct GRCDocumentSymbol {
    char* name;
    char* detail;
    GRCSymbolKind kind;
    GRCRange range;
    GRCRange selection_range;
    struct GRCDocumentSymbol* children;
    int32_t children_count;
} GRCDocumentSymbol;

// Array of document symbols
typedef struct {
    GRCDocumentSymbol* symbols;
    int32_t count;
} GRCDocumentSymbolArray;

// Workspace symbol (caller must free name/container_name with grc_free_string)
typedef struct {
    char* name;
    GRCSymbolKind kind;
    GRCLocation location;
    char* container_name;
} GRCSymbolInformation;

// Array of workspace symbols
typedef struct {
    GRCSymbolInformation* symbols;
    int32_t count;
} GRCSymbolInformationArray;

// Folding range kinds
typedef enum {
    GRC_FOLD_COMMENT = 0,
    GRC_FOLD_IMPORTS = 1,
    GRC_FOLD_REGION = 2
} GRCFoldingRangeKind;

// Folding range
typedef struct {
    int32_t start_line;
    int32_t end_line;
    bool has_kind;
    GRCFoldingRangeKind kind;
} GRCFoldingRange;

// Array of folding ranges
typedef struct {
    GRCFoldingRange* ranges;
    int32_t count;
} GRCFoldingRangeArray;

// Language types supported by LSP
typedef enum {
    GRC_LANG_UNKNOWN = 0,
    GRC_LANG_RUST = 1,
    GRC_LANG_PYTHON = 2,
    GRC_LANG_JAVASCRIPT = 3,
    GRC_LANG_TYPESCRIPT = 4,
    GRC_LANG_GO = 5,
    GRC_LANG_CPP = 6,
    GRC_LANG_C = 7,
    GRC_LANG_SWIFT = 8,
} GRCLanguage;

// Cache statistics
typedef struct {
    int64_t definition_count;
    int64_t reference_count;
    int64_t hover_count;
    int64_t symbol_count;
    int64_t total_size;  // bytes
} GRCCacheStats;

// Callback types for async operations
typedef void (*GRCLocationCallback)(GRCError error, GRCLocationArray* result, void* context);
typedef void (*GRCHoverCallback)(GRCError error, GRCHover* result, void* context);
typedef void (*GRCDocumentSymbolCallback)(GRCError error, GRCDocumentSymbolArray* result, void* context);
typedef void (*GRCSymbolInfoCallback)(GRCError error, GRCSymbolInformationArray* result, void* context);

// ============================================================================
// LSP Cache API
// ============================================================================

// Create a new LSP cache database at the given path
GRCLSPCache* grc_cache_create(const char* db_path, GRCError* out_error);

// Destroy an LSP cache
void grc_cache_destroy(GRCLSPCache* cache);

// Set the current git SHA for cache validity
void grc_cache_set_git_sha(GRCLSPCache* cache, const char* sha);

// Check if cache is valid for a file
bool grc_cache_is_valid(GRCLSPCache* cache, const char* file_path);

// Invalidate cache for a specific file
void grc_cache_invalidate_file(GRCLSPCache* cache, const char* file_path);

// Clear all cache data
void grc_cache_clear_all(GRCLSPCache* cache);

// Get cache statistics
GRCCacheStats grc_cache_get_stats(GRCLSPCache* cache);

// Compact the database
void grc_cache_vacuum(GRCLSPCache* cache);

// TTL hygiene: delete on-demand query-cache entries older than `days`
// days. The durable index tables are NOT touched. `days` <= 0 prunes
// all on-demand entries. Returns rows removed (0 if cache null/closed).
int64_t grc_cache_prune(GRCLSPCache* cache, int days);

// ============================================================================
// LSP Client API
// ============================================================================

// Create a new LSP client for a workspace
GRCLSPClient* grc_lsp_client_create(
    const char* workspace_path,
    GRCLanguage language,
    GRCError* out_error
);

// Destroy an LSP client
void grc_lsp_client_destroy(GRCLSPClient* client);

// Attach a cache to the client
void grc_lsp_client_set_cache(GRCLSPClient* client, GRCLSPCache* cache);

// Set the current git SHA for cache keying
void grc_lsp_client_set_git_sha(GRCLSPClient* client, const char* sha);

// Start the LSP server process
GRCError grc_lsp_client_start(GRCLSPClient* client);

// Stop the LSP server process
void grc_lsp_client_stop(GRCLSPClient* client);

// Check if the client is initialized and ready
bool grc_lsp_client_is_ready(GRCLSPClient* client);

// ============================================================================
// Document Management
// ============================================================================

// Open a document in the LSP server
GRCError grc_lsp_client_did_open(
    GRCLSPClient* client,
    const char* file_uri,
    const char* language_id,
    int32_t version,
    const char* content
);

// Close a document in the LSP server
GRCError grc_lsp_client_did_close(
    GRCLSPClient* client,
    const char* file_uri
);

// Notify about document changes
GRCError grc_lsp_client_did_change(
    GRCLSPClient* client,
    const char* file_uri,
    int32_t version,
    const char* content
);

// ============================================================================
// LSP Operations (Synchronous)
// ============================================================================

// Go to definition
GRCError grc_lsp_client_goto_definition(
    GRCLSPClient* client,
    const char* file_uri,
    GRCPosition position,
    GRCLocationArray* out_locations
);

// Find references
GRCError grc_lsp_client_find_references(
    GRCLSPClient* client,
    const char* file_uri,
    GRCPosition position,
    bool include_declaration,
    GRCLocationArray* out_locations
);

// Get hover information
GRCError grc_lsp_client_hover(
    GRCLSPClient* client,
    const char* file_uri,
    GRCPosition position,
    GRCHover* out_hover
);

// Get document symbols (outline)
GRCError grc_lsp_client_document_symbols(
    GRCLSPClient* client,
    const char* file_uri,
    GRCDocumentSymbolArray* out_symbols
);

// Search workspace symbols
GRCError grc_lsp_client_workspace_symbols(
    GRCLSPClient* client,
    const char* query,
    GRCSymbolInformationArray* out_symbols
);

// ============================================================================
// LSP Operations (Asynchronous)
// ============================================================================

void grc_lsp_client_goto_definition_async(
    GRCLSPClient* client,
    const char* file_uri,
    GRCPosition position,
    GRCLocationCallback callback,
    void* context
);

void grc_lsp_client_find_references_async(
    GRCLSPClient* client,
    const char* file_uri,
    GRCPosition position,
    bool include_declaration,
    GRCLocationCallback callback,
    void* context
);

void grc_lsp_client_hover_async(
    GRCLSPClient* client,
    const char* file_uri,
    GRCPosition position,
    GRCHoverCallback callback,
    void* context
);

void grc_lsp_client_document_symbols_async(
    GRCLSPClient* client,
    const char* file_uri,
    GRCDocumentSymbolCallback callback,
    void* context
);

void grc_lsp_client_workspace_symbols_async(
    GRCLSPClient* client,
    const char* query,
    GRCSymbolInfoCallback callback,
    void* context
);

// Folding range callback
typedef void (*GRCFoldingRangeCallback)(
    GRCError error,
    GRCFoldingRangeArray ranges,
    void* context
);

// Get folding ranges for a document (synchronous)
GRCError grc_lsp_client_folding_range(
    GRCLSPClient* client,
    const char* file_uri,
    GRCFoldingRangeArray* out_ranges
);

// Get folding ranges for a document (asynchronous)
void grc_lsp_client_folding_range_async(
    GRCLSPClient* client,
    const char* file_uri,
    GRCFoldingRangeCallback callback,
    void* context
);

// ============================================================================
// Memory Management
// ============================================================================

// Free a string allocated by the library
void grc_free_string(char* str);

// Free a locations array (frees contained URIs and the array itself)
void grc_free_locations(GRCLocationArray* locations);

// Free a hover result
void grc_free_hover(GRCHover* hover);

// Free document symbols array
void grc_free_document_symbols(GRCDocumentSymbolArray* symbols);

// Free workspace symbols array
void grc_free_symbol_information(GRCSymbolInformationArray* symbols);

// Free folding ranges array
void grc_free_folding_ranges(GRCFoldingRangeArray* ranges);

// ============================================================================
// Utility Functions
// ============================================================================

// Get the language ID string for a language enum
const char* grc_language_id(GRCLanguage language);

// Get the language server executable path for a language
// Returns NULL if no server is configured
// Caller must free the returned string with grc_free_string
char* grc_language_server_path(GRCLanguage language);

// Get symbol kind name
const char* grc_symbol_kind_name(GRCSymbolKind kind);

// Get version string
const char* grc_version(void);

// ============================================================================
// Filesystem API
// ============================================================================
//
// Working-tree file reads. Pure I/O (kernel-arbitrated; the agent runs
// these with free parallelism). On GRC_SUCCESS, *out_content is a
// NUL-terminated buffer the caller frees with grc_free_string and
// *out_size is the byte length (the content itself may contain NUL
// bytes for binary files; out_size is authoritative). On failure
// *out_content is NULL: GRC_ERROR_INVALID_ARGUMENT (null args),
// GRC_ERROR_CONNECTION_FAILED (open/read failed; e.g. ENOENT/EACCES/
// is-a-directory).
GRCError grc_fs_read_file(const char* path, char** out_content,
                          int64_t* out_size);

// ============================================================================
// HEAD-watch API
// ============================================================================
//
// Watches the resolved HEAD ref under `clone_path` (regular clone
// `<clone_path>/.git/HEAD`, or a linked worktree whose `.git` file
// points at the real gitdir) for external git changes (fetch/pull/
// checkout/rebase). `callback(context)` is invoked from a Core-managed
// background thread, debounced (~250 ms) and coalesced; git's atomic
// `HEAD.lock -> HEAD` rename is handled by re-arming on the new file.
// Linux uses inotify, Darwin kqueue. Returns NULL if HEAD can't be
// resolved. `grc_watch_destroy` stops the thread and is idempotent.
typedef struct GRCWatch GRCWatch;
typedef void (*GRCHeadChangeCallback)(void* context);

GRCWatch* grc_watch_head_create(const char* clone_path,
                                GRCHeadChangeCallback callback,
                                void* context);
void grc_watch_destroy(GRCWatch* watch);

// ============================================================================
// Full Index API
// ============================================================================

// Index entry for a single symbol definition
typedef struct {
    char* name;              // Symbol name
    char* file_uri;          // File where defined
    GRCSymbolKind kind;      // Type of symbol
    GRCRange range;          // Location in file
    char* container_name;    // Parent symbol name (if any)
} GRCIndexDefinition;

// Array of index definitions
typedef struct {
    GRCIndexDefinition* definitions;
    int32_t count;
} GRCIndexDefinitionArray;

// Index entry for a reference
typedef struct {
    char* from_file_uri;     // File containing the reference
    GRCRange from_range;     // Location of reference in source file
    char* to_file_uri;       // File containing the definition (if resolved)
    GRCRange to_range;       // Location of definition
    char* symbol_name;       // Name of referenced symbol
} GRCIndexReference;

// Array of index references
typedef struct {
    GRCIndexReference* references;
    int32_t count;
} GRCIndexReferenceArray;

// Progress callback for indexing operations
// file_path: current file being indexed
// current: current file number (1-based)
// total: total number of files to index
// Returns false to cancel indexing
typedef bool (*GRCIndexProgressCallback)(
    const char* file_path,
    int32_t current,
    int32_t total,
    void* context
);

// Completion callback for async indexing
typedef void (*GRCIndexCompleteCallback)(
    GRCError error,
    int32_t files_indexed,
    int32_t definitions_found,
    int32_t references_found,
    void* context
);

// Opaque handle for indexer
typedef struct GRCIndexer {
    void* _opaque;
} GRCIndexer;

// Create an indexer for a workspace
// workspace_path: root directory of the source tree
// language: language to index (or GRC_LANG_UNKNOWN for auto-detect)
// cache: optional cache for storing results (can be NULL)
GRCIndexer* grc_indexer_create(
    const char* workspace_path,
    GRCLanguage language,
    GRCLSPCache* cache,
    GRCError* out_error
);

// Destroy an indexer
void grc_indexer_destroy(GRCIndexer* indexer);

// Set file patterns to include (glob patterns, e.g., "*.py", "src/**/*.rs")
// patterns: null-terminated array of pattern strings
void grc_indexer_set_include_patterns(
    GRCIndexer* indexer,
    const char** patterns
);

// Set file patterns to exclude (glob patterns, e.g., "node_modules/**", "*.min.js")
// patterns: null-terminated array of pattern strings
void grc_indexer_set_exclude_patterns(
    GRCIndexer* indexer,
    const char** patterns
);

// Run full indexing (synchronous, blocks until complete)
// Returns number of files indexed, or -1 on error
GRCError grc_indexer_run(
    GRCIndexer* indexer,
    GRCIndexProgressCallback progress_callback,
    void* progress_context,
    int32_t* out_files_indexed,
    int32_t* out_definitions_found,
    int32_t* out_references_found
);

// Run full indexing (asynchronous)
void grc_indexer_run_async(
    GRCIndexer* indexer,
    GRCIndexProgressCallback progress_callback,
    void* progress_context,
    GRCIndexCompleteCallback complete_callback,
    void* complete_context
);

// Cancel an ongoing async indexing operation
void grc_indexer_cancel(GRCIndexer* indexer);

// Check if indexer is currently running
bool grc_indexer_is_running(GRCIndexer* indexer);

// Get all definitions from the index
GRCError grc_indexer_get_definitions(
    GRCIndexer* indexer,
    GRCIndexDefinitionArray* out_definitions
);

// Get all references from the index
GRCError grc_indexer_get_references(
    GRCIndexer* indexer,
    GRCIndexReferenceArray* out_references
);

// Get definitions for a specific file
GRCError grc_indexer_get_file_definitions(
    GRCIndexer* indexer,
    const char* file_uri,
    GRCIndexDefinitionArray* out_definitions
);

// Get references in a specific file (references from this file to other locations)
GRCError grc_indexer_get_file_references(
    GRCIndexer* indexer,
    const char* file_uri,
    GRCIndexReferenceArray* out_references
);

// Get references to a specific file (references from other files pointing here)
GRCError grc_indexer_get_references_to_file(
    GRCIndexer* indexer,
    const char* file_uri,
    GRCIndexReferenceArray* out_references
);

// Free index definitions array
void grc_free_index_definitions(GRCIndexDefinitionArray* definitions);

// Free index references array
void grc_free_index_references(GRCIndexReferenceArray* references);

#ifdef __cplusplus
}
#endif

#endif // GIT_REVIEW_CORE_H
