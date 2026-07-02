// LSPClient.cpp
// LSP Client implementation

#include "lsp/LSPClient.hpp"
#include "lsp/Process.hpp"
#include "db/LSPCache.hpp"

#include <spdlog/spdlog.h>
#include <sstream>
#include <regex>
#include <thread>
#include <chrono>
#include <unistd.h>  // for getpid()

namespace gitreview {
namespace lsp {

// ============================================================================
// Symbol Kind helpers
// ============================================================================

std::string symbolKindToString(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::File: return "file";
        case SymbolKind::Module: return "module";
        case SymbolKind::Namespace: return "namespace";
        case SymbolKind::Package: return "package";
        case SymbolKind::Class: return "class";
        case SymbolKind::Method: return "method";
        case SymbolKind::Property: return "property";
        case SymbolKind::Field: return "field";
        case SymbolKind::Constructor: return "constructor";
        case SymbolKind::Enum: return "enum";
        case SymbolKind::Interface: return "interface";
        case SymbolKind::Function: return "function";
        case SymbolKind::Variable: return "variable";
        case SymbolKind::Constant: return "constant";
        case SymbolKind::String: return "string";
        case SymbolKind::Number: return "number";
        case SymbolKind::Boolean: return "boolean";
        case SymbolKind::Array: return "array";
        case SymbolKind::Object: return "object";
        case SymbolKind::Key: return "key";
        case SymbolKind::Null: return "null";
        case SymbolKind::EnumMember: return "enumMember";
        case SymbolKind::Struct: return "struct";
        case SymbolKind::Event: return "event";
        case SymbolKind::Operator: return "operator";
        case SymbolKind::TypeParameter: return "typeParameter";
        default: return "unknown";
    }
}

SymbolKind symbolKindFromInt(int kind) {
    if (kind >= 1 && kind <= 26) {
        return static_cast<SymbolKind>(kind);
    }
    return SymbolKind::Variable;  // default
}

std::string foldingRangeKindToString(FoldingRangeKind kind) {
    switch (kind) {
        case FoldingRangeKind::Comment: return "comment";
        case FoldingRangeKind::Imports: return "imports";
        case FoldingRangeKind::Region: return "region";
        default: return "";
    }
}

// ============================================================================
// LSPClient
// ============================================================================

LSPClient::LSPClient(std::string workspacePath, Language language)
    : workspacePath_(std::move(workspacePath))
    , language_(language) {
}

LSPClient::~LSPClient() {
    stop();
}

LSPClient::LSPClient(LSPClient&& other) noexcept
    : workspacePath_(std::move(other.workspacePath_))
    , language_(other.language_)
    , initialized_(other.initialized_.load())
    , running_(other.running_.load())
    , currentGitSha_(std::move(other.currentGitSha_))
    , cache_(std::move(other.cache_))
    , process_(std::move(other.process_))
    , nextRequestId_(other.nextRequestId_.load())
    , pendingRequests_(std::move(other.pendingRequests_))
    , openDocuments_(std::move(other.openDocuments_))
    , outputBuffer_(std::move(other.outputBuffer_)) {
    other.initialized_ = false;
    other.running_ = false;
}

LSPClient& LSPClient::operator=(LSPClient&& other) noexcept {
    if (this != &other) {
        stop();

        workspacePath_ = std::move(other.workspacePath_);
        language_ = other.language_;
        initialized_ = other.initialized_.load();
        running_ = other.running_.load();
        currentGitSha_ = std::move(other.currentGitSha_);
        cache_ = std::move(other.cache_);
        process_ = std::move(other.process_);
        nextRequestId_ = other.nextRequestId_.load();
        pendingRequests_ = std::move(other.pendingRequests_);
        openDocuments_ = std::move(other.openDocuments_);
        outputBuffer_ = std::move(other.outputBuffer_);

        other.initialized_ = false;
        other.running_ = false;
    }
    return *this;
}

void LSPClient::setCache(std::shared_ptr<db::LSPCache> cache) {
    cache_ = std::move(cache);
}

void LSPClient::setGitSha(const std::string& sha) {
    currentGitSha_ = sha;
    if (cache_) {
        cache_->setCurrentGitSha(sha);
    }
}

// Helper to find an executable in common paths
static std::string findExecutable(const std::string& name) {
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

std::optional<LanguageServerConfig> LSPClient::getServerConfig(Language lang) {
    switch (lang) {
        case Language::Rust:
            return LanguageServerConfig{findExecutable("rust-analyzer"), {}};
        case Language::Python:
            return LanguageServerConfig{findExecutable("pylsp"), {}};
        case Language::Go:
            return LanguageServerConfig{findExecutable("gopls"), {}};
        case Language::TypeScript:
        case Language::JavaScript:
            return LanguageServerConfig{
                findExecutable("typescript-language-server"),
                {"--stdio"}
            };
        case Language::Cpp:
        case Language::C:
            return LanguageServerConfig{findExecutable("clangd"), {}};
        case Language::Swift:
            return LanguageServerConfig{findExecutable("sourcekit-lsp"), {}};
        default:
            return std::nullopt;
    }
}

std::string LSPClient::getLanguageId(Language lang) {
    switch (lang) {
        case Language::Rust:       return "rust";
        case Language::Python:     return "python";
        case Language::JavaScript: return "javascript";
        case Language::TypeScript: return "typescript";
        case Language::Go:         return "go";
        case Language::Cpp:        return "cpp";
        case Language::C:          return "c";
        case Language::Swift:      return "swift";
        default:                   return "plaintext";
    }
}

LSPError LSPClient::start() {
    spdlog::debug("[LSPClient] start() called for workspace:  {}", workspacePath_);
    spdlog::debug("[LSPClient] Language:  {}", static_cast<int>(language_));

    auto config = getServerConfig(language_);
    if (!config) {
        spdlog::error("[LSPClient] No language server configured for language  {}", static_cast<int>(language_));
        return LSPError::make(
            LSPErrorCode::MethodNotFound,
            "No language server configured for this language"
        );
    }

    spdlog::debug("[LSPClient] Using server:  {}", config->executable);

    process_ = std::make_unique<Process>(
        config->executable,
        config->arguments,
        workspacePath_
    );

    process_->setOutputCallback([this](const std::string& data) {
        spdlog::debug("[LSPClient] Received output callback with {} bytes", data.size());
        handleOutput(data);
    });

    spdlog::debug("[LSPClient] Starting LSP server process...");
    if (!process_->start()) {
        spdlog::error("[LSPClient] Failed to start LSP server process");
        return LSPError::make(
            LSPErrorCode::InternalError,
            "Failed to start language server process"
        );
    }

    spdlog::debug("[LSPClient] LSP server process started, sending initialize request...");
    running_ = true;

    // Send initialize request
    LSPError initError = sendInitialize();
    if (initError) {
        spdlog::error("[LSPClient] Initialize failed:  {}", initError.message);
        stop();
        return initError;
    }

    spdlog::debug("[LSPClient] Initialize succeeded, sending initialized notification...");
    sendInitialized();
    initialized_ = true;

    spdlog::debug("[LSPClient] LSP client fully initialized");
    return LSPError::none();
}

void LSPClient::stop() {
    if (!running_) return;

    running_ = false;
    initialized_ = false;

    // Cancel pending requests
    {
        std::lock_guard<std::mutex> lock(requestsMutex_);
        for (auto& [id, pending] : pendingRequests_) {
            pending->promise.set_exception(
                std::make_exception_ptr(std::runtime_error("Client stopped"))
            );
        }
        pendingRequests_.clear();
    }

    if (process_) {
        process_->stop();
        process_.reset();
    }

    {
        std::lock_guard<std::mutex> lock(documentsMutex_);
        openDocuments_.clear();
    }
}

bool LSPClient::isReady() const {
    return initialized_ && running_;
}

LSPError LSPClient::didOpen(
    const std::string& fileUri,
    const std::string& languageId,
    int version,
    const std::string& content
) {
    if (!isReady()) {
        return LSPError::make(
            LSPErrorCode::ServerNotInitialized,
            "LSP server not initialized"
        );
    }

    {
        std::lock_guard<std::mutex> lock(documentsMutex_);
        if (openDocuments_.count(fileUri)) {
            // Already open
            return LSPError::none();
        }
    }

    nlohmann::json params = {
        {"textDocument", {
            {"uri", fileUri},
            {"languageId", languageId},
            {"version", version},
            {"text", content}
        }}
    };

    sendNotification("textDocument/didOpen", params);

    {
        std::lock_guard<std::mutex> lock(documentsMutex_);
        openDocuments_.insert(fileUri);
    }

    return LSPError::none();
}

LSPError LSPClient::didClose(const std::string& fileUri) {
    if (!isReady()) {
        return LSPError::make(
            LSPErrorCode::ServerNotInitialized,
            "LSP server not initialized"
        );
    }

    {
        std::lock_guard<std::mutex> lock(documentsMutex_);
        if (!openDocuments_.count(fileUri)) {
            // Not open
            return LSPError::none();
        }
    }

    nlohmann::json params = {
        {"textDocument", {{"uri", fileUri}}}
    };

    sendNotification("textDocument/didClose", params);

    {
        std::lock_guard<std::mutex> lock(documentsMutex_);
        openDocuments_.erase(fileUri);
    }

    return LSPError::none();
}

LSPError LSPClient::didChange(
    const std::string& fileUri,
    int version,
    const std::string& content
) {
    if (!isReady()) {
        return LSPError::make(
            LSPErrorCode::ServerNotInitialized,
            "LSP server not initialized"
        );
    }

    nlohmann::json params = {
        {"textDocument", {
            {"uri", fileUri},
            {"version", version}
        }},
        {"contentChanges", {{
            {"text", content}
        }}}
    };

    sendNotification("textDocument/didChange", params);

    // Invalidate cache for this file since content changed
    if (cache_) {
        cache_->invalidateFile(fileUri);
    }

    return LSPError::none();
}

// ============================================================================
// LSP Operations
// ============================================================================

LSPError LSPClient::gotoDefinition(
    const std::string& fileUri,
    LSPPosition position,
    std::vector<LSPLocation>& outLocations
) {
    if (!isReady()) {
        return LSPError::make(
            LSPErrorCode::ServerNotInitialized,
            "LSP server not initialized"
        );
    }

    // Check cache first
    if (cache_) {
        auto cached = cache_->getCachedDefinitions(fileUri, position.line, position.character);
        if (cached) {
            outLocations = std::move(*cached);
            return LSPError::none();
        }
    }

    nlohmann::json params = {
        {"textDocument", {{"uri", fileUri}}},
        {"position", {
            {"line", position.line},
            {"character", position.character}
        }}
    };

    try {
        LSPResponse response = sendRequest("textDocument/definition", params);

        if (response.error) {
            spdlog::debug("[LSPClient] gotoDefinition error: {}", response.error->message);
            return *response.error;
        }

        if (response.result) {
            spdlog::debug("[LSPClient] gotoDefinition result: {}", response.result->dump());
            outLocations = parseLocations(*response.result);
            spdlog::debug("[LSPClient] gotoDefinition parsed {} locations", outLocations.size());

            // Cache the results
            if (cache_) {
                for (const auto& loc : outLocations) {
                    cache_->cacheDefinition(fileUri, position.line, position.character, loc);
                }
            }
        } else {
            spdlog::debug("[LSPClient] gotoDefinition result is null/empty");
        }

        return LSPError::none();
    } catch (const std::exception& e) {
        return LSPError::make(LSPErrorCode::InternalError, e.what());
    }
}

LSPError LSPClient::findReferences(
    const std::string& fileUri,
    LSPPosition position,
    bool includeDeclaration,
    std::vector<LSPLocation>& outLocations
) {
    if (!isReady()) {
        return LSPError::make(
            LSPErrorCode::ServerNotInitialized,
            "LSP server not initialized"
        );
    }

    // Check cache first
    if (cache_) {
        auto cached = cache_->getCachedReferences(fileUri, position.line, position.character);
        if (cached) {
            outLocations = std::move(*cached);
            return LSPError::none();
        }
    }

    nlohmann::json params = {
        {"textDocument", {{"uri", fileUri}}},
        {"position", {
            {"line", position.line},
            {"character", position.character}
        }},
        {"context", {
            {"includeDeclaration", includeDeclaration}
        }}
    };

    try {
        LSPResponse response = sendRequest("textDocument/references", params);

        if (response.error) {
            return *response.error;
        }

        if (response.result) {
            outLocations = parseLocations(*response.result);

            // Cache the results
            if (cache_) {
                cache_->cacheReferences(fileUri, position.line, position.character, outLocations);
            }
        }

        return LSPError::none();
    } catch (const std::exception& e) {
        return LSPError::make(LSPErrorCode::InternalError, e.what());
    }
}

LSPError LSPClient::hover(
    const std::string& fileUri,
    LSPPosition position,
    std::optional<LSPHover>& outHover
) {
    if (!isReady()) {
        return LSPError::make(
            LSPErrorCode::ServerNotInitialized,
            "LSP server not initialized"
        );
    }

    // Check cache first
    if (cache_) {
        auto cached = cache_->getCachedHover(fileUri, position.line, position.character);
        if (cached) {
            LSPHover hover;
            hover.contents = std::move(*cached);
            outHover = std::move(hover);
            return LSPError::none();
        }
    }

    nlohmann::json params = {
        {"textDocument", {{"uri", fileUri}}},
        {"position", {
            {"line", position.line},
            {"character", position.character}
        }}
    };

    try {
        LSPResponse response = sendRequest("textDocument/hover", params);

        if (response.error) {
            return *response.error;
        }

        if (response.result) {
            outHover = parseHover(*response.result);

            // Cache the result
            if (cache_ && outHover) {
                cache_->cacheHover(fileUri, position.line, position.character, outHover->contents);
            }
        }

        return LSPError::none();
    } catch (const std::exception& e) {
        return LSPError::make(LSPErrorCode::InternalError, e.what());
    }
}

LSPError LSPClient::documentSymbols(
    const std::string& fileUri,
    std::vector<LSPDocumentSymbol>& outSymbols
) {
    if (!isReady()) {
        return LSPError::make(
            LSPErrorCode::ServerNotInitialized,
            "LSP server not initialized"
        );
    }

    // Check cache first
    if (cache_) {
        auto cached = cache_->getCachedDocumentSymbols(fileUri);
        if (cached) {
            // Convert CachedDocumentSymbol to LSPDocumentSymbol
            for (const auto& sym : *cached) {
                LSPDocumentSymbol docSym;
                docSym.name = sym.name;
                docSym.detail = sym.detail;
                docSym.kind = symbolKindFromInt(std::stoi(sym.kind.empty() ? "13" : sym.kind));
                docSym.range.start.line = sym.line;
                docSym.range.start.character = sym.character;
                docSym.range.end.line = sym.endLine;
                docSym.range.end.character = sym.endCharacter;
                docSym.selectionRange = docSym.range;
                outSymbols.push_back(docSym);
            }
            return LSPError::none();
        }
    }

    nlohmann::json params = {
        {"textDocument", {{"uri", fileUri}}}
    };

    try {
        LSPResponse response = sendRequest("textDocument/documentSymbol", params);

        if (response.error) {
            return *response.error;
        }

        if (response.result) {
            outSymbols = parseDocumentSymbols(*response.result);

            // Cache the results
            if (cache_ && !outSymbols.empty()) {
                std::vector<db::CachedDocumentSymbol> cachedSymbols;
                for (const auto& sym : outSymbols) {
                    db::CachedDocumentSymbol cached;
                    cached.filePath = fileUri;
                    cached.name = sym.name;
                    cached.kind = std::to_string(static_cast<int>(sym.kind));
                    cached.detail = sym.detail;
                    cached.line = sym.range.start.line;
                    cached.character = sym.range.start.character;
                    cached.endLine = sym.range.end.line;
                    cached.endCharacter = sym.range.end.character;
                    cached.gitSha = currentGitSha_;
                    cachedSymbols.push_back(cached);
                }
                cache_->cacheDocumentSymbols(fileUri, cachedSymbols);
            }
        }

        return LSPError::none();
    } catch (const std::exception& e) {
        return LSPError::make(LSPErrorCode::InternalError, e.what());
    }
}

LSPError LSPClient::workspaceSymbols(
    const std::string& query,
    std::vector<LSPSymbolInformation>& outSymbols
) {
    if (!isReady()) {
        return LSPError::make(
            LSPErrorCode::ServerNotInitialized,
            "LSP server not initialized"
        );
    }

    // Check cache first
    if (cache_) {
        auto cached = cache_->getCachedWorkspaceSymbols(query);
        if (cached) {
            for (const auto& sym : *cached) {
                LSPSymbolInformation info;
                info.name = sym.name;
                info.kind = symbolKindFromInt(std::stoi(sym.kind.empty() ? "13" : sym.kind));
                info.location.uri = "file://" + sym.filePath;
                info.location.range.start.line = sym.line;
                info.location.range.start.character = sym.character;
                info.location.range.end.line = sym.endLine;
                info.location.range.end.character = sym.endCharacter;
                info.containerName = sym.containerName;
                outSymbols.push_back(info);
            }
            return LSPError::none();
        }
    }

    nlohmann::json params = {
        {"query", query}
    };

    try {
        LSPResponse response = sendRequest("workspace/symbol", params);

        if (response.error) {
            return *response.error;
        }

        if (response.result) {
            outSymbols = parseSymbolInformation(*response.result);

            // Cache the results
            if (cache_ && !outSymbols.empty()) {
                std::vector<db::CachedSymbol> cachedSymbols;
                for (const auto& sym : outSymbols) {
                    db::CachedSymbol cached;
                    cached.filePath = sym.location.uri;
                    cached.name = sym.name;
                    cached.kind = std::to_string(static_cast<int>(sym.kind));
                    cached.containerName = sym.containerName;
                    cached.line = sym.location.range.start.line;
                    cached.character = sym.location.range.start.character;
                    cached.endLine = sym.location.range.end.line;
                    cached.endCharacter = sym.location.range.end.character;
                    cached.gitSha = currentGitSha_;
                    cachedSymbols.push_back(cached);
                }
                cache_->cacheWorkspaceSymbols(query, cachedSymbols);
            }
        }

        return LSPError::none();
    } catch (const std::exception& e) {
        return LSPError::make(LSPErrorCode::InternalError, e.what());
    }
}

// ============================================================================
// Async operations
// ============================================================================

void LSPClient::gotoDefinitionAsync(
    const std::string& fileUri,
    LSPPosition position,
    DefinitionCallback callback
) {
    if (!isReady()) {
        callback(
            LSPError::make(LSPErrorCode::ServerNotInitialized, "Not initialized"),
            {}
        );
        return;
    }

    std::thread([this, fileUri, position, callback]() {
        std::vector<LSPLocation> locations;
        LSPError error = gotoDefinition(fileUri, position, locations);
        callback(error, std::move(locations));
    }).detach();
}

void LSPClient::findReferencesAsync(
    const std::string& fileUri,
    LSPPosition position,
    bool includeDeclaration,
    ReferencesCallback callback
) {
    if (!isReady()) {
        callback(
            LSPError::make(LSPErrorCode::ServerNotInitialized, "Not initialized"),
            {}
        );
        return;
    }

    std::thread([this, fileUri, position, includeDeclaration, callback]() {
        std::vector<LSPLocation> locations;
        LSPError error = findReferences(fileUri, position, includeDeclaration, locations);
        callback(error, std::move(locations));
    }).detach();
}

void LSPClient::hoverAsync(
    const std::string& fileUri,
    LSPPosition position,
    HoverCallback callback
) {
    if (!isReady()) {
        callback(
            LSPError::make(LSPErrorCode::ServerNotInitialized, "Not initialized"),
            std::nullopt
        );
        return;
    }

    std::thread([this, fileUri, position, callback]() {
        std::optional<LSPHover> hover;
        LSPError error = this->hover(fileUri, position, hover);
        callback(error, std::move(hover));
    }).detach();
}

void LSPClient::documentSymbolsAsync(
    const std::string& fileUri,
    DocumentSymbolsCallback callback
) {
    if (!isReady()) {
        callback(
            LSPError::make(LSPErrorCode::ServerNotInitialized, "Not initialized"),
            {}
        );
        return;
    }

    std::thread([this, fileUri, callback]() {
        std::vector<LSPDocumentSymbol> symbols;
        LSPError error = documentSymbols(fileUri, symbols);
        callback(error, std::move(symbols));
    }).detach();
}

void LSPClient::workspaceSymbolsAsync(
    const std::string& query,
    WorkspaceSymbolsCallback callback
) {
    if (!isReady()) {
        callback(
            LSPError::make(LSPErrorCode::ServerNotInitialized, "Not initialized"),
            {}
        );
        return;
    }

    std::thread([this, query, callback]() {
        std::vector<LSPSymbolInformation> symbols;
        LSPError error = workspaceSymbols(query, symbols);
        callback(error, std::move(symbols));
    }).detach();
}

// ============================================================================
// Folding Range
// ============================================================================

LSPError LSPClient::foldingRange(
    const std::string& fileUri,
    std::vector<LSPFoldingRange>& outRanges
) {
    if (!isReady()) {
        return LSPError::make(
            LSPErrorCode::ServerNotInitialized,
            "LSP server not initialized"
        );
    }

    // Check cache first
    if (cache_) {
        auto cached = cache_->getCachedFoldingRanges(fileUri);
        if (cached) {
            for (const auto& range : *cached) {
                LSPFoldingRange fold;
                fold.startLine = range.startLine;
                fold.endLine = range.endLine;
                if (!range.kind.empty()) {
                    if (range.kind == "comment") {
                        fold.kind = FoldingRangeKind::Comment;
                    } else if (range.kind == "imports") {
                        fold.kind = FoldingRangeKind::Imports;
                    } else if (range.kind == "region") {
                        fold.kind = FoldingRangeKind::Region;
                    }
                }
                outRanges.push_back(fold);
            }
            return LSPError::none();
        }
    }

    nlohmann::json params = {
        {"textDocument", {{"uri", fileUri}}}
    };

    try {
        LSPResponse response = sendRequest("textDocument/foldingRange", params);

        if (response.error) {
            return *response.error;
        }

        if (response.result && response.result->is_array()) {
            for (const auto& item : *response.result) {
                LSPFoldingRange fold;
                if (item.contains("startLine") && item["startLine"].is_number()) {
                    fold.startLine = item["startLine"].get<int>();
                }
                if (item.contains("endLine") && item["endLine"].is_number()) {
                    fold.endLine = item["endLine"].get<int>();
                }
                if (item.contains("kind") && item["kind"].is_string()) {
                    std::string kindStr = item["kind"].get<std::string>();
                    if (kindStr == "comment") {
                        fold.kind = FoldingRangeKind::Comment;
                    } else if (kindStr == "imports") {
                        fold.kind = FoldingRangeKind::Imports;
                    } else if (kindStr == "region") {
                        fold.kind = FoldingRangeKind::Region;
                    }
                }
                outRanges.push_back(fold);
            }

            // Cache the results
            if (cache_ && !outRanges.empty()) {
                std::vector<db::CachedFoldingRange> cachedRanges;
                for (const auto& fold : outRanges) {
                    db::CachedFoldingRange cached;
                    cached.filePath = fileUri;
                    cached.startLine = fold.startLine;
                    cached.endLine = fold.endLine;
                    if (fold.kind) {
                        cached.kind = foldingRangeKindToString(*fold.kind);
                    }
                    cached.gitSha = currentGitSha_;
                    cachedRanges.push_back(cached);
                }
                cache_->cacheFoldingRanges(fileUri, cachedRanges);
            }
        }

        return LSPError::none();
    } catch (const std::exception& e) {
        return LSPError::make(LSPErrorCode::InternalError, e.what());
    }
}

void LSPClient::foldingRangeAsync(
    const std::string& fileUri,
    FoldingRangeCallback callback
) {
    if (!isReady()) {
        callback(
            LSPError::make(LSPErrorCode::ServerNotInitialized, "Not initialized"),
            {}
        );
        return;
    }

    std::thread([this, fileUri, callback]() {
        std::vector<LSPFoldingRange> ranges;
        LSPError error = foldingRange(fileUri, ranges);
        callback(error, std::move(ranges));
    }).detach();
}

// ============================================================================
// Protocol implementation
// ============================================================================

LSPError LSPClient::sendInitialize() {
    spdlog::debug("[LSPClient] sendInitialize() called");

    nlohmann::json capabilities = {
        {"textDocument", {
            {"definition", {{"linkSupport", true}}},
            {"references", {{"dynamicRegistration", false}}},
            {"hover", {{"contentFormat", {"markdown", "plaintext"}}}},
            {"documentSymbol", {
                {"hierarchicalDocumentSymbolSupport", true}
            }},
            {"foldingRange", {
                {"dynamicRegistration", false},
                {"lineFoldingOnly", true}
            }}
        }},
        {"workspace", {
            {"symbol", {{"dynamicRegistration", false}}}
        }},
        {"window", {
            {"workDoneProgress", true}  // Enable progress notifications
        }},
        {"general", {
            {"progressSupport", true}  // Alternative way some servers check for progress support
        }}
    };

    nlohmann::json params = {
        {"processId", getpid()},
        {"rootUri", "file://" + workspacePath_},
        {"rootPath", workspacePath_},  // Some servers prefer rootPath
        {"capabilities", capabilities}
    };

    spdlog::debug("[LSPClient] Sending initialize request with rootUri: file:// {}", workspacePath_);

    try {
        LSPResponse response = sendRequest("initialize", params);
        spdlog::debug("[LSPClient] Got initialize response");
        if (response.error) {
            spdlog::debug("[LSPClient] Initialize response had error:  {}", response.error->message);
            return *response.error;
        }
        spdlog::debug("[LSPClient] Initialize succeeded");
        return LSPError::none();
    } catch (const std::exception& e) {
        spdlog::debug("[LSPClient] Initialize threw exception:  {}", e.what());
        return LSPError::make(LSPErrorCode::InternalError, e.what());
    }
}

void LSPClient::sendInitialized() {
    sendNotification("initialized", nlohmann::json::object());
}

LSPResponse LSPClient::sendRequest(
    const std::string& method,
    const nlohmann::json& params
) {
    spdlog::debug("[LSPClient] sendRequest() called for method:  {}", method);

    int id = nextRequestId_++;
    spdlog::debug("[LSPClient] Request ID:  {}", id);

    auto pending = std::make_shared<PendingRequest>();
    {
        std::lock_guard<std::mutex> lock(requestsMutex_);
        pendingRequests_[id] = pending;
    }

    LSPRequest request;
    request.id = id;
    request.method = method;
    request.params = params;

    nlohmann::json requestJson = request;
    std::string content = requestJson.dump();
    std::string message = "Content-Length: " + std::to_string(content.size()) +
                          "\r\n\r\n" + content;

    spdlog::debug("[LSPClient] Sending message ({} bytes)", message.size());

    if (!process_->write(message)) {
        spdlog::error("[LSPClient] Failed to write message to process");
        std::lock_guard<std::mutex> lock(requestsMutex_);
        pendingRequests_.erase(id);
        throw std::runtime_error("Failed to send request");
    }

    spdlog::debug("[LSPClient] Message sent, waiting for response...");

    // Wait for response with timeout
    std::future<LSPResponse> future = pending->promise.get_future();

    // Use wait_for to add timeout and logging
    auto status = future.wait_for(std::chrono::seconds(30));
    if (status == std::future_status::timeout) {
        spdlog::error("[LSPClient] Timeout waiting for response to  {}", method);
        std::lock_guard<std::mutex> lock(requestsMutex_);
        pendingRequests_.erase(id);
        throw std::runtime_error("Timeout waiting for LSP response");
    }

    spdlog::debug("[LSPClient] Response received for request  {}", id);
    return future.get();
}

void LSPClient::sendNotification(
    const std::string& method,
    const nlohmann::json& params
) {
    LSPNotification notification;
    notification.method = method;
    notification.params = params;

    nlohmann::json notifJson = notification;
    std::string content = notifJson.dump();
    std::string message = "Content-Length: " + std::to_string(content.size()) +
                          "\r\n\r\n" + content;

    process_->write(message);
}

void LSPClient::handleOutput(const std::string& data) {
    spdlog::debug("[LSPClient] handleOutput() called with {} bytes", data.size());
    std::lock_guard<std::mutex> lock(outputMutex_);
    outputBuffer_ += data;
    spdlog::debug("[LSPClient] Buffer now has {} bytes total", outputBuffer_.size());
    parseMessages(outputBuffer_);
}

void LSPClient::parseMessages(const std::string& content) {
    // Log first 200 chars of buffer for debugging
    if (content.size() > 0) {
        std::string preview = content.substr(0, std::min(content.size(), size_t(300)));
        // Escape control characters for logging
        std::string escaped;
        for (char c : preview) {
            if (c == '\r') escaped += "\\r";
            else if (c == '\n') escaped += "\\n";
            else if (c < 32) escaped += "\\x" + std::to_string((int)(unsigned char)c);
            else escaped += c;
        }
        spdlog::debug("[LSPClient] Buffer preview:  {}", escaped);
    }

    size_t pos = 0;
    while (pos < content.size()) {
        std::string remaining = content.substr(pos);

        // Find Content-Length header - be flexible about line endings
        size_t clPos = remaining.find("Content-Length:");
        if (clPos == std::string::npos) {
            spdlog::debug("[LSPClient] No Content-Length header found");
            break;
        }

        // Extract the content length value
        size_t valueStart = clPos + 15; // length of "Content-Length:"
        while (valueStart < remaining.size() && (remaining[valueStart] == ' ' || remaining[valueStart] == '\t')) {
            valueStart++;
        }

        size_t valueEnd = valueStart;
        while (valueEnd < remaining.size() && std::isdigit(remaining[valueEnd])) {
            valueEnd++;
        }

        if (valueEnd == valueStart) {
            spdlog::error("[LSPClient] Could not parse Content-Length value");
            break;
        }

        size_t contentLength = std::stoull(remaining.substr(valueStart, valueEnd - valueStart));
        spdlog::debug("[LSPClient] Found Content-Length:  {}", contentLength);

        // Find the end of headers (double newline)
        // Support \r\n\r\n, \n\n, or \r\n\n
        size_t headerEnd = std::string::npos;
        size_t searchPos = valueEnd;

        // Look for \r\n\r\n
        size_t crlf2 = remaining.find("\r\n\r\n", searchPos);
        // Look for \n\n
        size_t lf2 = remaining.find("\n\n", searchPos);

        if (crlf2 != std::string::npos && (lf2 == std::string::npos || crlf2 < lf2)) {
            headerEnd = crlf2 + 4;
        } else if (lf2 != std::string::npos) {
            headerEnd = lf2 + 2;
        }

        if (headerEnd == std::string::npos) {
            spdlog::debug("[LSPClient] Header terminator not found yet, waiting for more data");
            break;
        }

        spdlog::debug("[LSPClient] Header ends at position {}, content starts there", headerEnd);

        if (headerEnd + contentLength > remaining.size()) {
            // Incomplete message, wait for more data
            spdlog::debug("[LSPClient] Incomplete message, need {} bytes, have {}", headerEnd + contentLength, remaining.size());
            break;
        }

        std::string jsonStr = remaining.substr(headerEnd, contentLength);
        pos += headerEnd + contentLength;

        spdlog::debug("[LSPClient] Parsing JSON ({} bytes): {}{}", jsonStr.size(), jsonStr.substr(0, std::min(jsonStr.size(), size_t(200))), jsonStr.size() > 200 ? "..." : "");

        try {
            nlohmann::json j = nlohmann::json::parse(jsonStr);

            // Check if this is a notification (has "method" but no "id")
            if (j.contains("method") && !j.contains("id")) {
                std::string method = j["method"].get<std::string>();
                nlohmann::json params = j.contains("params") ? j["params"] : nlohmann::json::object();
                handleNotification(method, params);
            } else {
                // This is a response to a request
                LSPResponse response = j.get<LSPResponse>();
                spdlog::debug("[LSPClient] Parsed response, id= {}", (response.id ? std::to_string(*response.id) : "null"));
                handleResponse(response);
            }
        } catch (const std::exception& e) {
            spdlog::error("[LSPClient] Failed to parse LSP message:  {}", e.what());
        }
    }

    // Keep unparsed data in buffer
    outputBuffer_ = content.substr(pos);
    spdlog::debug("[LSPClient] Remaining unparsed: {} bytes", outputBuffer_.size());
}

void LSPClient::handleResponse(const LSPResponse& response) {
    if (!response.id) {
        // This is a notification, not a response - extract method and params
        // The LSPResponse was parsed from JSON that has "method" field for notifications
        return;
    }

    std::shared_ptr<PendingRequest> pending;
    {
        std::lock_guard<std::mutex> lock(requestsMutex_);
        auto it = pendingRequests_.find(*response.id);
        if (it == pendingRequests_.end()) {
            return;
        }
        pending = it->second;
        pendingRequests_.erase(it);
    }

    pending->promise.set_value(response);
}

void LSPClient::handleNotification(const std::string& method, const nlohmann::json& params) {
    spdlog::debug("[LSPClient] Received notification: {}", method);

    if (method == "$/progress") {
        // Handle progress notifications from rust-analyzer and other LSP servers
        // Progress tokens track indexing/analysis progress
        if (params.contains("token") && params.contains("value")) {
            const auto& value = params["value"];
            std::string tokenStr;
            if (params["token"].is_string()) {
                tokenStr = params["token"].get<std::string>();
            } else if (params["token"].is_number()) {
                tokenStr = std::to_string(params["token"].get<int>());
            }

            if (value.contains("kind")) {
                std::string kind = value["kind"].get<std::string>();

                if (kind == "begin") {
                    // New progress task started
                    activeProgressTokens_++;
                    serverIndexing_ = true;
                    spdlog::debug("[LSPClient] Progress begin (token: {}), active: {}", tokenStr, activeProgressTokens_.load());

                    if (value.contains("title")) {
                        std::string title = value["title"].get<std::string>();
                        spdlog::info("[LSPClient] Server progress: {} (token: {})", title, tokenStr);
                    }
                } else if (kind == "end") {
                    // Progress task finished
                    int remaining = --activeProgressTokens_;
                    spdlog::debug("[LSPClient] Progress end (token: {}), remaining: {}", tokenStr, remaining);

                    if (remaining <= 0) {
                        activeProgressTokens_ = 0;
                        // Don't immediately mark indexing as complete - wait a moment
                        // for any follow-up progress tasks to start
                        // (rust-analyzer often chains multiple progress tasks)
                    }
                } else if (kind == "report") {
                    // Progress update
                    if (value.contains("message")) {
                        spdlog::debug("[LSPClient] Progress report: {}", value["message"].get<std::string>());
                    }
                }
            }
        }
    } else if (method == "window/logMessage" || method == "window/showMessage") {
        // Log messages from server
        if (params.contains("message")) {
            spdlog::debug("[LSPClient] Server message: {}", params["message"].get<std::string>());
        }
    }
}

bool LSPClient::isIndexing() const {
    return serverIndexing_.load();
}

bool LSPClient::waitForIndexing(int timeoutSeconds) {
    spdlog::info("[LSPClient] Waiting for server indexing to complete (timeout: {}s)", timeoutSeconds);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    const int pollIntervalMs = 100;
    const int quietPeriodMs = 1500;  // Consider indexing done after 1.5s of no activity
    int quietTimeMs = 0;
    bool sawProgress = false;

    // Wait for indexing to complete by monitoring progress tokens
    // rust-analyzer sends multiple sequential progress tasks, so we wait for
    // a quiet period where no progress is active
    while (std::chrono::steady_clock::now() < deadline) {
        int activeTokens = activeProgressTokens_.load();

        if (activeTokens > 0) {
            // Still have active progress
            sawProgress = true;
            quietTimeMs = 0;
            spdlog::debug("[LSPClient] Active progress tokens: {}", activeTokens);
        } else if (sawProgress) {
            // No active tokens but we saw progress before - count quiet time
            quietTimeMs += pollIntervalMs;
            if (quietTimeMs >= quietPeriodMs) {
                spdlog::info("[LSPClient] Server indexing complete (quiet for {}ms)", quietTimeMs);
                serverIndexing_ = false;
                return true;
            }
        } else {
            // Haven't seen any progress yet - might be starting up
            quietTimeMs += pollIntervalMs;
            if (quietTimeMs >= 3000) {
                // 3 seconds without any progress - server might not support it
                spdlog::info("[LSPClient] No progress detected after 3s, proceeding");
                return true;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
    }

    spdlog::warn("[LSPClient] Timeout waiting for server indexing (tokens: {})", activeProgressTokens_.load());
    return false;
}

// ============================================================================
// Parsing helpers
// ============================================================================

std::vector<LSPLocation> LSPClient::parseLocations(const nlohmann::json& result) {
    std::vector<LSPLocation> locations;

    if (result.is_array()) {
        for (const auto& item : result) {
            if (auto loc = parseLocation(item)) {
                locations.push_back(*loc);
            }
        }
    } else if (result.is_object()) {
        if (auto loc = parseLocation(result)) {
            locations.push_back(*loc);
        }
    }

    return locations;
}

std::optional<LSPLocation> LSPClient::parseLocation(const nlohmann::json& loc) {
    try {
        // First try standard Location format (uri + range)
        if (loc.contains("uri") && loc.contains("range")) {
            return loc.get<LSPLocation>();
        }

        // Try LocationLink format (targetUri + targetRange)
        // This is what rust-analyzer returns
        if (loc.contains("targetUri") && loc.contains("targetRange")) {
            LSPLocation result;
            result.uri = loc["targetUri"].get<std::string>();
            result.range = loc["targetRange"].get<LSPRange>();
            return result;
        }

        // Fallback: try parsing as Location anyway
        return loc.get<LSPLocation>();
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<LSPHover> LSPClient::parseHover(const nlohmann::json& result) {
    if (result.is_null()) {
        return std::nullopt;
    }

    LSPHover hover;

    // Handle different content formats
    if (result.contains("contents")) {
        const auto& contents = result["contents"];

        if (contents.is_string()) {
            hover.contents = contents.get<std::string>();
        } else if (contents.is_object()) {
            if (contents.contains("value")) {
                hover.contents = contents["value"].get<std::string>();
            }
        } else if (contents.is_array()) {
            std::stringstream ss;
            for (const auto& item : contents) {
                if (item.is_string()) {
                    ss << item.get<std::string>() << "\n";
                } else if (item.is_object() && item.contains("value")) {
                    ss << item["value"].get<std::string>() << "\n";
                }
            }
            hover.contents = ss.str();
        }
    }

    if (result.contains("range")) {
        try {
            hover.range = result["range"].get<LSPRange>();
        } catch (...) {}
    }

    return hover;
}

std::vector<LSPDocumentSymbol> LSPClient::parseDocumentSymbols(const nlohmann::json& result) {
    std::vector<LSPDocumentSymbol> symbols;

    if (!result.is_array()) {
        return symbols;
    }

    for (const auto& item : result) {
        // Check if it's DocumentSymbol (has range) or SymbolInformation (has location)
        if (item.contains("range")) {
            symbols.push_back(parseDocumentSymbol(item));
        } else if (item.contains("location")) {
            // Convert SymbolInformation to DocumentSymbol
            LSPDocumentSymbol sym;
            sym.name = item.value("name", "");
            sym.kind = symbolKindFromInt(item.value("kind", 13));
            if (item.contains("location") && item["location"].contains("range")) {
                sym.range = item["location"]["range"].get<LSPRange>();
                sym.selectionRange = sym.range;
            }
            symbols.push_back(sym);
        }
    }

    return symbols;
}

LSPDocumentSymbol LSPClient::parseDocumentSymbol(const nlohmann::json& sym) {
    LSPDocumentSymbol result;

    result.name = sym.value("name", "");
    result.detail = sym.value("detail", "");
    result.kind = symbolKindFromInt(sym.value("kind", 13));

    if (sym.contains("range")) {
        result.range = sym["range"].get<LSPRange>();
    }
    if (sym.contains("selectionRange")) {
        result.selectionRange = sym["selectionRange"].get<LSPRange>();
    } else {
        result.selectionRange = result.range;
    }

    if (sym.contains("children") && sym["children"].is_array()) {
        for (const auto& child : sym["children"]) {
            result.children.push_back(parseDocumentSymbol(child));
        }
    }

    return result;
}

std::vector<LSPSymbolInformation> LSPClient::parseSymbolInformation(const nlohmann::json& result) {
    std::vector<LSPSymbolInformation> symbols;

    if (!result.is_array()) {
        return symbols;
    }

    for (const auto& item : result) {
        LSPSymbolInformation info;
        info.name = item.value("name", "");
        info.kind = symbolKindFromInt(item.value("kind", 13));
        info.containerName = item.value("containerName", "");

        if (item.contains("location")) {
            info.location = item["location"].get<LSPLocation>();
        }

        symbols.push_back(info);
    }

    return symbols;
}

} // namespace lsp
} // namespace gitreview
