// LSPTypes.hpp
// LSP Protocol type definitions

#ifndef LSP_TYPES_HPP
#define LSP_TYPES_HPP

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <nlohmann/json.hpp>

namespace gitreview {
namespace lsp {

// ============================================================================
// Basic Types
// ============================================================================

struct LSPPosition {
    int line = 0;
    int character = 0;

    bool operator==(const LSPPosition& other) const {
        return line == other.line && character == other.character;
    }
};

struct LSPRange {
    LSPPosition start;
    LSPPosition end;

    bool operator==(const LSPRange& other) const {
        return start == other.start && end == other.end;
    }
};

struct LSPLocation {
    std::string uri;
    LSPRange range;

    bool operator==(const LSPLocation& other) const {
        return uri == other.uri && range == other.range;
    }
};

struct LSPTextDocumentIdentifier {
    std::string uri;
};

struct LSPTextDocumentPositionParams {
    LSPTextDocumentIdentifier textDocument;
    LSPPosition position;
};

// ============================================================================
// Error Handling
// ============================================================================

enum class LSPErrorCode {
    None = 0,
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
    ServerNotInitialized = -32002,
    UnknownErrorCode = -32001,
    RequestCancelled = -32800,
    ContentModified = -32801
};

struct LSPError {
    LSPErrorCode code = LSPErrorCode::None;
    std::string message;

    bool isError() const { return code != LSPErrorCode::None; }
    explicit operator bool() const { return isError(); }

    static LSPError none() { return LSPError{}; }

    static LSPError make(LSPErrorCode code, std::string message) {
        return LSPError{code, std::move(message)};
    }
};

// ============================================================================
// Request/Response Types
// ============================================================================

struct LSPRequest {
    std::string jsonrpc = "2.0";
    int id = 0;
    std::string method;
    nlohmann::json params;
};

struct LSPResponse {
    std::string jsonrpc = "2.0";
    std::optional<int> id;
    std::optional<nlohmann::json> result;
    std::optional<LSPError> error;
};

struct LSPNotification {
    std::string jsonrpc = "2.0";
    std::string method;
    nlohmann::json params;
};

// ============================================================================
// JSON Serialization
// ============================================================================

inline void to_json(nlohmann::json& j, const LSPPosition& p) {
    j = nlohmann::json{{"line", p.line}, {"character", p.character}};
}

inline void from_json(const nlohmann::json& j, LSPPosition& p) {
    j.at("line").get_to(p.line);
    j.at("character").get_to(p.character);
}

inline void to_json(nlohmann::json& j, const LSPRange& r) {
    j = nlohmann::json{{"start", r.start}, {"end", r.end}};
}

inline void from_json(const nlohmann::json& j, LSPRange& r) {
    j.at("start").get_to(r.start);
    j.at("end").get_to(r.end);
}

inline void to_json(nlohmann::json& j, const LSPLocation& l) {
    j = nlohmann::json{{"uri", l.uri}, {"range", l.range}};
}

inline void from_json(const nlohmann::json& j, LSPLocation& l) {
    j.at("uri").get_to(l.uri);
    j.at("range").get_to(l.range);
}

inline void to_json(nlohmann::json& j, const LSPTextDocumentIdentifier& t) {
    j = nlohmann::json{{"uri", t.uri}};
}

inline void to_json(nlohmann::json& j, const LSPTextDocumentPositionParams& p) {
    j = nlohmann::json{
        {"textDocument", p.textDocument},
        {"position", p.position}
    };
}

inline void to_json(nlohmann::json& j, const LSPRequest& r) {
    j = nlohmann::json{
        {"jsonrpc", r.jsonrpc},
        {"id", r.id},
        {"method", r.method}
    };
    if (!r.params.is_null()) {
        j["params"] = r.params;
    }
}

inline void to_json(nlohmann::json& j, const LSPNotification& n) {
    j = nlohmann::json{
        {"jsonrpc", n.jsonrpc},
        {"method", n.method}
    };
    if (!n.params.is_null()) {
        j["params"] = n.params;
    }
}

inline void from_json(const nlohmann::json& j, LSPResponse& r) {
    if (j.contains("jsonrpc")) {
        j.at("jsonrpc").get_to(r.jsonrpc);
    }
    if (j.contains("id") && !j["id"].is_null()) {
        r.id = j["id"].get<int>();
    }
    if (j.contains("result")) {
        r.result = j["result"];
    }
    if (j.contains("error")) {
        LSPError err;
        err.code = static_cast<LSPErrorCode>(j["error"]["code"].get<int>());
        err.message = j["error"]["message"].get<std::string>();
        r.error = err;
    }
}

} // namespace lsp
} // namespace gitreview

#endif // LSP_TYPES_HPP
