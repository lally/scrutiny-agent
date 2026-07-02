// LSPClient.hpp
// C++ LSP Client implementation

#ifndef LSP_CLIENT_HPP
#define LSP_CLIENT_HPP

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <optional>

#include "LSPTypes.hpp"

namespace gitreview {

// Forward declarations
namespace db { class LSPCache; }

namespace lsp {

// Forward declarations
class Process;

enum class Language {
    Unknown = 0,
    Rust,
    Python,
    JavaScript,
    TypeScript,
    Go,
    Cpp,
    C,
    Swift
};

struct LanguageServerConfig {
    std::string executable;
    std::vector<std::string> arguments;
};

// Hover result
struct LSPHover {
    std::string contents;  // Markdown content
    std::optional<LSPRange> range;
};

// Symbol kinds (subset of LSP spec)
enum class SymbolKind {
    File = 1,
    Module = 2,
    Namespace = 3,
    Package = 4,
    Class = 5,
    Method = 6,
    Property = 7,
    Field = 8,
    Constructor = 9,
    Enum = 10,
    Interface = 11,
    Function = 12,
    Variable = 13,
    Constant = 14,
    String = 15,
    Number = 16,
    Boolean = 17,
    Array = 18,
    Object = 19,
    Key = 20,
    Null = 21,
    EnumMember = 22,
    Struct = 23,
    Event = 24,
    Operator = 25,
    TypeParameter = 26
};

std::string symbolKindToString(SymbolKind kind);
SymbolKind symbolKindFromInt(int kind);

// Document symbol (for outline)
struct LSPDocumentSymbol {
    std::string name;
    std::string detail;
    SymbolKind kind;
    LSPRange range;
    LSPRange selectionRange;
    std::vector<LSPDocumentSymbol> children;
};

// Folding range kind
enum class FoldingRangeKind {
    Comment,
    Imports,
    Region
};

std::string foldingRangeKindToString(FoldingRangeKind kind);

// Folding range
struct LSPFoldingRange {
    int startLine = 0;
    int endLine = 0;
    std::optional<FoldingRangeKind> kind;
};

// Workspace symbol
struct LSPSymbolInformation {
    std::string name;
    SymbolKind kind;
    LSPLocation location;
    std::string containerName;
};

class LSPClient {
public:
    using DefinitionCallback = std::function<void(LSPError, std::vector<LSPLocation>)>;
    using ReferencesCallback = std::function<void(LSPError, std::vector<LSPLocation>)>;
    using HoverCallback = std::function<void(LSPError, std::optional<LSPHover>)>;
    using DocumentSymbolsCallback = std::function<void(LSPError, std::vector<LSPDocumentSymbol>)>;
    using WorkspaceSymbolsCallback = std::function<void(LSPError, std::vector<LSPSymbolInformation>)>;
    using FoldingRangeCallback = std::function<void(LSPError, std::vector<LSPFoldingRange>)>;

    explicit LSPClient(std::string workspacePath, Language language);
    ~LSPClient();

    // Non-copyable
    LSPClient(const LSPClient&) = delete;
    LSPClient& operator=(const LSPClient&) = delete;

    // Movable
    LSPClient(LSPClient&&) noexcept;
    LSPClient& operator=(LSPClient&&) noexcept;

    // Cache management
    void setCache(std::shared_ptr<db::LSPCache> cache);
    void setGitSha(const std::string& sha);

    // Lifecycle
    LSPError start();
    void stop();
    bool isReady() const;

    // Document management
    LSPError didOpen(const std::string& fileUri,
                     const std::string& languageId,
                     int version,
                     const std::string& content);

    LSPError didClose(const std::string& fileUri);

    LSPError didChange(const std::string& fileUri,
                       int version,
                       const std::string& content);

    // LSP operations (synchronous)
    LSPError gotoDefinition(const std::string& fileUri,
                            LSPPosition position,
                            std::vector<LSPLocation>& outLocations);

    LSPError findReferences(const std::string& fileUri,
                            LSPPosition position,
                            bool includeDeclaration,
                            std::vector<LSPLocation>& outLocations);

    LSPError hover(const std::string& fileUri,
                   LSPPosition position,
                   std::optional<LSPHover>& outHover);

    LSPError documentSymbols(const std::string& fileUri,
                             std::vector<LSPDocumentSymbol>& outSymbols);

    LSPError workspaceSymbols(const std::string& query,
                              std::vector<LSPSymbolInformation>& outSymbols);

    // LSP operations (asynchronous)
    void gotoDefinitionAsync(const std::string& fileUri,
                             LSPPosition position,
                             DefinitionCallback callback);

    void findReferencesAsync(const std::string& fileUri,
                             LSPPosition position,
                             bool includeDeclaration,
                             ReferencesCallback callback);

    void hoverAsync(const std::string& fileUri,
                    LSPPosition position,
                    HoverCallback callback);

    void documentSymbolsAsync(const std::string& fileUri,
                              DocumentSymbolsCallback callback);

    void workspaceSymbolsAsync(const std::string& query,
                               WorkspaceSymbolsCallback callback);

    // Folding ranges (synchronous)
    LSPError foldingRange(const std::string& fileUri,
                          std::vector<LSPFoldingRange>& outRanges);

    // Folding ranges (asynchronous)
    void foldingRangeAsync(const std::string& fileUri,
                           FoldingRangeCallback callback);

    // Accessors
    Language language() const { return language_; }
    const std::string& workspacePath() const { return workspacePath_; }

    // Wait for server-side indexing to complete (for servers like rust-analyzer)
    // Returns true if indexing completed, false if timed out
    bool waitForIndexing(int timeoutSeconds = 60);

    // Check if server is currently indexing
    bool isIndexing() const;

private:
    // Configuration
    static std::optional<LanguageServerConfig> getServerConfig(Language lang);
    static std::string getLanguageId(Language lang);

    // Protocol methods
    LSPError sendInitialize();
    void sendInitialized();

    // Message handling
    struct PendingRequest {
        std::promise<LSPResponse> promise;
    };

    LSPResponse sendRequest(const std::string& method,
                            const nlohmann::json& params);
    void sendNotification(const std::string& method,
                          const nlohmann::json& params);

    void handleOutput(const std::string& data);
    void parseMessages(const std::string& content);
    void handleResponse(const LSPResponse& response);
    void handleNotification(const std::string& method, const nlohmann::json& params);

    // Parsing helpers
    std::vector<LSPLocation> parseLocations(const nlohmann::json& result);
    std::optional<LSPLocation> parseLocation(const nlohmann::json& loc);
    std::optional<LSPHover> parseHover(const nlohmann::json& result);
    std::vector<LSPDocumentSymbol> parseDocumentSymbols(const nlohmann::json& result);
    LSPDocumentSymbol parseDocumentSymbol(const nlohmann::json& sym);
    std::vector<LSPSymbolInformation> parseSymbolInformation(const nlohmann::json& result);

    // State
    std::string workspacePath_;
    Language language_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::string currentGitSha_;

    // Cache
    std::shared_ptr<db::LSPCache> cache_;

    // Process management
    std::unique_ptr<Process> process_;

    // Request tracking
    std::atomic<int> nextRequestId_{1};
    std::unordered_map<int, std::shared_ptr<PendingRequest>> pendingRequests_;
    std::mutex requestsMutex_;

    // Document tracking
    std::unordered_set<std::string> openDocuments_;
    std::mutex documentsMutex_;

    // Output buffer for partial messages
    std::string outputBuffer_;
    std::mutex outputMutex_;

    // Indexing state tracking (for servers like rust-analyzer)
    std::atomic<bool> serverIndexing_{false};
    std::atomic<int> activeProgressTokens_{0};
    std::mutex indexingMutex_;
    std::condition_variable indexingCondition_;
};

} // namespace lsp
} // namespace gitreview

#endif // LSP_CLIENT_HPP
