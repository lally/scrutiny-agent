// Indexer.hpp
// Full codebase indexing using LSP

#ifndef INDEXER_HPP
#define INDEXER_HPP

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <filesystem>

#include "lsp/LSPClient.hpp"
#include "db/LSPCache.hpp"

namespace gitreview {
namespace indexer {

// Forward declaration
struct IndexDefinition;
struct IndexReference;

// Progress callback: returns false to cancel
using ProgressCallback = std::function<bool(const std::string& filePath, int current, int total)>;

// Completion callback for async operations
using CompleteCallback = std::function<void(
    lsp::LSPError error,
    int filesIndexed,
    int definitionsFound,
    int referencesFound
)>;

// A single definition in the index
struct IndexDefinition {
    std::string name;
    std::string fileUri;
    lsp::SymbolKind kind;
    lsp::LSPRange range;
    std::string containerName;
};

// A single reference in the index
struct IndexReference {
    std::string fromFileUri;
    lsp::LSPRange fromRange;
    std::string toFileUri;
    lsp::LSPRange toRange;
    std::string symbolName;
};

// Result of an indexing operation
struct IndexResult {
    lsp::LSPError error;
    int filesIndexed = 0;
    int definitionsFound = 0;
    int referencesFound = 0;
};

class Indexer {
public:
    Indexer(
        const std::string& workspacePath,
        lsp::Language language,
        std::shared_ptr<db::LSPCache> cache = nullptr
    );
    ~Indexer();

    // Non-copyable
    Indexer(const Indexer&) = delete;
    Indexer& operator=(const Indexer&) = delete;

    // Set file patterns to include (glob patterns)
    void setIncludePatterns(const std::vector<std::string>& patterns);

    // Set file patterns to exclude (glob patterns)
    void setExcludePatterns(const std::vector<std::string>& patterns);

    // Run indexing synchronously
    IndexResult run(ProgressCallback progressCallback = nullptr);

    // Run indexing asynchronously
    void runAsync(
        ProgressCallback progressCallback,
        CompleteCallback completeCallback
    );

    // Cancel ongoing async operation
    void cancel();

    // Check if indexing is running
    bool isRunning() const;

    // Get all definitions
    std::vector<IndexDefinition> getDefinitions() const;

    // Get all references
    std::vector<IndexReference> getReferences() const;

    // Get definitions for a specific file
    std::vector<IndexDefinition> getFileDefinitions(const std::string& fileUri) const;

    // Get references from a specific file
    std::vector<IndexReference> getFileReferences(const std::string& fileUri) const;

    // Get references pointing to a specific file
    std::vector<IndexReference> getReferencesToFile(const std::string& fileUri) const;

private:
    // Collect files to index
    std::vector<std::filesystem::path> collectFiles();

    // Check if file matches include/exclude patterns
    bool shouldIncludeFile(const std::filesystem::path& path) const;

    // Match a path against glob patterns
    bool matchesPatterns(
        const std::filesystem::path& path,
        const std::vector<std::string>& patterns
    ) const;

    // Index a single file
    bool indexFile(
        const std::filesystem::path& filePath,
        lsp::LSPClient& client
    );

    // Read file content
    std::string readFileContent(const std::filesystem::path& path);

    // Get language ID for a file
    std::string getLanguageId(const std::filesystem::path& path) const;

    // Extract identifiers from a line for reference finding
    std::vector<std::pair<std::string, int>> extractIdentifiers(const std::string& line);

    std::string workspacePath_;
    lsp::Language language_;
    std::shared_ptr<db::LSPCache> cache_;

    std::vector<std::string> includePatterns_;
    std::vector<std::string> excludePatterns_;

    mutable std::mutex dataMutex_;
    std::vector<IndexDefinition> definitions_;
    std::vector<IndexReference> references_;

    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
    std::unique_ptr<std::thread> asyncThread_;
};

} // namespace indexer
} // namespace gitreview

#endif // INDEXER_HPP
