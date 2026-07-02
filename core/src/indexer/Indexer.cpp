// Indexer.cpp
// Full codebase indexing implementation

#include "indexer/Indexer.hpp"
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <fnmatch.h>
#include <unordered_set>

namespace gitreview {
namespace indexer {

namespace fs = std::filesystem;

// Default patterns for common languages
static const std::vector<std::string> DEFAULT_INCLUDE_PATTERNS = {
    "*.rs", "*.py", "*.js", "*.ts", "*.jsx", "*.tsx",
    "*.go", "*.c", "*.cpp", "*.cc", "*.cxx", "*.h", "*.hpp", "*.hxx",
    "*.swift", "*.java", "*.rb", "*.php"
};

static const std::vector<std::string> DEFAULT_EXCLUDE_PATTERNS = {
    "node_modules/*", ".git/*", "target/*", "build/*", "dist/*",
    "__pycache__/*", "*.pyc", ".venv/*", "venv/*",
    "vendor/*", ".idea/*", ".vscode/*", "*.min.js", "*.bundle.js"
};

// Convert SymbolKind to int for storage
static int symbolKindToInt(lsp::SymbolKind kind) {
    return static_cast<int>(kind);
}

Indexer::Indexer(
    const std::string& workspacePath,
    lsp::Language language,
    std::shared_ptr<db::LSPCache> cache
)
    : workspacePath_(workspacePath)
    , language_(language)
    , cache_(cache)
    , includePatterns_(DEFAULT_INCLUDE_PATTERNS)
    , excludePatterns_(DEFAULT_EXCLUDE_PATTERNS)
{
    spdlog::debug("[Indexer] Created for workspace: {}", workspacePath);
}

Indexer::~Indexer() {
    cancel();
    if (asyncThread_ && asyncThread_->joinable()) {
        asyncThread_->join();
    }
}

void Indexer::setIncludePatterns(const std::vector<std::string>& patterns) {
    includePatterns_ = patterns;
}

void Indexer::setExcludePatterns(const std::vector<std::string>& patterns) {
    excludePatterns_ = patterns;
}

bool Indexer::isRunning() const {
    return running_.load();
}

void Indexer::cancel() {
    cancelled_.store(true);
}

std::vector<IndexDefinition> Indexer::getDefinitions() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return definitions_;
}

std::vector<IndexReference> Indexer::getReferences() const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    return references_;
}

std::vector<IndexDefinition> Indexer::getFileDefinitions(const std::string& fileUri) const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    std::vector<IndexDefinition> result;
    for (const auto& def : definitions_) {
        if (def.fileUri == fileUri) {
            result.push_back(def);
        }
    }
    return result;
}

std::vector<IndexReference> Indexer::getFileReferences(const std::string& fileUri) const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    std::vector<IndexReference> result;
    for (const auto& ref : references_) {
        if (ref.fromFileUri == fileUri) {
            result.push_back(ref);
        }
    }
    return result;
}

std::vector<IndexReference> Indexer::getReferencesToFile(const std::string& fileUri) const {
    std::lock_guard<std::mutex> lock(dataMutex_);
    std::vector<IndexReference> result;
    for (const auto& ref : references_) {
        if (ref.toFileUri == fileUri) {
            result.push_back(ref);
        }
    }
    return result;
}

IndexResult Indexer::run(ProgressCallback progressCallback) {
    IndexResult result;

    if (running_.exchange(true)) {
        result.error = lsp::LSPError::make(
            lsp::LSPErrorCode::InternalError,
            "Indexer is already running"
        );
        return result;
    }

    cancelled_.store(false);

    // Clear previous data
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        definitions_.clear();
        references_.clear();
    }

    spdlog::info("[Indexer] Starting indexing of {}", workspacePath_);

    // Collect files to index
    auto files = collectFiles();
    int totalFiles = static_cast<int>(files.size());
    spdlog::info("[Indexer] Found {} files to index", totalFiles);

    if (totalFiles == 0) {
        running_.store(false);
        return result;
    }

    // Create LSP client
    auto client = std::make_unique<lsp::LSPClient>(workspacePath_, language_);
    if (cache_) {
        client->setCache(cache_);
    }

    auto startError = client->start();
    if (startError) {
        spdlog::error("[Indexer] Failed to start LSP client: {}", startError.message);
        result.error = startError;
        running_.store(false);
        return result;
    }

    // For languages like Rust, wait for server-side indexing to complete
    // rust-analyzer needs to analyze the Cargo project before references work
    if (language_ == lsp::Language::Rust) {
        spdlog::info("[Indexer] Waiting for rust-analyzer to finish indexing...");
        client->waitForIndexing(30);  // 30 second timeout for initial indexing

        // Pre-open all Rust files so rust-analyzer has full workspace context
        // This is necessary for cross-file references to work
        spdlog::info("[Indexer] Pre-opening all Rust files for workspace analysis...");
        for (const auto& filePath : files) {
            std::string content = readFileContent(filePath);
            if (!content.empty()) {
                std::string fileUri = "file://" + filePath.string();
                std::string languageId = getLanguageId(filePath);
                client->didOpen(fileUri, languageId, 1, content);
            }
        }
        // Give rust-analyzer time to process all files
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // Index each file
    int currentFile = 0;
    for (const auto& filePath : files) {
        if (cancelled_.load()) {
            spdlog::info("[Indexer] Indexing cancelled");
            break;
        }

        currentFile++;

        if (progressCallback) {
            if (!progressCallback(filePath.string(), currentFile, totalFiles)) {
                spdlog::info("[Indexer] Indexing cancelled by callback");
                break;
            }
        }

        if (indexFile(filePath, *client)) {
            result.filesIndexed++;
        }
    }

    // Get final counts
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        result.definitionsFound = static_cast<int>(definitions_.size());
        result.referencesFound = static_cast<int>(references_.size());
    }

    spdlog::info("[Indexer] Indexing complete: {} files, {} definitions, {} references",
        result.filesIndexed, result.definitionsFound, result.referencesFound);

    client->stop();
    running_.store(false);

    return result;
}

void Indexer::runAsync(
    ProgressCallback progressCallback,
    CompleteCallback completeCallback
) {
    if (running_.load()) {
        if (completeCallback) {
            completeCallback(
                lsp::LSPError::make(lsp::LSPErrorCode::InternalError, "Already running"),
                0, 0, 0
            );
        }
        return;
    }

    // Join any previous thread
    if (asyncThread_ && asyncThread_->joinable()) {
        asyncThread_->join();
    }

    asyncThread_ = std::make_unique<std::thread>([this, progressCallback, completeCallback]() {
        auto result = run(progressCallback);
        if (completeCallback) {
            completeCallback(
                result.error,
                result.filesIndexed,
                result.definitionsFound,
                result.referencesFound
            );
        }
    });
}

std::vector<fs::path> Indexer::collectFiles() {
    std::vector<fs::path> files;

    try {
        for (const auto& entry : fs::recursive_directory_iterator(
            workspacePath_,
            fs::directory_options::skip_permission_denied
        )) {
            if (!entry.is_regular_file()) continue;

            if (shouldIncludeFile(entry.path())) {
                files.push_back(entry.path());
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("[Indexer] Error collecting files: {}", e.what());
    }

    // Sort for consistent ordering
    std::sort(files.begin(), files.end());

    return files;
}

bool Indexer::shouldIncludeFile(const fs::path& path) const {
    // Get path relative to workspace
    auto relativePath = fs::relative(path, workspacePath_);
    std::string relativeStr = relativePath.string();
    std::string filename = path.filename().string();

    // Check exclude patterns first
    if (matchesPatterns(relativePath, excludePatterns_)) {
        return false;
    }

    // Check include patterns
    if (!includePatterns_.empty()) {
        return matchesPatterns(relativePath, includePatterns_);
    }

    return true;
}

bool Indexer::matchesPatterns(
    const fs::path& path,
    const std::vector<std::string>& patterns
) const {
    std::string pathStr = path.string();
    std::string filename = path.filename().string();

    for (const auto& pattern : patterns) {
        // Use fnmatch for glob-style matching
        if (fnmatch(pattern.c_str(), pathStr.c_str(), FNM_PATHNAME) == 0) {
            return true;
        }
        // Also check just the filename for simple patterns like "*.py"
        if (fnmatch(pattern.c_str(), filename.c_str(), 0) == 0) {
            return true;
        }
    }

    return false;
}

bool Indexer::indexFile(const fs::path& filePath, lsp::LSPClient& client) {
    spdlog::debug("[Indexer] Indexing file: {}", filePath.string());

    std::string content = readFileContent(filePath);
    if (content.empty()) {
        return false;
    }

    std::string fileUri = "file://" + filePath.string();
    std::string languageId = getLanguageId(filePath);

    if (languageId.empty()) {
        spdlog::debug("[Indexer] Unknown language for file: {}", filePath.string());
        return false;
    }

    // Check if already indexed (in cache)
    if (cache_ && cache_->isFileIndexed(fileUri)) {
        spdlog::debug("[Indexer] File already indexed: {}", filePath.string());
        return true;
    }

    // Open the file in LSP
    auto openError = client.didOpen(fileUri, languageId, 1, content);
    if (openError) {
        spdlog::warn("[Indexer] Failed to open file {}: {}", filePath.string(), openError.message);
        return false;
    }

    // Get document symbols (definitions)
    std::vector<lsp::LSPDocumentSymbol> symbols;
    auto symbolError = client.documentSymbols(fileUri, symbols);

    if (!symbolError) {
        // Convert symbols to definitions and store in cache
        std::function<void(const lsp::LSPDocumentSymbol&, const std::string&)> processSymbol;
        processSymbol = [&](const lsp::LSPDocumentSymbol& sym, const std::string& container) {
            // Store in memory for result tracking
            IndexDefinition def;
            def.name = sym.name;
            def.fileUri = fileUri;
            def.kind = sym.kind;
            def.range = sym.range;
            def.containerName = container;

            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                definitions_.push_back(def);
            }

            // Store in SQLite cache
            if (cache_) {
                db::LSPCache::IndexedDefinition cacheDef;
                cacheDef.name = sym.name;
                cacheDef.fileUri = fileUri;
                cacheDef.kind = symbolKindToInt(sym.kind);
                cacheDef.startLine = sym.range.start.line;
                cacheDef.startChar = sym.range.start.character;
                cacheDef.endLine = sym.range.end.line;
                cacheDef.endChar = sym.range.end.character;
                cacheDef.containerName = container;
                cache_->storeIndexDefinition(cacheDef);
            }

            // Process children
            std::string newContainer = container.empty() ? sym.name : container + "::" + sym.name;
            for (const auto& child : sym.children) {
                processSymbol(child, newContainer);
            }
        };

        for (const auto& sym : symbols) {
            processSymbol(sym, "");
        }
    }

    // Find references by scanning identifiers in the file
    // This is a heuristic approach - we look for identifiers and try to resolve them
    std::istringstream stream(content);
    std::string line;
    int lineNum = 0;

    while (std::getline(stream, line)) {
        auto identifiers = extractIdentifiers(line);

        for (const auto& [ident, col] : identifiers) {
            // Skip very short identifiers or common keywords
            if (ident.length() < 2) continue;

            lsp::LSPPosition pos{lineNum, col};
            std::vector<lsp::LSPLocation> locations;

            auto defError = client.gotoDefinition(fileUri, pos, locations);
            if (!defError && !locations.empty()) {
                // Only record if definition is in a different location
                const auto& defLoc = locations[0];
                if (defLoc.uri != fileUri ||
                    defLoc.range.start.line != lineNum ||
                    defLoc.range.start.character != col) {

                    // Store in memory
                    IndexReference ref;
                    ref.fromFileUri = fileUri;
                    ref.fromRange = lsp::LSPRange{pos, {lineNum, col + static_cast<int>(ident.length())}};
                    ref.toFileUri = defLoc.uri;
                    ref.toRange = defLoc.range;
                    ref.symbolName = ident;

                    {
                        std::lock_guard<std::mutex> lock(dataMutex_);
                        references_.push_back(ref);
                    }

                    // Store in SQLite cache
                    if (cache_) {
                        db::LSPCache::IndexedReference cacheRef;
                        cacheRef.symbolName = ident;
                        cacheRef.fromFileUri = fileUri;
                        cacheRef.fromStartLine = lineNum;
                        cacheRef.fromStartChar = col;
                        cacheRef.fromEndLine = lineNum;
                        cacheRef.fromEndChar = col + static_cast<int>(ident.length());
                        cacheRef.toFileUri = defLoc.uri;
                        cacheRef.toStartLine = defLoc.range.start.line;
                        cacheRef.toStartChar = defLoc.range.start.character;
                        cacheRef.toEndLine = defLoc.range.end.line;
                        cacheRef.toEndChar = defLoc.range.end.character;
                        cache_->storeIndexReference(cacheRef);
                    }
                }
            }
        }

        lineNum++;

        // Check for cancellation periodically
        if (cancelled_.load()) {
            break;
        }
    }

    // Mark file as indexed
    if (cache_) {
        cache_->markFileIndexed(fileUri);
    }

    // Close the file
    client.didClose(fileUri);

    return true;
}

std::string Indexer::readFileContent(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return "";
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::string Indexer::getLanguageId(const fs::path& path) const {
    std::string ext = path.extension().string();

    // Convert to lowercase
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    static const std::unordered_map<std::string, std::string> extToLang = {
        {".rs", "rust"},
        {".py", "python"},
        {".js", "javascript"},
        {".jsx", "javascriptreact"},
        {".ts", "typescript"},
        {".tsx", "typescriptreact"},
        {".go", "go"},
        {".c", "c"},
        {".cpp", "cpp"},
        {".cc", "cpp"},
        {".cxx", "cpp"},
        {".h", "c"},
        {".hpp", "cpp"},
        {".hxx", "cpp"},
        {".swift", "swift"},
        {".java", "java"},
        {".rb", "ruby"},
        {".php", "php"},
    };

    auto it = extToLang.find(ext);
    if (it != extToLang.end()) {
        return it->second;
    }

    return "";
}

std::vector<std::pair<std::string, int>> Indexer::extractIdentifiers(const std::string& line) {
    std::vector<std::pair<std::string, int>> result;

    // Simple regex for identifiers
    static const std::regex identRegex(R"(\b([a-zA-Z_][a-zA-Z0-9_]*)\b)");

    // Keywords to skip
    static const std::unordered_set<std::string> keywords = {
        // C/C++
        "if", "else", "while", "for", "do", "switch", "case", "default",
        "break", "continue", "return", "goto", "sizeof", "typedef",
        "struct", "union", "enum", "const", "static", "extern", "register",
        "volatile", "inline", "void", "char", "short", "int", "long",
        "float", "double", "signed", "unsigned", "auto", "class", "public",
        "private", "protected", "virtual", "override", "final", "new", "delete",
        "try", "catch", "throw", "namespace", "using", "template", "typename",
        "true", "false", "nullptr", "this",
        // Python
        "def", "class", "import", "from", "as", "with", "lambda", "pass",
        "raise", "assert", "yield", "global", "nonlocal", "del", "in", "is",
        "and", "or", "not", "None", "True", "False", "async", "await",
        // JavaScript/TypeScript
        "function", "var", "let", "const", "undefined", "null", "typeof",
        "instanceof", "export", "import", "extends", "implements", "interface",
        // Rust
        "fn", "let", "mut", "pub", "mod", "use", "crate", "self", "super",
        "impl", "trait", "where", "match", "loop", "ref", "move", "dyn",
        // Go
        "func", "package", "import", "type", "interface", "struct", "map",
        "chan", "go", "defer", "select", "range", "fallthrough",
        // Swift
        "func", "var", "let", "class", "struct", "enum", "protocol",
        "extension", "init", "deinit", "guard", "where", "associatedtype",
    };

    auto begin = std::sregex_iterator(line.begin(), line.end(), identRegex);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string ident = (*it)[1].str();
        int pos = static_cast<int>(it->position(1));

        if (keywords.find(ident) == keywords.end()) {
            result.push_back({ident, pos});
        }
    }

    return result;
}

} // namespace indexer
} // namespace gitreview
