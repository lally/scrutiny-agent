// scrutiny-agent
//
// The remote backend process. The Mac app (RemoteBackend) spawns this
// over a user-supplied transport (ssh/tsh/kubectl exec/...) and speaks
// JSON-RPC 2.0 over stdio to it. The agent links GitReviewCore and
// answers Backend operations against the working tree it runs next to.
//
// Phase 2.5 — the concurrency model from docs/protocol.md:
//
//   stdin --> [reader thread] --> [priority work queue: interactive /
//             normal / bulk lanes, per-lane slot caps] --> [bounded
//             worker pool] --> [single priority-aware writer thread] -->
//             stdout
//
// Hard rules implemented here (plan "Two rules that are easy to get
// wrong"):
//   1. Single writer. Every outbound frame goes through ONE writer
//      thread. Worker threads never touch stdout, so frame bytes can
//      never interleave/corrupt.
//   2. Cancellation is wire-level. A `$/cancelRequest` notification
//      marks the request id cancelled; queued work is dropped before it
//      starts and long-running work polls the flag at safe points.
//
// Also: frame-cap negotiation in `meta.hello`, and chunked streaming
// (`rpc.chunk` + a small `streamed` envelope) for any response whose
// serialized body would exceed the negotiated cap.
//
// libgit2/git-manipulation thread-safety: audited (decisions doc 2.5
// hardening). git-manipulation's error state is `thread_local` and its
// libgit2 one-time init is a C++ function-local `static LibGit2Init`
// ("magic static") -- thread-safe-once *if first triggered
// single-threaded*. So at startup, before the pool, we call
// `gm_global_init()` (constructs that static on the main thread) and
// `gm_libgit2_threadsafe()`. When libgit2 has GIT_FEATURE_THREADS, git
// runs fully parallel: each request opens/uses/frees its OWN
// GMRepository on one worker thread, never shared -- exactly the plan's
// "distinct repos are independent". The global git mutex is kept only
// as a defensive fallback for a non-threadsafe libgit2 build.
//
// No Swift. Builds with the same CMake/Conan toolchain as the rest of
// GitReviewCore so it cross-compiles to Linux for the open-source agent.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <deque>
#include <fstream>
#include <sstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>  // _NSGetExecutablePath (self-exec path for askpass)
#endif

#include <map>
#include <memory>
#include <nlohmann/json.hpp>

#include "GitManipBridge.h"
#include "GitReviewCore.h"

using nlohmann::json;

namespace {

// Serialize a value for the wire. `git.showFile` can return whole-file
// bytes that are not valid UTF-8 (binary blobs); a strict dump() would
// throw mid-response. Replace invalid sequences with U+FFFD so emitting
// a response can never fail -- strictly safer than throwing, and for
// text (the StaleThreadAnalyzer use case) byte-identical.
std::string dumpSafe(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// ---- Diagnostics logger (Phase 2.10) ---------------------------------
//
// Config-gated on-disk structured log. Default policy "paths OK,
// contents redacted": method/id/lane/timing/sizes/errors + file/repo
// paths + LSP query strings + resolved LSP server path are logged from
// info/debug; full request/response payloads (incl. fileContent) only
// at trace. Size-capped with a single rotation so a remote home dir
// can't fill. NEVER writes stdout (that is the RPC channel) -- its own
// file fd only. Lines carry id+lane for end-to-end correlation.

enum class LogLevel { Off = 0, Error = 1, Warn = 2, Info = 3, Debug = 4,
                      Trace = 5 };
std::atomic<int> g_logLevel{0};
std::string g_logPath;                          // empty => disabled
std::mutex g_logMu;
constexpr std::size_t kLogRotateBytes = 8 * 1024 * 1024;
std::chrono::steady_clock::time_point g_startTime;

LogLevel parseLogLevel(const std::string& s) {
    if (s == "error") return LogLevel::Error;
    if (s == "warn")  return LogLevel::Warn;
    if (s == "info")  return LogLevel::Info;
    if (s == "debug") return LogLevel::Debug;
    if (s == "trace") return LogLevel::Trace;
    return LogLevel::Off;
}
const char* logLevelName(int l) {
    switch (l) {
        case 1: return "ERROR"; case 2: return "WARN"; case 3: return "INFO";
        case 4: return "DEBUG"; case 5: return "TRACE"; default: return "OFF";
    }
}
inline bool logOn(LogLevel l) {
    return !g_logPath.empty() &&
           static_cast<int>(l) <= g_logLevel.load(std::memory_order_relaxed);
}

void logWrite(LogLevel lvl, const std::string& msg) {
    if (!logOn(lvl)) return;
    char ts[32];
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    ::gmtime_r(&t, &tmv);
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    std::string line = std::string(ts) + " " +
                       logLevelName(static_cast<int>(lvl)) + " " + msg + "\n";
    std::lock_guard<std::mutex> g(g_logMu);
    std::ofstream f(g_logPath, std::ios::app | std::ios::binary);
    if (!f) return;
    f.write(line.data(), static_cast<std::streamsize>(line.size()));
    if (f.good() &&
        static_cast<std::size_t>(f.tellp()) >= kLogRotateBytes) {
        f.close();
        std::string bak = g_logPath + ".1";   // single rotated backup
        std::rename(g_logPath.c_str(), bak.c_str());
    }
}

// Redact content-bearing params unless trace (paths/queries kept).
json redactParams(const json& p) {
    if (g_logLevel.load(std::memory_order_relaxed) >=
        static_cast<int>(LogLevel::Trace))
        return p;
    if (!p.is_object()) return p;
    json c = p;
    for (const char* k : {"fileContent", "content"}) {
        auto it = c.find(k);
        if (it != c.end() && it->is_string())
            *it = "<redacted " +
                  std::to_string(it->get<std::string>().size()) + "B>";
    }
    return c;
}

// Protocol version this agent speaks. Negotiated in meta.hello; a
// mismatch tells the Mac to re-bootstrap a matching binary.
constexpr int kProtocolVersion = 1;

// Injected by the build (agent/CMakeLists.txt); release builds carry
// the git tag so meta.hello reports exactly the released version.
#ifndef SCRUTINY_AGENT_VERSION
#define SCRUTINY_AGENT_VERSION "0.0.0-dev"
#endif
constexpr const char* kAgentVersion = SCRUTINY_AGENT_VERSION;

// Stable error codes (mirror docs/protocol.md "Error model").
enum RpcError : int {
    kInternal       = 1000,
    kNotFound       = 1001,
    kInvalidRequest = 1002,
    kGitFailed      = 1003,
    kLspFailed      = 1004,
    kPermissionDenied = 1005,
    kVersionMismatch = 1006,
    kCancelled      = 1007,
};

// ---- Priority lanes --------------------------------------------------
//
// Lane is a property of the *call* (the Mac may set top-level "lane";
// otherwise a per-method default applies). It governs both worker-pool
// scheduling (per-lane slot caps) and writer scheduling (higher lanes
// preempt at frame boundaries).

enum class Lane { Interactive = 0, Normal = 1, Bulk = 2 };
constexpr int kLaneCount = 3;

const char* laneName(Lane l) {
    return l == Lane::Interactive ? "interactive"
         : l == Lane::Normal ? "normal" : "bulk";
}
// Request-scoped log prefix: "[id=<id> lane=<lane>] "
std::string logTag(const json& id, Lane lane) {
    return "[id=" + id.dump() + " lane=" + laneName(lane) + "] ";
}

Lane laneFromString(const std::string& s, Lane fallback) {
    if (s == "interactive") return Lane::Interactive;
    if (s == "normal")      return Lane::Normal;
    if (s == "bulk")        return Lane::Bulk;
    return fallback;
}

// Per-method default lane. Every method the agent serves today is a
// small interactive read; meta.debug defaults to normal so the test
// harness can explicitly tag lanes.
Lane defaultLaneFor(const std::string& method) {
    if (method == "meta.debug") return Lane::Normal;
    if (method == "index.run" || method == "git.clone" ||
        method == "git.fetch" || method == "git.ensureRepository")
        return Lane::Bulk;                           // long-running / network
    if (method == "cred.selftest") return Lane::Normal;
    return Lane::Interactive;  // incl. cred.provide (must unblock git fast)
}

// ---- Frame size / cap ------------------------------------------------
//
// Negotiated in meta.hello: the Mac proposes, the agent clamps into
// [64 KB, 256 KB] and confirms. Set once before heavy traffic; read by
// the response path on worker threads, hence atomic.

constexpr size_t kMinFrameCap     = 64 * 1024;
constexpr size_t kMaxFrameCap     = 256 * 1024;
constexpr size_t kDefaultFrameCap = 128 * 1024;
std::atomic<size_t> g_frameCap{kDefaultFrameCap};

// ---- Base64 (for rpc.chunk payloads) ---------------------------------

std::string base64Encode(const std::string& in) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned n = (static_cast<unsigned char>(in[i]) << 16) |
                     (static_cast<unsigned char>(in[i + 1]) << 8) |
                     (static_cast<unsigned char>(in[i + 2]));
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back(T[(n >> 6) & 63]);
        out.push_back(T[n & 63]);
    }
    if (i < in.size()) {
        unsigned n = static_cast<unsigned char>(in[i]) << 16;
        bool two = (i + 1 < in.size());
        if (two) n |= static_cast<unsigned char>(in[i + 1]) << 8;
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back(two ? T[(n >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

// ---- Cancellation registry ------------------------------------------
//
// $/cancelRequest marks an id cancelled. Workers consult this before
// starting queued work and (for long ops) at safe points. Keyed by the
// canonical JSON dump of the id so numeric and string ids both work.

class CancelRegistry {
public:
    void cancel(const std::string& key) {
        std::lock_guard<std::mutex> l(m_);
        cancelled_.insert(key);
    }
    bool isCancelled(const std::string& key) {
        std::lock_guard<std::mutex> l(m_);
        return cancelled_.count(key) != 0;
    }
    // Called when a request completes; bounds the set to in-flight ids.
    // A `$/cancelRequest` that arrives after completion leaves a stale
    // key (bounded by the rare cancel-after-done case; acceptable v1).
    void forget(const std::string& key) {
        std::lock_guard<std::mutex> l(m_);
        cancelled_.erase(key);
    }
private:
    std::mutex m_;
    std::unordered_set<std::string> cancelled_;
};

CancelRegistry g_cancel;

// ---- Reading framed input (reader thread only) -----------------------
//
// LSP-style: "Content-Length: <n>\r\n\r\n" then exactly <n> UTF-8 bytes.
// Only the single reader thread calls these.

bool readExact(std::string& out, size_t n) {
    out.resize(n);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(STDIN_FILENO, out.data() + got, n - got);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

bool readHeaderLine(std::string& line) {
    line.clear();
    char c = 0;
    while (true) {
        ssize_t r = ::read(STDIN_FILENO, &c, 1);
        if (r <= 0) return false;
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        line.push_back(c);
    }
}

std::optional<std::string> readFrame() {
    size_t contentLength = 0;
    bool haveLength = false;
    while (true) {
        std::string line;
        if (!readHeaderLine(line)) return std::nullopt;
        if (line.empty()) break;  // blank line terminates headers
        constexpr const char* kCL = "Content-Length:";
        if (line.rfind(kCL, 0) == 0) {
            contentLength = std::strtoul(line.c_str() + std::strlen(kCL), nullptr, 10);
            haveLength = true;
        }
    }
    if (!haveLength) return std::nullopt;
    std::string body;
    if (!readExact(body, contentLength)) return std::nullopt;
    return body;
}

// ---- Writer thread (the single writer) -------------------------------
//
// The ONLY code that writes stdout. Workers hand it fully-serialized
// frames tagged with a lane; at every frame boundary it transmits the
// highest-priority frame that is ready, so an interactive response is
// never stuck behind a multi-chunk bulk body.

class Writer {
public:
    void start() {
        thread_ = std::thread([this] { loop(); });
    }
    // Flush remaining frames and join. Called once, after the reader
    // loop ends and all workers have joined (no more producers).
    void stop() {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    // Enqueue one ready-to-transmit frame body in `lane`.
    void enqueue(Lane lane, std::string payload) {
        {
            std::lock_guard<std::mutex> l(m_);
            q_[static_cast<int>(lane)].push_back(std::move(payload));
        }
        cv_.notify_one();
    }

private:
    bool anyReadyLocked() const {
        for (int i = 0; i < kLaneCount; ++i)
            if (!q_[i].empty()) return true;
        return false;
    }

    void loop() {
        while (true) {
            std::string frame;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return stop_ || anyReadyLocked(); });
                if (!anyReadyLocked()) {
                    // stop_ and fully drained -> exit.
                    return;
                }
                for (int i = 0; i < kLaneCount; ++i) {
                    if (!q_[i].empty()) {
                        frame = std::move(q_[i].front());
                        q_[i].pop_front();
                        break;
                    }
                }
            }
            std::string header =
                "Content-Length: " + std::to_string(frame.size()) + "\r\n\r\n";
            if (::fwrite(header.data(), 1, header.size(), stdout) != header.size() ||
                ::fwrite(frame.data(), 1, frame.size(), stdout) != frame.size()) {
                // Transport gone (SIGPIPE is ignored): nothing more we
                // can deliver. Stop the writer; the process will exit.
                return;
            }
            ::fflush(stdout);
        }
    }

    std::mutex m_;
    std::condition_variable cv_;
    std::array<std::deque<std::string>, kLaneCount> q_;
    bool stop_ = false;
    std::thread thread_;
};

Writer g_writer;

// ---- Response path (chunking) ----------------------------------------
//
// Worker threads build a complete JSON-RPC envelope string and hand it
// here. Within the cap -> one plain frame. Over the cap -> a sequence
// of base64 `rpc.chunk` notifications keyed by id, then a small
// `{ "result": { "streamed": true, "bytes": N } }` envelope. Each chunk
// is enqueued as its own frame so higher-priority frames interleave.

void respondEnvelope(const json& id, Lane lane, const std::string& envelope) {
    const size_t cap = g_frameCap.load();
    if (envelope.size() <= cap) {
        if (logOn(LogLevel::Debug))
            logWrite(LogLevel::Debug, logTag(id, lane) + "resp bytes=" +
                     std::to_string(envelope.size()) + " chunked=false");
        g_writer.enqueue(lane, envelope);
        return;
    }
    const std::string b64 = base64Encode(envelope);
    // Keep each chunk frame comfortably under the cap (the rpc.chunk
    // JSON wrapper is tiny; 4 KB headroom is ample).
    const size_t headroom = 4096;
    const size_t maxData = cap > headroom ? cap - headroom : cap / 2;
    size_t off = 0;
    int seq = 0;
    while (off < b64.size()) {
        size_t take = std::min(maxData, b64.size() - off);
        bool last = (off + take >= b64.size());
        json note = {
            {"jsonrpc", "2.0"},
            {"method", "rpc.chunk"},
            {"params", {{"id", id},
                        {"seq", seq},
                        {"last", last},
                        {"data", b64.substr(off, take)}}},
        };
        g_writer.enqueue(lane, dumpSafe(note));
        off += take;
        ++seq;
    }
    json fin = {{"jsonrpc", "2.0"},
                {"id", id},
                {"result", {{"streamed", true},
                            {"bytes", envelope.size()}}}};
    g_writer.enqueue(lane, dumpSafe(fin));
    if (logOn(LogLevel::Debug))
        logWrite(LogLevel::Debug, logTag(id, lane) + "resp bytes=" +
                 std::to_string(envelope.size()) + " chunked=true chunks=" +
                 std::to_string(seq));
}

// Handle context: carries the id/lane and routes results/errors through
// the chunking response path. Replaces the old free sendResult/sendError.
struct Responder {
    json id;
    std::string idKey;
    Lane lane;

    void result(const json& r) const {
        respondEnvelope(id, lane,
                        dumpSafe(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", r}}));
    }
    void error(int code, const std::string& message) const {
        if (logOn(LogLevel::Warn))
            logWrite(LogLevel::Warn, logTag(id, lane) + "error code=" +
                     std::to_string(code) + " msg=" + message);
        respondEnvelope(
            id, lane,
            dumpSafe(json{{"jsonrpc", "2.0"},
                          {"id", id},
                          {"error", {{"code", code}, {"message", message}}}}));
    }
};

// A null-id error (parse failure) goes straight to the writer at normal
// priority; there is no Responder/lane context for it.
void sendNullIdError(int code, const std::string& message) {
    g_writer.enqueue(
        Lane::Normal,
        dumpSafe(json{{"jsonrpc", "2.0"},
                      {"id", nullptr},
                      {"error", {{"code", code}, {"message", message}}}}));
}

// ---- git serialization ----------------------------------------------
//
// Set once at startup from gm_libgit2_threadsafe() (see file header).
// false (default / threadsafe libgit2): no locking -- each request uses
// its own GMRepository on its own worker thread. true (non-threadsafe
// libgit2 fallback): every gm_* sequence runs under g_gitMutex.
std::atomic<bool> g_serializeGit{false};
std::mutex g_gitMutex;

// Conditionally locks g_gitMutex (only when serialization is required).
struct GitGuard {
    std::unique_lock<std::mutex> lk;
    GitGuard() {
        if (g_serializeGit.load(std::memory_order_relaxed)) {
            lk = std::unique_lock<std::mutex>(g_gitMutex);
        }
    }
};

// ---- Methods ---------------------------------------------------------

// meta.hello { clientVersion, supportedProtocolVersions:[..], frameCap? }
//   -> { agentVersion, protocolVersion, frameCap, capabilities:[..] }
// or VERSION_MISMATCH if the client cannot speak kProtocolVersion.
void handleHello(const Responder& rsp, const json& params) {
    bool ok = false;
    if (auto it = params.find("supportedProtocolVersions");
        it != params.end() && it->is_array()) {
        for (const auto& v : *it) {
            if (v.is_number_integer() && v.get<int>() == kProtocolVersion) ok = true;
        }
    }
    if (!ok) {
        rsp.error(kVersionMismatch,
                  "agent speaks protocol " + std::to_string(kProtocolVersion));
        return;
    }
    // Frame-cap negotiation: clamp the Mac's proposal into range, else
    // keep the default. Single number for the connection lifetime.
    size_t cap = kDefaultFrameCap;
    if (auto it = params.find("frameCap");
        it != params.end() && it->is_number_integer()) {
        long long proposed = it->get<long long>();
        if (proposed < static_cast<long long>(kMinFrameCap)) proposed = kMinFrameCap;
        if (proposed > static_cast<long long>(kMaxFrameCap)) proposed = kMaxFrameCap;
        cap = static_cast<size_t>(proposed);
    }
    g_frameCap.store(cap);
    rsp.result(json{{"agentVersion", kAgentVersion},
                    {"protocolVersion", kProtocolVersion},
                    {"frameCap", cap},
                    {"capabilities", json::array({"git.headSha",
                                                  "git.repoMetadata",
                                                  "git.remotes",
                                                  "git.branches",
                                                  "git.commits",
                                                  "git.aheadBehind",
                                                  "git.diffForCommit",
                                                  "git.workingTreeDiff",
                                                  "git.stagedDiff",
                                                  "git.checkoutBranch",
                                                  "git.showFile",
                                                  "git.diff",
                                                  "git.isAncestor",
                                                  "fs.readFile",
                                                  "fs.listDirectory",
                                                  "lsp.gotoDefinition",
                                                  "lsp.findReferences",
                                                  "lsp.hover",
                                                  "lsp.documentSymbols",
                                                  "lsp.workspaceSymbols",
                                                  "lsp.foldingRange",
                                                  "meta.debug",
                                                  "meta.stat",
                                                  "meta.capabilities",
                                                  "fs.selftest",
                                                  "logs.tail",
                                                  "index.create",
                                                  "index.run",
                                                  "index.cancel",
                                                  "index.destroy",
                                                  "watch.head",
                                                  "watch.stop",
                                                  "git.clone",
                                                  "git.fetch",
                                                  "git.ensureRepository",
                                                  "cred.provide",
                                                  "diffcache.get",
                                                  "diffcache.put",
                                                  "diffcache.prune"})}});
}

// git.headSha { path } -> { headSha }
void handleHeadSha(const Responder& rsp, const json& params) {
    auto it = params.find("path");
    if (it == params.end() || !it->is_string()) {
        rsp.error(kInvalidRequest, "git.headSha requires string param 'path'");
        return;
    }
    const std::string path = it->get<std::string>();
    GitGuard gl;
    GMRepository* repo = gm_repository_open(path.c_str());
    if (repo == nullptr) {
        const char* err = gm_get_last_error();
        rsp.error(kNotFound,
                  std::string("could not open repository at '") + path + "': " +
                      (err ? err : "unknown error"));
        return;
    }
    const char* oid = gm_repository_head_oid(repo);
    if (oid == nullptr) {
        gm_repository_free(repo);
        rsp.error(kGitFailed, "could not resolve HEAD at '" + path + "'");
        return;
    }
    std::string headSha = oid;
    gm_repository_free(repo);
    rsp.result(json{{"headSha", headSha}});
}

// Nullable C string -> JSON string or null.
json strOrNull(const char* s) {
    return s == nullptr ? json(nullptr) : json(std::string(s));
}

// git.repoMetadata { path }
//   -> { path, isBare, headSha, currentBranch, hasUncommittedChanges }
void handleRepoMetadata(const Responder& rsp, const json& params) {
    auto it = params.find("path");
    if (it == params.end() || !it->is_string()) {
        rsp.error(kInvalidRequest, "git.repoMetadata requires string param 'path'");
        return;
    }
    const std::string path = it->get<std::string>();
    GitGuard gl;
    GMRepository* repo = gm_repository_open(path.c_str());
    if (repo == nullptr) {
        const char* err = gm_get_last_error();
        rsp.error(kNotFound,
                  std::string("could not open repository at '") + path + "': " +
                      (err ? err : "unknown error"));
        return;
    }
    json result{
        {"path", strOrNull(gm_repository_path(repo))},
        {"isBare", gm_repository_is_bare(repo)},
        {"headSha", strOrNull(gm_repository_head_oid(repo))},
        {"currentBranch", strOrNull(gm_repository_current_branch(repo))},
        {"hasUncommittedChanges", gm_repository_has_uncommitted_changes(repo)},
    };
    gm_repository_free(repo);
    rsp.result(result);
}

// Open a repo for a `{ path }` request, or send the standard NOT_FOUND
// error and return nullptr. Caller holds a GitGuard and frees the repo.
GMRepository* openForRequest(const Responder& rsp, const json& params,
                             const char* method) {
    auto it = params.find("path");
    if (it == params.end() || !it->is_string()) {
        rsp.error(kInvalidRequest,
                  std::string(method) + " requires string param 'path'");
        return nullptr;
    }
    const std::string path = it->get<std::string>();
    GMRepository* repo = gm_repository_open(path.c_str());
    if (repo == nullptr) {
        const char* err = gm_get_last_error();
        rsp.error(kNotFound,
                  std::string("could not open repository at '") + path + "': " +
                      (err ? err : "unknown error"));
    }
    return repo;
}

// Non-empty C string -> JSON string, else null (mirrors the Swift side,
// which treats an empty upstream/remoteName as nil).
json nonEmptyOrNull(const char* s) {
    return (s == nullptr || *s == '\0') ? json(nullptr) : json(std::string(s));
}

// git.remotes { path } -> { remotes: [ { name, url, pushUrl } ] }
// Wire-honest with LocalBackend/GitRepository.getRemotes (pushUrl falls
// back to the fetch url when unset; entries missing name/url are skipped).
void handleRemotes(const Responder& rsp, const json& params) {
    GitGuard gl;
    GMRepository* repo = openForRequest(rsp, params, "git.remotes");
    if (repo == nullptr) return;
    size_t count = 0;
    GMRemoteInfo* infos = gm_repository_get_remotes(repo, &count);
    json arr = json::array();
    for (size_t i = 0; infos != nullptr && i < count; ++i) {
        GMRemoteInfo* r = gm_remote_info_at_index(infos, i);
        if (r == nullptr) continue;
        const char* name = gm_remote_info_get_name(r);
        const char* url = gm_remote_info_get_url(r);
        if (name == nullptr || url == nullptr) continue;
        const char* push = gm_remote_info_get_push_url(r);
        arr.push_back({{"name", std::string(name)},
                       {"url", std::string(url)},
                       {"pushUrl", std::string(push != nullptr ? push : url)}});
    }
    if (infos != nullptr) gm_remote_info_free(infos, count);
    gm_repository_free(repo);
    rsp.result(json{{"remotes", arr}});
}

// git.branches { path, local, remote }
//   -> { branches: [ { name, refname, targetOid, isRemote, isHead,
//                       upstream|null, remoteName|null } ] }
void handleBranches(const Responder& rsp, const json& params) {
    bool local = true, remote = false;
    if (auto it = params.find("local"); it != params.end() && it->is_boolean())
        local = it->get<bool>();
    if (auto it = params.find("remote"); it != params.end() && it->is_boolean())
        remote = it->get<bool>();
    GitGuard gl;
    GMRepository* repo = openForRequest(rsp, params, "git.branches");
    if (repo == nullptr) return;
    size_t count = 0;
    GMBranchInfo* infos = gm_repository_get_branches(repo, local, remote, &count);
    json arr = json::array();
    for (size_t i = 0; infos != nullptr && i < count; ++i) {
        GMBranchInfo* b = gm_branch_info_at_index(infos, i);
        if (b == nullptr) continue;
        const char* name = gm_branch_info_get_name(b);
        const char* refname = gm_branch_info_get_refname(b);
        const char* target = gm_branch_info_get_target_oid(b);
        if (name == nullptr || refname == nullptr || target == nullptr) continue;
        arr.push_back({{"name", std::string(name)},
                       {"refname", std::string(refname)},
                       {"targetOid", std::string(target)},
                       {"isRemote", gm_branch_info_is_remote(b)},
                       {"isHead", gm_branch_info_is_head(b)},
                       {"upstream", nonEmptyOrNull(gm_branch_info_get_upstream(b))},
                       {"remoteName", nonEmptyOrNull(gm_branch_info_get_remote_name(b))}});
    }
    if (infos != nullptr) gm_branch_info_free(infos, count);
    gm_repository_free(repo);
    rsp.result(json{{"branches", arr}});
}

// git.commits { path, branch?, limit }
//   -> { commits: [ { oid, shortOid, message, summary, authorName,
//                      authorEmail, authorTime (epoch s), parentOids[] } ] }
void handleCommits(const Responder& rsp, const json& params) {
    std::string branch;
    bool haveBranch = false;
    if (auto it = params.find("branch");
        it != params.end() && it->is_string()) {
        branch = it->get<std::string>();
        haveBranch = true;
    }
    long long limit = 100;
    if (auto it = params.find("limit");
        it != params.end() && it->is_number_integer()) {
        limit = it->get<long long>();
    }
    GitGuard gl;
    GMRepository* repo = openForRequest(rsp, params, "git.commits");
    if (repo == nullptr) return;
    size_t count = 0;
    GMCommitInfo* infos = gm_repository_get_commits(
        repo, haveBranch ? branch.c_str() : nullptr,
        static_cast<size_t>(limit < 0 ? 0 : limit), &count);
    json arr = json::array();
    for (size_t i = 0; infos != nullptr && i < count; ++i) {
        GMCommitInfo* c = gm_commit_info_at_index(infos, i);
        if (c == nullptr) continue;
        const char* oid = gm_commit_info_get_oid(c);
        const char* shortOid = gm_commit_info_get_short_oid(c);
        const char* message = gm_commit_info_get_message(c);
        const char* summary = gm_commit_info_get_summary(c);
        const char* an = gm_commit_info_get_author_name(c);
        const char* ae = gm_commit_info_get_author_email(c);
        if (oid == nullptr || shortOid == nullptr || message == nullptr ||
            summary == nullptr || an == nullptr || ae == nullptr)
            continue;
        json parents = json::array();
        size_t pc = gm_commit_info_get_parent_count(c);
        for (size_t j = 0; j < pc; ++j) {
            const char* p = gm_commit_info_get_parent_oid(c, j);
            if (p != nullptr) parents.push_back(std::string(p));
        }
        arr.push_back({{"oid", std::string(oid)},
                       {"shortOid", std::string(shortOid)},
                       {"message", std::string(message)},
                       {"summary", std::string(summary)},
                       {"authorName", std::string(an)},
                       {"authorEmail", std::string(ae)},
                       {"authorTime", static_cast<int64_t>(
                                          gm_commit_info_get_author_time(c))},
                       {"parentOids", parents}});
    }
    if (infos != nullptr) gm_commit_info_free(infos, count);
    gm_repository_free(repo);
    rsp.result(json{{"commits", arr}});
}

// git.aheadBehind { path, branch } -> { ahead, behind }
void handleAheadBehind(const Responder& rsp, const json& params) {
    auto bIt = params.find("branch");
    if (bIt == params.end() || !bIt->is_string()) {
        rsp.error(kInvalidRequest, "git.aheadBehind requires string param 'branch'");
        return;
    }
    const std::string branch = bIt->get<std::string>();
    GitGuard gl;
    GMRepository* repo = openForRequest(rsp, params, "git.aheadBehind");
    if (repo == nullptr) return;
    size_t ahead = 0, behind = 0;
    gm_repository_get_ahead_behind(repo, branch.c_str(), &ahead, &behind);
    gm_repository_free(repo);
    rsp.result(json{{"ahead", static_cast<int64_t>(ahead)},
                    {"behind", static_cast<int64_t>(behind)}});
}

// git.diffForCommit { path, sha }
//   -> { diffs: [ { status (int), oldPath, newPath, patch,
//                    hunks: [ { oldStart, oldLines, newStart, newLines,
//                               header, lines: [ { origin (1-char str),
//                               content, oldLineNo, newLineNo } ] } ] } ] }
// Wire-honest with GitRepository.getDiffForCommit: the raw libgit2
// status int and the raw origin char are sent verbatim; the Mac applies
// the exact same status switch / Origin(rawValue:) mapping (skipping
// unknown origins). An invalid/empty sha yields an empty list (not an
// error envelope) -- LocalBackend returns [] there too, so parity holds.
//
// Shared serializer for a GMFileDiff* array. git.diffForCommit /
// git.workingTreeDiff / git.stagedDiff all return the same wire shape;
// kept in one place so the agent/Mac decoder pair stays single-sourced.
static json serializeFileDiffs(GMFileDiff* diffs, size_t count) {
    json arr = json::array();
    for (size_t i = 0; diffs != nullptr && i < count; ++i) {
        GMFileDiff* d = gm_file_diff_at_index(diffs, i);
        if (d == nullptr) continue;
        const char* oldPath = gm_file_diff_get_old_path(d);
        const char* newPath = gm_file_diff_get_new_path(d);
        const char* patch = gm_file_diff_get_patch(d);
        if (oldPath == nullptr || newPath == nullptr || patch == nullptr) continue;
        json hunks = json::array();
        size_t hc = gm_file_diff_get_hunk_count(d);
        for (size_t j = 0; j < hc; ++j) {
            GMDiffHunk* h = gm_file_diff_get_hunk(d, j);
            if (h == nullptr) continue;
            const char* hdr = gm_diff_hunk_get_header(h);
            json lines = json::array();
            size_t lc = gm_diff_hunk_get_line_count(h);
            for (size_t k = 0; k < lc; ++k) {
                GMDiffLine* ln = gm_diff_hunk_get_line(h, k);
                if (ln == nullptr) continue;
                const char* content = gm_diff_line_get_content(ln);
                lines.push_back(
                    {{"origin", std::string(1, gm_diff_line_get_origin(ln))},
                     {"content", std::string(content != nullptr ? content : "")},
                     {"oldLineNo", gm_diff_line_get_old_lineno(ln)},
                     {"newLineNo", gm_diff_line_get_new_lineno(ln)}});
            }
            hunks.push_back({{"oldStart", gm_diff_hunk_get_old_start(h)},
                             {"oldLines", gm_diff_hunk_get_old_lines(h)},
                             {"newStart", gm_diff_hunk_get_new_start(h)},
                             {"newLines", gm_diff_hunk_get_new_lines(h)},
                             {"header", std::string(hdr != nullptr ? hdr : "")},
                             {"lines", lines}});
        }
        arr.push_back({{"status", gm_file_diff_get_status(d)},
                       {"oldPath", std::string(oldPath)},
                       {"newPath", std::string(newPath)},
                       {"patch", std::string(patch)},
                       {"hunks", hunks}});
    }
    return arr;
}

// git.checkoutBranch { path, branch } -> empty result on success;
// JSON-RPC error envelope (kInternal + libgit2 reason) on failure.
// The same SAFE strategy as on Mac — if the working tree has changes
// that conflict with the target branch, libgit2 refuses rather than
// overwriting. The error string is bubbled verbatim so the user
// sees what's blocking the switch.
void handleCheckoutBranch(const Responder& rsp, const json& params) {
    auto bIt = params.find("branch");
    if (bIt == params.end() || !bIt->is_string()) {
        rsp.error(kInvalidRequest, "git.checkoutBranch requires string param 'branch'");
        return;
    }
    const std::string branch = bIt->get<std::string>();
    GitGuard gl;
    GMRepository* repo = openForRequest(rsp, params, "git.checkoutBranch");
    if (repo == nullptr) return;
    if (!gm_repository_checkout_branch(repo, branch.c_str())) {
        const char* err = gm_get_last_error();
        const std::string msg = err != nullptr && err[0] != '\0'
            ? std::string(err) : std::string("checkout failed");
        gm_repository_free(repo);
        rsp.error(kInternal, msg);
        return;
    }
    gm_repository_free(repo);
    rsp.result(json::object());
}

void handleDiffForCommit(const Responder& rsp, const json& params) {
    auto sIt = params.find("sha");
    if (sIt == params.end() || !sIt->is_string()) {
        rsp.error(kInvalidRequest, "git.diffForCommit requires string param 'sha'");
        return;
    }
    const std::string sha = sIt->get<std::string>();
    GitGuard gl;
    GMRepository* repo = openForRequest(rsp, params, "git.diffForCommit");
    if (repo == nullptr) return;
    size_t count = 0;
    GMFileDiff* diffs = gm_commit_get_diff(repo, sha.c_str(), &count);
    json arr = serializeFileDiffs(diffs, count);
    if (diffs != nullptr) gm_file_diff_free(diffs, count);
    gm_repository_free(repo);
    rsp.result(json{{"diffs", arr}});
}

// git.workingTreeDiff { path }
//   -> same wire shape as git.diffForCommit
// Working-tree diff (index → workdir, including untracked files,
// recursed) — pairs with the boolean in git.repoMetadata. The wire
// format is byte-identical with git.diffForCommit so the Mac decoder
// is shared.
void handleWorkingTreeDiff(const Responder& rsp, const json& params) {
    GitGuard gl;
    GMRepository* repo = openForRequest(rsp, params, "git.workingTreeDiff");
    if (repo == nullptr) return;
    size_t count = 0;
    GMFileDiff* diffs = gm_repository_get_workdir_diff(repo, &count);
    json arr = serializeFileDiffs(diffs, count);
    if (diffs != nullptr) gm_file_diff_free(diffs, count);
    gm_repository_free(repo);
    rsp.result(json{{"diffs", arr}});
}

// git.stagedDiff { path }
//   -> same wire shape as git.diffForCommit
// HEAD-tree → index diff (staged changes). Falls back to empty-tree
// → index for a repo with no HEAD yet. Pairs with git.workingTreeDiff
// to cover every "not yet committed" change with staged-vs-unstaged
// distinguished.
void handleStagedDiff(const Responder& rsp, const json& params) {
    GitGuard gl;
    GMRepository* repo = openForRequest(rsp, params, "git.stagedDiff");
    if (repo == nullptr) return;
    size_t count = 0;
    GMFileDiff* diffs = gm_repository_get_staged_diff(repo, &count);
    json arr = serializeFileDiffs(diffs, count);
    if (diffs != nullptr) gm_file_diff_free(diffs, count);
    gm_repository_free(repo);
    rsp.result(json{{"diffs", arr}});
}

// ---- Raw git utilities (subprocess) ----------------------------------
//
// gitShowFile / gitDiff / gitIsAncestor are the StaleThreadAnalyzer
// primitives. LocalBackend runs `/usr/bin/git` via Process and keys off
// the exit code; the agent mirrors that exactly with a fork/exec runner
// (no shell -> no injection; argv passed verbatim). These are plain
// subprocesses, not libgit2, so no GitGuard is needed (independent
// processes are inherently parallel-safe).

struct GitRun { int exitCode; std::string out; std::string err; };

// Run `git` with optional extra environment ("KEY=VALUE" entries set in
// the child before exec) and separate stdout/stderr capture. `runGit`
// (no env, stderr captured but ignored by its callers) preserves the
// prior behavior for showFile/diff/isAncestor.
// Run `exe` (PATH-resolved) with args/cwd/extra-env, capturing stdout
// and stderr separately. `runGitEnv`/`runGit` are thin "git" wrappers;
// the credential self-test re-execs THIS binary as the askpass child.
GitRun runProc(const std::string& exe, const std::vector<std::string>& args,
                const std::string& cwd, const std::vector<std::string>& env) {
    int outfd[2], errfd[2];
    if (::pipe(outfd) != 0) return {-1, "", ""};
    if (::pipe(errfd) != 0) { ::close(outfd[0]); ::close(outfd[1]);
                              return {-1, "", ""}; }
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(outfd[0]); ::close(outfd[1]);
        ::close(errfd[0]); ::close(errfd[1]);
        return {-1, "", ""};
    }
    if (pid == 0) {
        ::close(outfd[0]); ::close(errfd[0]);
        ::dup2(outfd[1], STDOUT_FILENO); ::close(outfd[1]);
        ::dup2(errfd[1], STDERR_FILENO); ::close(errfd[1]);
        if (::chdir(cwd.c_str()) != 0) _exit(127);
        // setenv after fork (child is single-threaded; immediately
        // exec's) -- the standard, portable askpass-env technique.
        for (const auto& kv : env) {
            auto eq = kv.find('=');
            if (eq == std::string::npos) continue;
            ::setenv(kv.substr(0, eq).c_str(),
                     kv.substr(eq + 1).c_str(), 1);
        }
        std::vector<char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execvp(exe.c_str(), argv.data());
        _exit(127);
    }
    ::close(outfd[1]); ::close(errfd[1]);
    std::string out, errOut;
    // Drain both pipes so neither blocks the child on a full pipe.
    struct pollfd pf[2] = {{outfd[0], POLLIN, 0}, {errfd[0], POLLIN, 0}};
    int open = 2;
    char buf[65536];
    while (open > 0) {
        if (::poll(pf, 2, -1) < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < 2; ++i) {
            if (pf[i].fd < 0) continue;
            if (pf[i].revents & (POLLIN | POLLHUP)) {
                ssize_t r = ::read(pf[i].fd, buf, sizeof(buf));
                if (r > 0) (i == 0 ? out : errOut)
                               .append(buf, static_cast<size_t>(r));
                else { ::close(pf[i].fd); pf[i].fd = -1; --open; }
            }
        }
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (logOn(LogLevel::Debug)) {
        std::string a;
        for (const auto& x : args) a += (a.empty() ? "" : " ") + x;
        logWrite(LogLevel::Debug, exe + " [" + a + "] cwd=" + cwd + " exit=" +
                 std::to_string(code) + " outBytes=" +
                 std::to_string(out.size()) + " errBytes=" +
                 std::to_string(errOut.size()));
    }
    return {code, std::move(out), std::move(errOut)};
}

GitRun runGitEnv(const std::vector<std::string>& args, const std::string& cwd,
                 const std::vector<std::string>& env) {
    return runProc("git", args, cwd, env);
}

GitRun runGit(const std::vector<std::string>& args, const std::string& cwd) {
    return runProc("git", args, cwd, {});
}

// Defined with the LSP section; forward-declared so the fs handler
// (grouped with the git utilities, earlier) can name GRC errors too.
const char* grcErrName(GRCError e);

// Pull a required string param or send INVALID_REQUEST. Returns false
// (and responds) on failure.
bool reqStr(const Responder& rsp, const json& params,
            const char* key, const char* method, std::string& out) {
    auto it = params.find(key);
    if (it == params.end() || !it->is_string()) {
        rsp.error(kInvalidRequest,
                  std::string(method) + " requires string param '" + key + "'");
        return false;
    }
    out = it->get<std::string>();
    return true;
}

// git.showFile { path, sha, file } -> { content: string | null }
// Mirrors LocalBackend.gitShowFile: `git show <sha>:<file>`; stdout on
// exit 0, else null.
void handleShowFile(const Responder& rsp, const json& params) {
    std::string path, sha, file;
    if (!reqStr(rsp, params, "path", "git.showFile", path)) return;
    if (!reqStr(rsp, params, "sha", "git.showFile", sha)) return;
    if (!reqStr(rsp, params, "file", "git.showFile", file)) return;
    GitRun g = runGit({"show", sha + ":" + file}, path);
    rsp.result(json{{"content",
                     g.exitCode == 0 ? json(g.out) : json(nullptr)}});
}

// git.diff { path, from, to, file } -> { diff: string | null }
// Mirrors LocalBackend.gitDiff: `git diff -U3 <from>..<to> -- <file>`.
void handleGitDiff(const Responder& rsp, const json& params) {
    std::string path, from, to, file;
    if (!reqStr(rsp, params, "path", "git.diff", path)) return;
    if (!reqStr(rsp, params, "from", "git.diff", from)) return;
    if (!reqStr(rsp, params, "to", "git.diff", to)) return;
    if (!reqStr(rsp, params, "file", "git.diff", file)) return;
    GitRun g = runGit({"diff", "-U3", from + ".." + to, "--", file}, path);
    rsp.result(json{{"diff", g.exitCode == 0 ? json(g.out) : json(nullptr)}});
}

// git.isAncestor { path, ancestor, descendant } -> { isAncestor: bool }
// Mirrors LocalBackend.gitIsAncestor: `git merge-base --is-ancestor`
// (exit 0 = ancestor, anything else = not).
void handleIsAncestor(const Responder& rsp, const json& params) {
    std::string path, ancestor, descendant;
    if (!reqStr(rsp, params, "path", "git.isAncestor", path)) return;
    if (!reqStr(rsp, params, "ancestor", "git.isAncestor", ancestor)) return;
    if (!reqStr(rsp, params, "descendant", "git.isAncestor", descendant)) return;
    GitRun g = runGit(
        {"merge-base", "--is-ancestor", ancestor, descendant}, path);
    rsp.result(json{{"isAncestor", g.exitCode == 0}});
}

// Defined later (anchors relative roots under $HOME); forward-declared
// so the fs handlers below can resolve paths the same way as
// clone/fetch/diffcache.
std::string resolveRoot(const std::string& p);

// ---- Filesystem sandbox ---------------------------------------------
//
// The agent runs over an authenticated transport, but its user-facing
// promise is "scoped to your clones" -- not "anything readable by the
// remote uid." Before any fs surface touches the kernel, the supplied
// path is (a) anchored under $HOME if relative (resolveRoot, same
// convention as clone/fetch/diffcache), then (b) canonicalized via
// realpath() so every symlink is followed, then (c) checked against a
// configured allowlist. The kernel sees only the canonical path, which
// closes the symlink-swap TOCTOU window: a follow-up rename would
// affect the original input, not the canonical path we hand to read().
//
// Sources of g_allowedRoots (filled once in main() before workers
// start, then read-only -- no locking):
//   * `--allow-root <path>` on the agent's argv (repeatable). Anchored
//     via resolveRoot (so "src" -> "$HOME/src"), realpath()'d, deduped.
//   * If none configured, defaults to [realpath($HOME)]. That is the
//     "at minimum, outside the agent's $HOME" floor from the threat
//     model -- a user who wants to narrow further (e.g. "$HOME/src")
//     does so by passing --allow-root on the host's bootstrap line.
std::vector<std::string> g_allowedRoots;

// True iff `path` is at-or-below `root` on a component boundary. Both
// must be canonical absolutes ('/'-free of '..' and trailing '/'). The
// component boundary matters: "/a/bc" must NOT be considered under
// "/a/b" -- only "/a/b" itself and "/a/b/..." are.
bool isUnderRoot(const std::string& path, const std::string& root) {
    if (root.empty()) return false;
    if (path.size() < root.size()) return false;
    if (path.compare(0, root.size(), root) != 0) return false;
    if (path.size() == root.size()) return true;        // path == root
    if (root.back() == '/') return true;                // root is "/"
    return path[root.size()] == '/';
}

// Anchor + symlink-resolve + allowlist-check. Returns the canonical
// absolute path on success. On failure, fills `err` with a message
// suitable for the wire and sets `code` to NOT_FOUND (path can't be
// resolved on disk) or PERMISSION_DENIED (resolved, but outside every
// allowed root). The returned canonical path is what the caller hands
// to grc_fs_* / directory_iterator -- never the raw input.
std::optional<std::string> resolveAndAuthorize(
    const std::string& userPath, int& code, std::string& err) {
    const std::string anchored = resolveRoot(userPath);
    char real[4096];
    if (::realpath(anchored.c_str(), real) == nullptr) {
        code = kNotFound;
        err = "could not resolve '" + userPath + "': " +
              std::strerror(errno);
        return std::nullopt;
    }
    std::string canon(real);
    for (const auto& root : g_allowedRoots) {
        if (isUnderRoot(canon, root)) return canon;
    }
    code = kPermissionDenied;
    err = "path '" + canon +
          "' is outside the agent's allowed roots (configure with "
          "--allow-root)";
    return std::nullopt;
}

// fs.readFile { path } -> { content }
// Working-tree read via the Core fs surface (grc_fs_read_file). Pure
// I/O, so no GitGuard (the kernel arbitrates; free parallelism). The
// JSON emit is UTF-8-safe (dumpSafe) so binary blobs don't abort the
// response; out_size is authoritative (content may contain NULs).
//
// Sandbox: the canonical (symlink-resolved) path must lie inside a
// configured allowed root. Reads the canonical path -- not the raw
// input -- so a post-check symlink swap can't redirect the read.
void handleFsReadFile(const Responder& rsp, const json& params) {
    std::string path;
    if (!reqStr(rsp, params, "path", "fs.readFile", path)) return;
    int code = 0;
    std::string err;
    auto canon = resolveAndAuthorize(path, code, err);
    if (!canon) {
        rsp.error(code, err);
        return;
    }
    char* buf = nullptr;
    int64_t size = 0;
    GRCError e = grc_fs_read_file(canon->c_str(), &buf, &size);
    if (e != GRC_SUCCESS) {
        rsp.error(e == GRC_ERROR_INVALID_ARGUMENT ? kInvalidRequest
                                                  : kNotFound,
                  "could not read '" + *canon + "': " + grcErrName(e));
        return;
    }
    std::string content(buf, buf + (size > 0 ? static_cast<size_t>(size) : 0));
    grc_free_string(buf);
    rsp.result(json{{"content", content}});
}

// fs.listDirectory { path } -> { path, entries: [{ name, isDir }] }
// Read-only listing for the remote-clone directory browser. Relative
// paths anchor under $HOME (resolveRoot), matching clone/fetch/diffcache.
// The returned `path` is **canonical+absolute** (realpath() resolves
// `.`/`..`/symlinks) so the browser can start at "." (=> the remote
// $HOME) and then do reliable parent/child path math off it.
// `.`/`..` are naturally excluded by directory_iterator; `is_directory`
// follows symlinks (a link to a dir reads as a dir, so the browser can
// descend it) -- parity with LocalBackend (fileExists follows links).
// Entries are sorted by name ascending so local and remote orderings
// match. Pure I/O (kernel arbitrates), so no GitGuard.
//
// Sandbox: same allowlist as fs.readFile -- the directory the browser
// lands on must be inside a configured allowed root. The browser
// cannot peek at /etc or any other tree outside the user's clones.
void handleFsListDirectory(const Responder& rsp, const json& params) {
    std::string path;
    if (!reqStr(rsp, params, "path", "fs.listDirectory", path)) return;
    int code = 0;
    std::string err;
    auto canon = resolveAndAuthorize(path, code, err);
    if (!canon) {
        rsp.error(code, err);
        return;
    }
    path = *canon;
    std::error_code ec;
    std::filesystem::directory_iterator it(path, ec), end;
    if (ec) {
        rsp.error(kNotFound,
                  "could not list '" + path + "': " + ec.message());
        return;
    }
    std::vector<std::pair<std::string, bool>> kids;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code dec;
        bool isDir = it->is_directory(dec);  // follows symlinks
        kids.emplace_back(it->path().filename().string(), isDir);
    }
    std::sort(kids.begin(), kids.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    json entries = json::array();
    for (const auto& k : kids)
        entries.push_back(json{{"name", k.first}, {"isDir", k.second}});
    rsp.result(json{{"path", path}, {"entries", entries}});
}

// ---- LSP session manager (grc_lsp_client_*) --------------------------
//
// Mirrors the Mac's CoreLSPManager: one language-server client per
// (workspace, language), created+started lazily and reused for the
// connection's lifetime (servers are expensive to spawn). Core's
// LSPClient is one-in-flight-per-session, so each client is serialized
// behind its own mutex; distinct clients run in parallel. The SQLite
// LSP cache is deliberately NOT attached here -- decisions doc C8.3 /
// Phase 5 supersede it with a Mac-side RAM LRU, so wiring the
// soon-to-be-removed grc_cache_* path into the agent would be throwaway
// (correctness is unaffected; the cache is a pure perf layer).

struct LspSession {
    GRCLSPClient* client = nullptr;
    std::mutex mu;                             // one in-flight req / session
    std::map<std::string, int32_t> versions;   // file_uri -> last version
};

std::mutex g_lspMu;
std::map<std::string, std::unique_ptr<LspSession>> g_lspSessions;

// Get-or-create a started client. nullptr + errMsg on failure (the
// caller maps it to an LSP_FAILED rpc error). Element pointers from a
// node-based std::map are stable, so returning the raw pointer while
// other sessions are inserted is safe.
const char* grcErrName(GRCError e) {
    switch (e) {
        case GRC_SUCCESS: return "SUCCESS";
        case GRC_ERROR_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case GRC_ERROR_NO_LANGUAGE_SERVER: return "NO_LANGUAGE_SERVER";
        case GRC_ERROR_CONNECTION_FAILED: return "CONNECTION_FAILED";
        case GRC_ERROR_TIMEOUT: return "TIMEOUT";
        case GRC_ERROR_LSP_ERROR: return "LSP_ERROR";
        case GRC_ERROR_NOT_INITIALIZED: return "NOT_INITIALIZED";
        case GRC_ERROR_ALLOCATION_FAILED: return "ALLOCATION_FAILED";
        case GRC_ERROR_DATABASE_ERROR: return "DATABASE_ERROR";
        default: return "UNKNOWN";
    }
}

LspSession* lspSessionFor(const std::string& workspace, int32_t lang,
                          std::string& errMsg) {
    const std::string key = workspace + "\x1f" + std::to_string(lang);
    std::lock_guard<std::mutex> g(g_lspMu);
    auto it = g_lspSessions.find(key);
    if (it != g_lspSessions.end()) return it->second.get();

    // Resolve the configured server path up front: it is the single
    // most useful datum when "why did LSP fail?" -- include it both in
    // the surfaced RPC error (never hide errors) and the log.
    char* sp = grc_language_server_path(static_cast<GRCLanguage>(lang));
    const std::string serverPath = sp != nullptr ? sp : "<none configured>";
    if (sp != nullptr) grc_free_string(sp);

    GRCError err = GRC_SUCCESS;
    GRCLSPClient* client = grc_lsp_client_create(
        workspace.c_str(), static_cast<GRCLanguage>(lang), &err);
    if (client == nullptr || err != GRC_SUCCESS) {
        if (client != nullptr) grc_lsp_client_destroy(client);
        errMsg = "could not create language server (lang=" +
                 std::to_string(lang) + " server='" + serverPath +
                 "' ws='" + workspace + "'): " + grcErrName(err);
        if (logOn(LogLevel::Error))
            logWrite(LogLevel::Error, "lsp session create FAILED " + errMsg);
        return nullptr;
    }
    GRCError serr = grc_lsp_client_start(client);
    if (serr != GRC_SUCCESS) {
        grc_lsp_client_destroy(client);
        errMsg = "language server failed to start (lang=" +
                 std::to_string(lang) + " server='" + serverPath +
                 "' ws='" + workspace + "'): " + grcErrName(serr);
        if (logOn(LogLevel::Error))
            logWrite(LogLevel::Error, "lsp session start FAILED " + errMsg);
        return nullptr;
    }
    auto session = std::make_unique<LspSession>();
    session->client = client;
    LspSession* ptr = session.get();
    g_lspSessions.emplace(key, std::move(session));
    if (logOn(LogLevel::Info))
        logWrite(LogLevel::Info, "lsp session ready lang=" +
                 std::to_string(lang) + " server='" + serverPath +
                 "' ws='" + workspace + "'");
    return ptr;
}

// Sync a document into the server (did_open first time, else
// did_change). Caller holds session->mu.
void lspSyncDoc(LspSession* s, const std::string& uri,
                const std::string& languageId, const std::string& content) {
    auto vit = s->versions.find(uri);
    if (vit == s->versions.end()) {
        s->versions[uri] = 1;
        grc_lsp_client_did_open(s->client, uri.c_str(), languageId.c_str(),
                                1, content.c_str());
    } else {
        int32_t v = ++vit->second;
        grc_lsp_client_did_change(s->client, uri.c_str(), v, content.c_str());
    }
}

// ---- GRC -> JSON (mirrors the Mac's Core* struct fields) -------------

json jPos(const GRCPosition& p) {
    return json{{"line", p.line}, {"character", p.character}};
}
json jRange(const GRCRange& r) {
    return json{{"start", jPos(r.start)}, {"end", jPos(r.end)}};
}
json jLoc(const GRCLocation& l) {
    return json{{"uri", std::string(l.uri != nullptr ? l.uri : "")},
                {"range", jRange(l.range)}};
}
json jDocSym(const GRCDocumentSymbol& s) {
    json children = json::array();
    for (int32_t i = 0; s.children != nullptr && i < s.children_count; ++i)
        children.push_back(jDocSym(s.children[i]));
    return json{{"name", std::string(s.name != nullptr ? s.name : "")},
                {"detail", s.detail != nullptr ? json(std::string(s.detail))
                                               : json(nullptr)},
                {"kind", static_cast<int>(s.kind)},
                {"range", jRange(s.range)},
                {"selectionRange", jRange(s.selection_range)},
                {"children", children}};
}

// Common request params shared by the file-scoped LSP methods.
bool lspCommon(const Responder& rsp, const json& p, const char* method,
               std::string& ws, int32_t& lang, std::string& uri,
               std::string& langId, std::string& content) {
    std::string filePath;
    if (!reqStr(rsp, p, "workspacePath", method, ws)) return false;
    auto lit = p.find("language");
    if (lit == p.end() || !lit->is_number_integer()) {
        rsp.error(kInvalidRequest,
                  std::string(method) + " requires int param 'language'");
        return false;
    }
    lang = lit->get<int32_t>();
    if (!reqStr(rsp, p, "filePath", method, filePath)) return false;
    if (!reqStr(rsp, p, "fileContent", method, content)) return false;
    uri = "file://" + filePath;
    const char* lid = grc_language_id(static_cast<GRCLanguage>(lang));
    langId = lid != nullptr ? lid : "plaintext";
    return true;
}

bool lspPos(const Responder& rsp, const json& p, const char* method,
            GRCPosition& pos) {
    auto l = p.find("line");
    auto c = p.find("character");
    if (l == p.end() || !l->is_number_integer() ||
        c == p.end() || !c->is_number_integer()) {
        rsp.error(kInvalidRequest,
                  std::string(method) + " requires int 'line' and 'character'");
        return false;
    }
    pos.line = l->get<int32_t>();
    pos.character = c->get<int32_t>();
    return true;
}

// lsp.gotoDefinition / lsp.findReferences -> { locations: [...] }
void handleLspLocations(const Responder& rsp, const json& p,
                        const char* method, bool references) {
    std::string ws, uri, langId, content;
    int32_t lang;
    if (!lspCommon(rsp, p, method, ws, lang, uri, langId, content)) return;
    GRCPosition pos{};
    if (!lspPos(rsp, p, method, pos)) return;
    std::string err;
    LspSession* s = lspSessionFor(ws, lang, err);
    if (s == nullptr) { rsp.error(kLspFailed, err); return; }
    std::lock_guard<std::mutex> g(s->mu);
    lspSyncDoc(s, uri, langId, content);
    GRCLocationArray out{};
    GRCError e;
    if (references) {
        bool incl = true;
        if (auto i = p.find("includeDeclaration");
            i != p.end() && i->is_boolean())
            incl = i->get<bool>();
        e = grc_lsp_client_find_references(s->client, uri.c_str(), pos, incl,
                                           &out);
    } else {
        e = grc_lsp_client_goto_definition(s->client, uri.c_str(), pos, &out);
    }
    if (e != GRC_SUCCESS) {
        rsp.error(kLspFailed, std::string(method) + " failed (grc error " +
                                  std::to_string(static_cast<int>(e)) + ")");
        return;
    }
    json arr = json::array();
    for (int32_t i = 0; out.locations != nullptr && i < out.count; ++i)
        arr.push_back(jLoc(out.locations[i]));
    grc_free_locations(&out);
    rsp.result(json{{"locations", arr}});
}

void handleLspHover(const Responder& rsp, const json& p) {
    std::string ws, uri, langId, content;
    int32_t lang;
    if (!lspCommon(rsp, p, "lsp.hover", ws, lang, uri, langId, content)) return;
    GRCPosition pos{};
    if (!lspPos(rsp, p, "lsp.hover", pos)) return;
    std::string err;
    LspSession* s = lspSessionFor(ws, lang, err);
    if (s == nullptr) { rsp.error(kLspFailed, err); return; }
    std::lock_guard<std::mutex> g(s->mu);
    lspSyncDoc(s, uri, langId, content);
    GRCHover h{};
    GRCError e = grc_lsp_client_hover(s->client, uri.c_str(), pos, &h);
    if (e != GRC_SUCCESS) {
        rsp.error(kLspFailed, "lsp.hover failed (grc error " +
                                  std::to_string(static_cast<int>(e)) + ")");
        return;
    }
    json hov;
    if (h.contents == nullptr || *h.contents == '\0') {
        hov = json(nullptr);  // no hover -> null (Mac maps to nil)
    } else {
        hov = json{{"contents", std::string(h.contents)},
                   {"hasRange", h.has_range},
                   {"range", h.has_range ? jRange(h.range) : json(nullptr)}};
    }
    grc_free_hover(&h);
    rsp.result(json{{"hover", hov}});
}

void handleLspDocumentSymbols(const Responder& rsp, const json& p) {
    std::string ws, uri, langId, content;
    int32_t lang;
    if (!lspCommon(rsp, p, "lsp.documentSymbols", ws, lang, uri, langId,
                   content))
        return;
    std::string err;
    LspSession* s = lspSessionFor(ws, lang, err);
    if (s == nullptr) { rsp.error(kLspFailed, err); return; }
    std::lock_guard<std::mutex> g(s->mu);
    lspSyncDoc(s, uri, langId, content);
    GRCDocumentSymbolArray out{};
    GRCError e = grc_lsp_client_document_symbols(s->client, uri.c_str(), &out);
    if (e != GRC_SUCCESS) {
        rsp.error(kLspFailed, "lsp.documentSymbols failed (grc error " +
                                  std::to_string(static_cast<int>(e)) + ")");
        return;
    }
    json arr = json::array();
    for (int32_t i = 0; out.symbols != nullptr && i < out.count; ++i)
        arr.push_back(jDocSym(out.symbols[i]));
    grc_free_document_symbols(&out);
    rsp.result(json{{"symbols", arr}});
}

void handleLspWorkspaceSymbols(const Responder& rsp, const json& p) {
    std::string ws, query;
    if (!reqStr(rsp, p, "workspacePath", "lsp.workspaceSymbols", ws)) return;
    if (!reqStr(rsp, p, "query", "lsp.workspaceSymbols", query)) return;
    auto lit = p.find("language");
    if (lit == p.end() || !lit->is_number_integer()) {
        rsp.error(kInvalidRequest,
                  "lsp.workspaceSymbols requires int param 'language'");
        return;
    }
    int32_t lang = lit->get<int32_t>();
    std::string err;
    LspSession* s = lspSessionFor(ws, lang, err);
    if (s == nullptr) { rsp.error(kLspFailed, err); return; }
    std::lock_guard<std::mutex> g(s->mu);
    GRCSymbolInformationArray out{};
    GRCError e =
        grc_lsp_client_workspace_symbols(s->client, query.c_str(), &out);
    if (e != GRC_SUCCESS) {
        rsp.error(kLspFailed, "lsp.workspaceSymbols failed (grc error " +
                                  std::to_string(static_cast<int>(e)) + ")");
        return;
    }
    json arr = json::array();
    for (int32_t i = 0; out.symbols != nullptr && i < out.count; ++i) {
        const GRCSymbolInformation& si = out.symbols[i];
        arr.push_back(
            {{"name", std::string(si.name != nullptr ? si.name : "")},
             {"kind", static_cast<int>(si.kind)},
             {"location", jLoc(si.location)},
             {"containerName", si.container_name != nullptr
                                   ? json(std::string(si.container_name))
                                   : json(nullptr)}});
    }
    grc_free_symbol_information(&out);
    rsp.result(json{{"symbols", arr}});
}

void handleLspFoldingRange(const Responder& rsp, const json& p) {
    std::string ws, uri, langId, content;
    int32_t lang;
    if (!lspCommon(rsp, p, "lsp.foldingRange", ws, lang, uri, langId, content))
        return;
    std::string err;
    LspSession* s = lspSessionFor(ws, lang, err);
    if (s == nullptr) { rsp.error(kLspFailed, err); return; }
    std::lock_guard<std::mutex> g(s->mu);
    lspSyncDoc(s, uri, langId, content);
    GRCFoldingRangeArray out{};
    GRCError e = grc_lsp_client_folding_range(s->client, uri.c_str(), &out);
    if (e != GRC_SUCCESS) {
        rsp.error(kLspFailed, "lsp.foldingRange failed (grc error " +
                                  std::to_string(static_cast<int>(e)) + ")");
        return;
    }
    json arr = json::array();
    for (int32_t i = 0; out.ranges != nullptr && i < out.count; ++i) {
        const GRCFoldingRange& fr = out.ranges[i];
        arr.push_back({{"startLine", fr.start_line},
                       {"endLine", fr.end_line},
                       {"hasKind", fr.has_kind},
                       {"kind", static_cast<int>(fr.kind)}});
    }
    grc_free_folding_ranges(&out);
    rsp.result(json{{"ranges", arr}});
}

// ---- Workspace indexer (grc_indexer_*) -------------------------------
//
// Maps the Mac's BackendIndexer (runAsync + cancel) onto the agent. The
// index is a *durable, agent-side* store (the plan's Cache-Locality
// table puts the indexer/outline/diff caches on the agent for remote --
// distinct from the LSP-query-result cache that Phase 5 moves to a Mac
// RAM LRU, decisions doc C8.3 / 2.9a). So `index.create` opens a
// grc_cache at the Mac-supplied path *on the agent*. `index.run` is
// bulk-lane and blocks its worker for the whole index (bulk cap=2,
// intended), streaming throttled `index.progress` notifications;
// `index.cancel` (notification) flips the flag + grc_indexer_cancel.

struct IndexerEntry {
    GRCIndexer* idx = nullptr;
    GRCLSPCache* cache = nullptr;
    std::atomic<bool> cancelled{false};
};
std::mutex g_idxMu;
std::map<std::string, std::unique_ptr<IndexerEntry>> g_indexers;
std::atomic<unsigned long> g_idxSeq{0};

struct IdxProgressCtx {
    std::string indexerId;
    std::atomic<bool>* cancelled;
    int lastPct = -1;
};

// C progress callback (called from inside grc_indexer_run on the bulk
// worker). Coalesced to <=~101 notifications (per-percent + the last)
// so a huge repo doesn't flood the wire -- the plan's "coalesce
// progress" intent; full back-pressure is a later phase.
bool idxProgressCb(const char* file, int32_t cur, int32_t total,
                   void* ctxv) {
    auto* c = static_cast<IdxProgressCtx*>(ctxv);
    if (c->cancelled->load()) return false;
    const bool last = (total > 0 && cur >= total);
    const int pct = total > 0
        ? static_cast<int>(static_cast<long long>(cur) * 100 / total) : 0;
    if (pct != c->lastPct || last) {
        c->lastPct = pct;
        json note = {{"jsonrpc", "2.0"},
                     {"method", "index.progress"},
                     {"params", {{"indexerId", c->indexerId},
                                 {"filePath", file != nullptr
                                                  ? std::string(file) : ""},
                                 {"current", cur},
                                 {"total", total}}}};
        g_writer.enqueue(Lane::Bulk, dumpSafe(note));
    }
    return !c->cancelled->load();
}

// index.create { workspacePath, language, cacheDBPath } -> { indexerId }
void handleIndexCreate(const Responder& rsp, const json& p) {
    std::string ws, cacheDBPath;
    if (!reqStr(rsp, p, "workspacePath", "index.create", ws)) return;
    if (!reqStr(rsp, p, "cacheDBPath", "index.create", cacheDBPath)) return;
    auto lit = p.find("language");
    if (lit == p.end() || !lit->is_number_integer()) {
        rsp.error(kInvalidRequest,
                  "index.create requires int param 'language'");
        return;
    }
    const int32_t lang = lit->get<int32_t>();
    // Phase 5c: anchor a relative cacheDBPath under the agent's $HOME
    // (the diffcache/clone/fetch resolveRoot convention) so the durable
    // index lives under the agent cache root, persists across
    // reconnects, and never pollutes the remote working tree's .git.
    // Absolute paths pass through unchanged (back-compat). Ensure the
    // parent dir exists so grc_cache_create can open the db.
    cacheDBPath = resolveRoot(cacheDBPath);
    {
        std::error_code mkec;
        std::filesystem::create_directories(
            std::filesystem::path(cacheDBPath).parent_path(), mkec);
    }
    GRCError ce = GRC_SUCCESS;
    GRCLSPCache* cache = grc_cache_create(cacheDBPath.c_str(), &ce);
    if (cache == nullptr || ce != GRC_SUCCESS) {
        rsp.error(kLspFailed, "index cache open failed at '" + cacheDBPath +
                                  "': " + grcErrName(ce));
        return;
    }
    GRCError ie = GRC_SUCCESS;
    GRCIndexer* idx = grc_indexer_create(
        ws.c_str(), static_cast<GRCLanguage>(lang), cache, &ie);
    if (idx == nullptr || ie != GRC_SUCCESS) {
        grc_cache_destroy(cache);
        rsp.error(kLspFailed, "indexer create failed (ws='" + ws +
                                  "' lang=" + std::to_string(lang) + "): " +
                                  grcErrName(ie));
        return;
    }
    const std::string id =
        "idx-" + std::to_string(g_idxSeq.fetch_add(1) + 1);
    auto e = std::make_unique<IndexerEntry>();
    e->idx = idx;
    e->cache = cache;
    {
        std::lock_guard<std::mutex> g(g_idxMu);
        g_indexers.emplace(id, std::move(e));
    }
    if (logOn(LogLevel::Info))
        logWrite(LogLevel::Info, "index.create id=" + id + " lang=" +
                 std::to_string(lang) + " ws='" + ws + "' cacheDB='" +
                 cacheDBPath + "'");
    rsp.result(json{{"indexerId", id}});
}

IndexerEntry* indexerById(const std::string& id) {
    std::lock_guard<std::mutex> g(g_idxMu);
    auto it = g_indexers.find(id);
    return it == g_indexers.end() ? nullptr : it->second.get();
}

// index.run { indexerId } -> { filesIndexed, definitionsFound,
//   referencesFound }  (bulk lane; blocks the worker for the index)
void handleIndexRun(const Responder& rsp, const json& p) {
    std::string id;
    if (!reqStr(rsp, p, "indexerId", "index.run", id)) return;
    IndexerEntry* e = indexerById(id);
    if (e == nullptr) {
        rsp.error(kNotFound, "unknown indexerId '" + id + "'");
        return;
    }
    e->cancelled.store(false);
    IdxProgressCtx ctx{id, &e->cancelled, -1};
    int32_t fi = 0, df = 0, rf = 0;
    GRCError re = grc_indexer_run(e->idx, idxProgressCb, &ctx, &fi, &df, &rf);
    if (re != GRC_SUCCESS) {
        rsp.error(kLspFailed, "index.run failed: " + std::string(grcErrName(re)) +
                                  (e->cancelled.load() ? " (cancelled)" : ""));
        return;
    }
    if (logOn(LogLevel::Info))
        logWrite(LogLevel::Info, "index.run done id=" + id + " files=" +
                 std::to_string(fi) + " defs=" + std::to_string(df) +
                 " refs=" + std::to_string(rf));
    rsp.result(json{{"filesIndexed", fi},
                    {"definitionsFound", df},
                    {"referencesFound", rf}});
}

// index.destroy { indexerId } -> { ok }  (refused while running, to
// avoid freeing an entry a bulk worker is still inside; the real Mac
// flow only destroys after completion).
void handleIndexDestroy(const Responder& rsp, const json& p) {
    std::string id;
    if (!reqStr(rsp, p, "indexerId", "index.destroy", id)) return;
    std::lock_guard<std::mutex> g(g_idxMu);
    auto it = g_indexers.find(id);
    if (it == g_indexers.end()) {
        rsp.result(json{{"ok", true}});  // already gone: idempotent
        return;
    }
    if (grc_indexer_is_running(it->second->idx)) {
        it->second->cancelled.store(true);
        grc_indexer_cancel(it->second->idx);
        rsp.result(json{{"ok", false}, {"running", true}});
        return;
    }
    grc_indexer_destroy(it->second->idx);
    grc_cache_destroy(it->second->cache);
    g_indexers.erase(it);
    rsp.result(json{{"ok", true}});
}

// ---- HEAD watch (grc_watch_*) ----------------------------------------
//
// Mirrors the Mac's BranchHeadWatcher over the wire: watch.head opens a
// Core HEAD watch; each external HEAD change streams a debounced
// watch.headChanged notification; watch.stop (notification, like
// index.cancel) tears it down. The Core callback fires on a watcher
// thread -> g_writer.enqueue is thread-safe (single writer drains).

struct WatchEntry {
    GRCWatch* w = nullptr;
    std::string watchId;
};
std::mutex g_watchMu;
std::map<std::string, std::unique_ptr<WatchEntry>> g_watches;
std::atomic<unsigned long> g_watchSeq{0};

void watchHeadCb(void* ctxv) {
    auto* e = static_cast<WatchEntry*>(ctxv);
    json note = {{"jsonrpc", "2.0"},
                 {"method", "watch.headChanged"},
                 {"params", {{"watchId", e->watchId}}}};
    // Interactive lane: a tiny, latency-sensitive UI-refresh signal.
    g_writer.enqueue(Lane::Interactive, dumpSafe(note));
    if (logOn(LogLevel::Debug))
        logWrite(LogLevel::Debug, "watch.headChanged id=" + e->watchId);
}

// watch.head { path } -> { watchId }
void handleWatchHead(const Responder& rsp, const json& p) {
    std::string path;
    if (!reqStr(rsp, p, "path", "watch.head", path)) return;
    const std::string id = "w-" + std::to_string(g_watchSeq.fetch_add(1) + 1);
    auto e = std::make_unique<WatchEntry>();
    e->watchId = id;
    WatchEntry* ptr = e.get();
    {
        std::lock_guard<std::mutex> g(g_watchMu);
        g_watches.emplace(id, std::move(e));
    }
    GRCWatch* w = grc_watch_head_create(path.c_str(), watchHeadCb, ptr);
    if (w == nullptr) {
        std::lock_guard<std::mutex> g(g_watchMu);
        g_watches.erase(id);
        rsp.error(kNotFound,
                  "could not resolve HEAD to watch at '" + path + "'");
        return;
    }
    ptr->w = w;
    if (logOn(LogLevel::Info))
        logWrite(LogLevel::Info, "watch.head id=" + id + " path='" + path + "'");
    rsp.result(json{{"watchId", id}});
}

// Stop+destroy a watch by id (joins the Core watcher thread; prompt via
// its self-pipe). Shared by the watch.stop notification and shutdown.
void stopWatch(const std::string& id) {
    std::unique_ptr<WatchEntry> e;
    {
        std::lock_guard<std::mutex> g(g_watchMu);
        auto it = g_watches.find(id);
        if (it == g_watches.end()) return;
        e = std::move(it->second);
        g_watches.erase(it);
    }
    grc_watch_destroy(e->w);  // outside the registry lock (it joins)
    if (logOn(LogLevel::Info))
        logWrite(LogLevel::Info, "watch.stop id=" + id);
}

// ---- Credential broker (Phase 2.13; "tokens stay on the Mac") --------
//
// git on the agent NEVER receives a token in its URL/config/env. Each
// op runs git with GIT_ASKPASS = this binary (askpass mode) + a private
// per-op unix socket. When git prompts, the askpass child connects to
// the socket; the broker emits a `cred.request` notification and waits
// for the Mac's `cred.provide`, then hands the secret back. The token
// thus exists on the agent ONLY transiently in the broker<->askpass
// pipe for the git child -- never on disk, never in a git.* RPC param,
// never in git config.

std::string g_selfExe;  // absolute path to this binary (set in main)

struct PendingCred {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::string value;
};
std::mutex g_credMu;
std::map<std::string, std::shared_ptr<PendingCred>> g_creds;
std::atomic<unsigned long> g_credSeq{0};

// cred.provide { credId, value } -> { ok }: the Mac's answer to a
// cred.request; unblocks the waiting broker connection.
void handleCredProvide(const Responder& rsp, const json& p) {
    std::string credId;
    if (!reqStr(rsp, p, "credId", "cred.provide", credId)) return;
    std::string value;
    if (auto it = p.find("value"); it != p.end() && it->is_string())
        value = it->get<std::string>();
    std::shared_ptr<PendingCred> pc;
    {
        std::lock_guard<std::mutex> g(g_credMu);
        auto it = g_creds.find(credId);
        if (it != g_creds.end()) pc = it->second;
    }
    if (pc) {
        {
            std::lock_guard<std::mutex> l(pc->m);
            pc->value = value;
            pc->done = true;
        }
        pc->cv.notify_all();
    }
    rsp.result(json{{"ok", pc != nullptr}});
}

// Per-op unix-socket askpass broker.
struct CredBroker {
    std::string authOpId;
    std::string sockPath;
    int listenFd = -1;
    std::atomic<bool> running{true};
    std::thread th;

    bool start(const std::string& opId, const std::string& dir) {
        authOpId = opId;
        sockPath = dir + "/.scrutiny-cred-" +
                   std::to_string(g_credSeq.fetch_add(1) + 1) + ".sock";
        ::unlink(sockPath.c_str());
        listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listenFd < 0) return false;
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (sockPath.size() >= sizeof(addr.sun_path)) {
            ::close(listenFd); listenFd = -1; return false;
        }
        std::strncpy(addr.sun_path, sockPath.c_str(),
                     sizeof(addr.sun_path) - 1);
        if (::bind(listenFd, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) != 0 ||
            ::listen(listenFd, 4) != 0) {
            ::close(listenFd); listenFd = -1; return false;
        }
        ::chmod(sockPath.c_str(), 0600);
        th = std::thread([this] { loop(); });
        return true;
    }

    void loop() {
        while (running.load()) {
            struct pollfd pf{listenFd, POLLIN, 0};
            if (::poll(&pf, 1, 250) <= 0) continue;  // timeout: re-check
            int c = ::accept(listenFd, nullptr, nullptr);
            if (c < 0) continue;
            serve(c);
            ::close(c);
        }
    }

    // askpass child wrote "<prompt>\n"; broker to the Mac; reply
    // "<value>\n".
    void serve(int c) {
        std::string prompt;
        char ch;
        while (::read(c, &ch, 1) == 1 && ch != '\n') prompt.push_back(ch);
        const std::string credId =
            "c-" + std::to_string(g_credSeq.fetch_add(1) + 1);
        auto pc = std::make_shared<PendingCred>();
        {
            std::lock_guard<std::mutex> g(g_credMu);
            g_creds[credId] = pc;
        }
        json note = {{"jsonrpc", "2.0"},
                     {"method", "cred.request"},
                     {"params", {{"authOpId", authOpId},
                                 {"credId", credId},
                                 {"prompt", prompt}}}};
        g_writer.enqueue(Lane::Interactive, dumpSafe(note));
        std::string value;
        {
            std::unique_lock<std::mutex> l(pc->m);
            pc->cv.wait_for(l, std::chrono::seconds(120),
                            [&] { return pc->done; });
            value = pc->value;
        }
        {
            std::lock_guard<std::mutex> g(g_credMu);
            g_creds.erase(credId);
        }
        value.push_back('\n');
        ssize_t w = ::write(c, value.data(), value.size());
        (void)w;
    }

    void stop() {
        running.store(false);
        if (th.joinable()) th.join();
        if (listenFd >= 0) { ::close(listenFd); listenFd = -1; }
        ::unlink(sockPath.c_str());
    }
};

std::vector<std::string> credEnv(const std::string& sock) {
    return {"GIT_ASKPASS=" + g_selfExe,
            "SCRUTINY_CRED_SOCK=" + sock,
            "GIT_TERMINAL_PROMPT=0"};
}

std::string repoKey(const std::string& fullName) {
    std::string k = fullName;
    for (auto& ch : k) if (ch == '/') ch = '_';
    return k;
}

// Resolve a Mac-supplied root. The agent's cwd is undefined (it's
// exec'd over an arbitrary transport), so a relative root is anchored
// to $HOME (the plan's "~/.scrutiny/..." model) -- never the cwd.
// Absolute paths pass through. Used for cache/repo roots so clone/
// fetch/diffcache land in a stable, writable, per-host location.
std::string resolveRoot(const std::string& p) {
    if (!p.empty() && p[0] == '/') return p;
    const char* h = ::getenv("HOME");
    const std::string base = (h != nullptr && *h != '\0') ? h : "/tmp";
    return base + "/" + p;
}

// Run a credential-brokered git op (clone/fetch). Returns the GitRun;
// sets `err` for the caller's error message.
GitRun runGitBrokered(const std::vector<std::string>& args,
                      const std::string& cwd, const std::string& authOpId) {
    CredBroker br;
    if (!br.start(authOpId, cwd)) return {-1, "", "cred broker setup failed"};
    GitRun g = runGitEnv(args, cwd, credEnv(br.sockPath));
    br.stop();
    return g;
}

// git.clone { fullName, cloneURL, installDir, authOpId }
//   -> { localPath, cloneURL, lastFetched }
void handleGitClone(const Responder& rsp, const json& p) {
    std::string fullName, cloneURL, installDir, authOpId;
    if (!reqStr(rsp, p, "fullName", "git.clone", fullName)) return;
    if (!reqStr(rsp, p, "cloneURL", "git.clone", cloneURL)) return;
    if (!reqStr(rsp, p, "installDir", "git.clone", installDir)) return;
    if (!reqStr(rsp, p, "authOpId", "git.clone", authOpId)) return;
    installDir = resolveRoot(installDir);
    std::error_code ec;
    std::filesystem::create_directories(installDir, ec);
    const std::string dest = installDir + "/" + repoKey(fullName);
    std::filesystem::remove_all(dest, ec);  // fresh clone
    GitRun g = runGitBrokered({"clone", "--progress", cloneURL, dest},
                              installDir, authOpId);
    if (g.exitCode != 0) {
        rsp.error(kGitFailed, "git clone failed (exit " +
                  std::to_string(g.exitCode) + "): " + g.err.substr(0, 600));
        return;
    }
    rsp.result(json{{"localPath", dest},
                    {"cloneURL", cloneURL},
                    {"lastFetched", static_cast<int64_t>(::time(nullptr))}});
}

// git.fetch { repoPath, authOpId } -> { ok }
void handleGitFetch(const Responder& rsp, const json& p) {
    std::string repoPath, authOpId;
    if (!reqStr(rsp, p, "repoPath", "git.fetch", repoPath)) return;
    if (!reqStr(rsp, p, "authOpId", "git.fetch", authOpId)) return;
    repoPath = resolveRoot(repoPath);
    GitRun g = runGitBrokered({"fetch", "--all", "--prune"}, repoPath,
                              authOpId);
    if (g.exitCode != 0) {
        rsp.error(kGitFailed, "git fetch failed (exit " +
                  std::to_string(g.exitCode) + "): " + g.err.substr(0, 600));
        return;
    }
    rsp.result(json{{"ok", true},
                    {"lastFetched", static_cast<int64_t>(::time(nullptr))}});
}

// git.ensureRepository { fullName, cloneURL, installDir, authOpId }
//   -> { localPath, cloneURL, lastFetched } : fetch if present else clone.
void handleEnsureRepository(const Responder& rsp, const json& p) {
    std::string fullName, cloneURL, installDir, authOpId;
    if (!reqStr(rsp, p, "fullName", "git.ensureRepository", fullName)) return;
    if (!reqStr(rsp, p, "cloneURL", "git.ensureRepository", cloneURL)) return;
    if (!reqStr(rsp, p, "installDir", "git.ensureRepository", installDir)) return;
    if (!reqStr(rsp, p, "authOpId", "git.ensureRepository", authOpId)) return;
    installDir = resolveRoot(installDir);
    std::error_code ec;
    std::filesystem::create_directories(installDir, ec);
    const std::string dest = installDir + "/" + repoKey(fullName);
    const bool present =
        std::filesystem::exists(dest + "/.git", ec) ||
        std::filesystem::exists(dest + "/HEAD", ec);  // bare
    GitRun g = present
        ? runGitBrokered({"fetch", "--all", "--prune"}, dest, authOpId)
        : runGitBrokered({"clone", "--progress", cloneURL, dest},
                         installDir, authOpId);
    if (g.exitCode != 0) {
        rsp.error(kGitFailed,
                  std::string(present ? "git fetch" : "git clone") +
                  " failed (exit " + std::to_string(g.exitCode) + "): " +
                  g.err.substr(0, 600));
        return;
    }
    rsp.result(json{{"localPath", dest},
                    {"cloneURL", cloneURL},
                    {"lastFetched", static_cast<int64_t>(::time(nullptr))}});
}

// cred.selftest { authOpId, prompt } -> { got }
//
// Deterministic, network/git-free proof of the whole credential path:
// stand up a broker, re-exec THIS binary in --askpass mode against it
// with `prompt`, and return what the askpass child printed -- which it
// could only have obtained via the broker -> cred.request -> Mac ->
// cred.provide round-trip. The token never appears in any param.
void handleCredSelftest(const Responder& rsp, const json& p) {
    std::string authOpId, prompt;
    if (!reqStr(rsp, p, "authOpId", "cred.selftest", authOpId)) return;
    if (!reqStr(rsp, p, "prompt", "cred.selftest", prompt)) return;
    CredBroker br;
    if (!br.start(authOpId, "/tmp")) {
        rsp.error(kInternal, "cred broker setup failed");
        return;
    }
    GitRun r = runProc(g_selfExe, {prompt}, "/tmp",
                       {"SCRUTINY_CRED_SOCK=" + br.sockPath});
    br.stop();
    std::string got = r.out;
    while (!got.empty() && (got.back() == '\n' || got.back() == '\r'))
        got.pop_back();
    rsp.result(json{{"got", got}, {"askpassExit", r.exitCode}});
}

// askpass mode: git invoked us as `$GIT_ASKPASS "<prompt>"` with
// SCRUTINY_CRED_SOCK set. Connect to that socket, send the prompt, read
// the secret, print it for git. Any failure -> exit 1 (git, with
// GIT_TERMINAL_PROMPT=0, then fails auth cleanly -- never hangs).
int askpassMain(const char* sockPath, const char* prompt) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sockPath, sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 1;
    }
    std::string req(prompt);
    req.push_back('\n');
    if (::write(fd, req.data(), req.size()) < 0) { ::close(fd); return 1; }
    std::string val;
    char ch;
    while (::read(fd, &ch, 1) == 1 && ch != '\n') val.push_back(ch);
    ::close(fd);
    val.push_back('\n');
    ssize_t w = ::write(STDOUT_FILENO, val.data(), val.size());
    (void)w;
    return 0;
}

// ---- Agent-side DiffCache (Phase 2.14) -------------------------------
//
// The plan's Cache-Locality table puts DiffCache on the agent ("per
// host; diffs depend on git data that lives on the agent; immutable by
// (fromSha,toSha,file) so it never invalidates"). A content-addressed
// *filesystem* cache is the right fit for immutable blobs: no DB code
// in the agent, crash-safe via atomic write+rename, prune = delete by
// mtime. Key = sha256(fromSha\x1f toSha\x1f file) so the filename is
// fixed-length and collision-free regardless of path depth. The agent
// stores the Mac's CachedDiff JSON verbatim (it never parses it).

// Minimal, dependency-free SHA-256 (cache-key only) -> 64-hex.
std::string sha256hex(const std::string& in) {
    auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
        0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
        0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
        0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
        0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
        0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
        0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
        0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
        0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::string msg = in;
    uint64_t bits = static_cast<uint64_t>(msg.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<uint8_t>(msg[off + i*4]) << 24) |
                   (static_cast<uint8_t>(msg[off + i*4+1]) << 16) |
                   (static_cast<uint8_t>(msg[off + i*4+2]) << 8) |
                   (static_cast<uint8_t>(msg[off + i*4+3]));
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    static const char* hexd = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i)
        for (int s = 28; s >= 0; s -= 4)
            out.push_back(hexd[(h[i] >> s) & 0xf]);
    return out;
}

std::string diffCacheFile(const std::string& cacheDir,
                          const std::string& fromSha,
                          const std::string& toSha,
                          const std::string& file) {
    return cacheDir + "/diffcache/" +
           sha256hex(fromSha + "\x1f" + toSha + "\x1f" + file) + ".json";
}

// diffcache.get { cacheDir, fromSha, toSha, file } -> { hit, value? }
void handleDiffCacheGet(const Responder& rsp, const json& p) {
    std::string cacheDir, fromSha, toSha, file;
    if (!reqStr(rsp, p, "cacheDir", "diffcache.get", cacheDir)) return;
    if (!reqStr(rsp, p, "fromSha", "diffcache.get", fromSha)) return;
    if (!reqStr(rsp, p, "toSha", "diffcache.get", toSha)) return;
    if (!reqStr(rsp, p, "file", "diffcache.get", file)) return;
    cacheDir = resolveRoot(cacheDir);
    const std::string path = diffCacheFile(cacheDir, fromSha, toSha, file);
    std::ifstream f(path, std::ios::binary);
    if (!f) { rsp.result(json{{"hit", false}}); return; }
    std::string data((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    json value;
    try { value = json::parse(data); }
    catch (...) { rsp.result(json{{"hit", false}}); return; }  // corrupt -> miss
    rsp.result(json{{"hit", true}, {"value", value}});
}

// diffcache.put { cacheDir, fromSha, toSha, file, value } -> { ok }
void handleDiffCachePut(const Responder& rsp, const json& p) {
    std::string cacheDir, fromSha, toSha, file;
    if (!reqStr(rsp, p, "cacheDir", "diffcache.put", cacheDir)) return;
    if (!reqStr(rsp, p, "fromSha", "diffcache.put", fromSha)) return;
    if (!reqStr(rsp, p, "toSha", "diffcache.put", toSha)) return;
    if (!reqStr(rsp, p, "file", "diffcache.put", file)) return;
    auto vit = p.find("value");
    if (vit == p.end()) {
        rsp.error(kInvalidRequest, "diffcache.put requires 'value'");
        return;
    }
    cacheDir = resolveRoot(cacheDir);
    std::error_code ec;
    std::filesystem::create_directories(cacheDir + "/diffcache", ec);
    const std::string path = diffCacheFile(cacheDir, fromSha, toSha, file);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) { rsp.error(kInternal, "diffcache write failed"); return; }
        const std::string s = dumpSafe(*vit);
        o.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
    std::filesystem::rename(tmp, path, ec);  // atomic same-dir replace
    if (ec) {
        std::filesystem::remove(tmp, ec);
        rsp.error(kInternal, "diffcache rename failed");
        return;
    }
    rsp.result(json{{"ok", true}});
}

// diffcache.prune { cacheDir, days } -> { removed }  (mtime hygiene)
void handleDiffCachePrune(const Responder& rsp, const json& p) {
    std::string cacheDir;
    if (!reqStr(rsp, p, "cacheDir", "diffcache.prune", cacheDir)) return;
    long long days = 0;
    if (auto it = p.find("days"); it != p.end() && it->is_number_integer())
        days = it->get<long long>();
    cacheDir = resolveRoot(cacheDir);
    const auto cutoff = std::chrono::system_clock::now() -
                        std::chrono::hours(24 * days);
    int removed = 0;
    std::error_code ec;
    std::filesystem::directory_iterator it(cacheDir + "/diffcache", ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        auto wt = std::filesystem::last_write_time(it->path(), ec);
        if (ec) { ec.clear(); continue; }
        // file_time -> system_clock (good enough for day-granular prune)
        auto sys = std::chrono::file_clock::to_sys(wt);
        if (sys < cutoff) {
            std::error_code rm;
            if (!std::filesystem::remove(it->path(), rm)) continue;
            ++removed;
        }
    }
    rsp.result(json{{"removed", removed}});
}

// meta.debug { sleepMs?, padBytes? } -> { echoId, pad }
//
// Diagnostic endpoint: sleeps `sleepMs` (polling cancellation every
// 20 ms so cancellation actually interrupts in-flight work), then
// returns a result padded to `padBytes` so an over-cap response
// exercises the chunked path end-to-end. Used by the concurrency /
// interleaving / chunking / cancellation tests. Harmless on the user's
// own transport; also useful for transport latency/throughput probing.
void handleDebug(const Responder& rsp, const json& params) {
    long long sleepMs = 0;
    if (auto it = params.find("sleepMs");
        it != params.end() && it->is_number_integer()) {
        sleepMs = it->get<long long>();
    }
    long long padBytes = 0;
    if (auto it = params.find("padBytes");
        it != params.end() && it->is_number_integer()) {
        padBytes = it->get<long long>();
    }
    long long slept = 0;
    while (slept < sleepMs) {
        if (g_cancel.isCancelled(rsp.idKey)) {
            rsp.error(kCancelled, "request cancelled");
            return;
        }
        long long slice = std::min<long long>(20, sleepMs - slept);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        slept += slice;
    }
    if (g_cancel.isCancelled(rsp.idKey)) {
        rsp.error(kCancelled, "request cancelled");
        return;
    }
    std::string pad(padBytes > 0 ? static_cast<size_t>(padBytes) : 0, 'x');
    rsp.result(json{{"echoId", rsp.id}, {"pad", pad}});
}

// Defined after `WorkQueue g_queue;` (forward-declared so the
// diagnostic handlers, which precede dispatch, can use it).
json queueStat();

// meta.stat -> live load snapshot for diagnostics / the Phase-3
// Connection Log. { lanes:{...}, lspSessions, uptimeMs, gitParallel,
// logLevel, agentVersion }.
void handleMetaStat(const Responder& rsp, const json&) {
    size_t sessions;
    {
        std::lock_guard<std::mutex> g(g_lspMu);
        sessions = g_lspSessions.size();
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_startTime).count();
    rsp.result(json{
        {"lanes", queueStat()},
        {"lspSessions", static_cast<int>(sessions)},
        {"uptimeMs", static_cast<long long>(ms)},
        {"gitParallel", !g_serializeGit.load(std::memory_order_relaxed)},
        {"logLevel", logLevelName(g_logLevel.load(std::memory_order_relaxed))},
        {"agentVersion", kAgentVersion}});
}

// Phase E: surface the agent's sandbox posture so the Mac UI can show
// the user what this binary can and cannot do. Honest about current
// gaps: `fs.readFile` accepts arbitrary absolute paths today. Pair with
// `fs.selftest` for a runtime confirmation rather than a documentation
// claim.
//
// Returns:
//   { agentVersion, protocolVersion,
//     outboundIO: [...],
//     fileSystemAccess: { absolutePathsAllowed, relativeAnchoredUnderHome },
//     languageServers: { binaries: [...] },
//     network: "stdio-only-and-git-via-transport" }
void handleMetaCapabilities(const Responder& rsp, const json&) {
    // GRCLanguage in GitReviewCore.h goes UNKNOWN(0) -> SWIFT(8); we
    // skip UNKNOWN. Hardcoded bound mirrors the enum to avoid an
    // unspecified C-API count.
    json lspBinaries = json::array();
    for (int i = 1; i <= static_cast<int>(GRC_LANG_SWIFT); ++i) {
        char* p = grc_language_server_path(static_cast<GRCLanguage>(i));
        if (p != nullptr) { lspBinaries.push_back(std::string(p)); grc_free_string(p); }
    }
    rsp.result(json{
        {"agentVersion", kAgentVersion},
        {"protocolVersion", kProtocolVersion},
        {"outboundIO", json::array({
            "stdio (jsonrpc to the Mac)",
            "git-fetch via the user-supplied transport (credentials brokered from the Mac)",
            "language-server child processes (stdio)"})},
        {"fileSystemAccess", json{
            {"absolutePathsAllowed", true},
            {"relativeAnchoredUnderHome", true},
            {"description",
             "fs.readFile and fs.listDirectory resolve symlinks, then "
             "require the canonical path to lie inside a configured "
             "allowed root (--allow-root, repeatable; default $HOME). "
             "Relative paths anchor under $HOME (resolveRoot). Paths "
             "outside every allowed root fail PERMISSION_DENIED."}}},
        {"languageServers", json{
            {"binaries", lspBinaries},
            {"description",
             "The agent spawns language-server binaries by absolute path "
             "from grc_language_server_path(); each speaks LSP over stdio "
             "to the agent and has no separate network surface."}}},
        {"network", "no outbound sockets opened by the agent itself"},
        {"selftestMethod", "fs.selftest"}});
}

// Phase E: defensive runtime probe. The Mac calls this from the
// "Security" pane; the agent attempts to read a representative
// outside-roots path (/etc/passwd) and reports what happens. If the
// read succeeds, the dot in the UI shows "no FS sandbox"; if it fails,
// the user sees the failure mode. Either way, it's a *fact*, not a
// promise — the audit is grounded in the binary that's actually
// running, not the docs. Reads at most a few hundred bytes so we don't
// blast the user's transport with a leaked /etc/passwd.
void handleFsSelftest(const Responder& rsp, const json&) {
    constexpr const char* kProbePath = "/etc/passwd";
    char* buf = nullptr;
    int64_t size = 0;
    GRCError e = grc_fs_read_file(kProbePath, &buf, &size);
    bool succeeded = (e == GRC_SUCCESS);
    int firstBytes = 0;
    std::string sample;
    if (succeeded && size > 0) {
        firstBytes = static_cast<int>(std::min<int64_t>(size, 128));
        sample.assign(buf, buf + firstBytes);
        // Stop at first newline; this is a sample for display, not data.
        if (auto pos = sample.find('\n'); pos != std::string::npos) {
            sample.erase(pos);
        }
    }
    if (buf != nullptr) grc_free_string(buf);
    rsp.result(json{
        {"probe", kProbePath},
        {"succeeded", succeeded},
        {"errorCode", succeeded ? "" : grcErrName(e)},
        {"firstBytesReadable", firstBytes},
        {"sampleSnippet", sample}});
}

// logs.tail { maxBytes? } -> the last <=maxBytes of the on-disk log so
// the Mac can pull remote diagnostics without SSHing in (Phase-3
// Connection Log UI). { enabled, path, level, bytes, text }.
void handleLogsTail(const Responder& rsp, const json& params) {
    if (g_logPath.empty()) {
        rsp.result(json{{"enabled", false}});
        return;
    }
    long long maxBytes = 65536;
    if (auto it = params.find("maxBytes");
        it != params.end() && it->is_number_integer())
        maxBytes = it->get<long long>();
    if (maxBytes < 0) maxBytes = 0;
    if (maxBytes > (1 << 20)) maxBytes = 1 << 20;  // 1 MiB cap
    std::string text;
    {   // Snapshot under the log mutex; do NOT logWrite while held.
        std::lock_guard<std::mutex> g(g_logMu);
        std::ifstream f(g_logPath, std::ios::binary);
        if (f) {
            f.seekg(0, std::ios::end);
            std::streamoff size = f.tellg();
            std::streamoff start =
                size > maxBytes ? size - maxBytes : std::streamoff(0);
            f.seekg(start, std::ios::beg);
            std::ostringstream ss;
            ss << f.rdbuf();
            text = ss.str();
        }
    }
    rsp.result(json{{"enabled", true},
                    {"path", g_logPath},
                    {"level", logLevelName(g_logLevel.load(std::memory_order_relaxed))},
                    {"bytes", static_cast<long long>(text.size())},
                    {"text", text}});
}

void dispatch(const std::string& method, const Responder& rsp, const json& params) {
    if (method == "meta.hello") {
        handleHello(rsp, params);
    } else if (method == "git.headSha") {
        handleHeadSha(rsp, params);
    } else if (method == "git.repoMetadata") {
        handleRepoMetadata(rsp, params);
    } else if (method == "git.remotes") {
        handleRemotes(rsp, params);
    } else if (method == "git.branches") {
        handleBranches(rsp, params);
    } else if (method == "git.commits") {
        handleCommits(rsp, params);
    } else if (method == "git.aheadBehind") {
        handleAheadBehind(rsp, params);
    } else if (method == "git.checkoutBranch") {
        handleCheckoutBranch(rsp, params);
    } else if (method == "git.diffForCommit") {
        handleDiffForCommit(rsp, params);
    } else if (method == "git.workingTreeDiff") {
        handleWorkingTreeDiff(rsp, params);
    } else if (method == "git.stagedDiff") {
        handleStagedDiff(rsp, params);
    } else if (method == "git.showFile") {
        handleShowFile(rsp, params);
    } else if (method == "git.diff") {
        handleGitDiff(rsp, params);
    } else if (method == "git.isAncestor") {
        handleIsAncestor(rsp, params);
    } else if (method == "fs.readFile") {
        handleFsReadFile(rsp, params);
    } else if (method == "fs.listDirectory") {
        handleFsListDirectory(rsp, params);
    } else if (method == "lsp.gotoDefinition") {
        handleLspLocations(rsp, params, "lsp.gotoDefinition", false);
    } else if (method == "lsp.findReferences") {
        handleLspLocations(rsp, params, "lsp.findReferences", true);
    } else if (method == "lsp.hover") {
        handleLspHover(rsp, params);
    } else if (method == "lsp.documentSymbols") {
        handleLspDocumentSymbols(rsp, params);
    } else if (method == "lsp.workspaceSymbols") {
        handleLspWorkspaceSymbols(rsp, params);
    } else if (method == "lsp.foldingRange") {
        handleLspFoldingRange(rsp, params);
    } else if (method == "meta.debug") {
        handleDebug(rsp, params);
    } else if (method == "meta.stat") {
        handleMetaStat(rsp, params);
    } else if (method == "meta.capabilities") {
        handleMetaCapabilities(rsp, params);
    } else if (method == "fs.selftest") {
        handleFsSelftest(rsp, params);
    } else if (method == "logs.tail") {
        handleLogsTail(rsp, params);
    } else if (method == "index.create") {
        handleIndexCreate(rsp, params);
    } else if (method == "index.run") {
        handleIndexRun(rsp, params);
    } else if (method == "index.destroy") {
        handleIndexDestroy(rsp, params);
    } else if (method == "watch.head") {
        handleWatchHead(rsp, params);
    } else if (method == "git.clone") {
        handleGitClone(rsp, params);
    } else if (method == "git.fetch") {
        handleGitFetch(rsp, params);
    } else if (method == "git.ensureRepository") {
        handleEnsureRepository(rsp, params);
    } else if (method == "cred.provide") {
        handleCredProvide(rsp, params);
    } else if (method == "cred.selftest") {
        handleCredSelftest(rsp, params);
    } else if (method == "diffcache.get") {
        handleDiffCacheGet(rsp, params);
    } else if (method == "diffcache.put") {
        handleDiffCachePut(rsp, params);
    } else if (method == "diffcache.prune") {
        handleDiffCachePrune(rsp, params);
    } else {
        rsp.error(kNotFound, "unknown method: " + method);
    }
}

// ---- Work queue + worker pool ----------------------------------------

struct Job {
    json id;
    std::string idKey;
    std::string method;
    json params;
    Lane lane;
};

// Plan defaults (Agent-side rate control): global cap 16 worker threads;
// per-lane in-flight caps interactive=unbounded, normal=8, bulk=2. With
// a 16-thread pool that leaves >=6 threads always free for interactive
// even with normal+bulk saturated (reserved interactive capacity).
constexpr int kPoolSize    = 16;
constexpr int kNormalSlots = 8;
constexpr int kBulkSlots   = 2;

class WorkQueue {
public:
    void push(Job j) {
        {
            std::lock_guard<std::mutex> l(m_);
            q_[static_cast<int>(j.lane)].push_back(std::move(j));
        }
        cv_.notify_one();
    }

    // Block until a runnable job is available, honoring per-lane caps
    // and strict interactive>normal>bulk priority. Returns false only
    // when stopped and fully drained.
    bool pop(Job& out) {
        std::unique_lock<std::mutex> l(m_);
        while (true) {
            int lane = pickRunnableLocked();
            if (lane >= 0) {
                out = std::move(q_[lane].front());
                q_[lane].pop_front();
                ++active_[lane];
                return true;
            }
            if (stop_ && emptyLocked()) return false;
            cv_.wait(l);
        }
    }

    void finish(Lane lane) {
        {
            std::lock_guard<std::mutex> l(m_);
            --active_[static_cast<int>(lane)];
        }
        // A freed slot may make a capped lane runnable for a waiter.
        cv_.notify_all();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> l(m_);
            stop_ = true;
        }
        cv_.notify_all();
    }

    // Snapshot of per-lane queued/active depth for meta.stat.
    json stat() {
        std::lock_guard<std::mutex> l(m_);
        auto lane = [&](int i) {
            return json{{"queued", static_cast<int>(q_[i].size())},
                        {"active", active_[i]}};
        };
        return json{{"interactive", lane(0)},
                    {"normal", lane(1)},
                    {"bulk", lane(2)}};
    }

private:
    int cap(int lane) const {
        if (lane == static_cast<int>(Lane::Normal)) return kNormalSlots;
        if (lane == static_cast<int>(Lane::Bulk))   return kBulkSlots;
        return kPoolSize;  // interactive: effectively unbounded
    }
    bool emptyLocked() const {
        for (int i = 0; i < kLaneCount; ++i)
            if (!q_[i].empty()) return false;
        return true;
    }
    // Highest-priority lane that has work AND a free slot, else -1.
    int pickRunnableLocked() const {
        for (int i = 0; i < kLaneCount; ++i) {
            if (!q_[i].empty() && active_[i] < cap(i)) return i;
        }
        return -1;
    }

    std::mutex m_;
    std::condition_variable cv_;
    std::array<std::deque<Job>, kLaneCount> q_;
    std::array<int, kLaneCount> active_{};
    bool stop_ = false;
};

WorkQueue g_queue;

json queueStat() { return g_queue.stat(); }

void workerLoop() {
    Job job;
    while (g_queue.pop(job)) {
        Responder rsp{job.id, job.idKey, job.lane};
        // Queued-then-cancelled work dies before it starts (frees the
        // slot immediately for the next request in the lane).
        if (g_cancel.isCancelled(job.idKey)) {
            if (logOn(LogLevel::Info))
                logWrite(LogLevel::Info, logTag(job.id, job.lane) +
                         "cancelled before start method=" + job.method);
            rsp.error(kCancelled, "request cancelled");
        } else {
            if (logOn(LogLevel::Debug))
                logWrite(LogLevel::Debug, logTag(job.id, job.lane) + "req method=" +
                         job.method + " params=" +
                         dumpSafe(redactParams(job.params)));
            const auto t0 = std::chrono::steady_clock::now();
            try {
                dispatch(job.method, rsp, job.params);
            } catch (const std::exception& e) {
                rsp.error(kInternal, std::string("agent exception: ") + e.what());
            }
            if (logOn(LogLevel::Debug)) {
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                logWrite(LogLevel::Debug, logTag(job.id, job.lane) + "done method=" +
                         job.method + " dur_ms=" + std::to_string(ms));
            }
        }
        g_cancel.forget(job.idKey);
        g_queue.finish(job.lane);
    }
}

// ---- Reader: classify, route to queue or cancel registry -------------

void handleIncoming(const std::string& frameBody) {
    json msg;
    try {
        msg = json::parse(frameBody);
    } catch (const std::exception& e) {
        if (logOn(LogLevel::Warn))
            logWrite(LogLevel::Warn, std::string("parse error: ") + e.what() +
                     " (frame " + std::to_string(frameBody.size()) + "B)");
        sendNullIdError(kInvalidRequest, std::string("invalid JSON: ") + e.what());
        return;
    }

    auto methodIt = msg.find("method");
    const bool hasMethod = methodIt != msg.end() && methodIt->is_string();
    const bool hasId = msg.contains("id") && !msg.at("id").is_null();

    // Notification (no id). $/cancelRequest and index.cancel are actioned.
    if (!hasId) {
        const std::string m = hasMethod ? methodIt->get<std::string>() : "";
        if (m == "$/cancelRequest") {
            if (auto p = msg.find("params");
                p != msg.end() && p->contains("id") && !p->at("id").is_null()) {
                g_cancel.cancel(p->at("id").dump());
                if (logOn(LogLevel::Info))
                    logWrite(LogLevel::Info, "cancel-request id=" +
                             p->at("id").dump());
            }
        } else if (m == "index.cancel") {
            if (auto p = msg.find("params");
                p != msg.end() && p->contains("indexerId") &&
                p->at("indexerId").is_string()) {
                const std::string id = p->at("indexerId").get<std::string>();
                if (IndexerEntry* e = indexerById(id)) {
                    e->cancelled.store(true);
                    grc_indexer_cancel(e->idx);
                    if (logOn(LogLevel::Info))
                        logWrite(LogLevel::Info, "index.cancel id=" + id);
                }
            }
        } else if (m == "watch.stop") {
            if (auto p = msg.find("params");
                p != msg.end() && p->contains("watchId") &&
                p->at("watchId").is_string()) {
                stopWatch(p->at("watchId").get<std::string>());
            }
        }
        return;  // other notifications ignored
    }

    const json id = msg.at("id");
    if (!hasMethod) {
        Responder rsp{id, id.dump(), Lane::Normal};
        rsp.error(kInvalidRequest, "missing string 'method'");
        return;
    }
    const std::string method = methodIt->get<std::string>();
    const json params = msg.contains("params") ? msg.at("params") : json::object();

    Lane lane = defaultLaneFor(method);
    if (auto it = msg.find("lane"); it != msg.end() && it->is_string()) {
        lane = laneFromString(it->get<std::string>(), lane);
    }
    g_queue.push(Job{id, id.dump(), method, params, lane});
}

}  // namespace

int main(int argc, char** argv) {
    // askpass mode: git invoked us as `$GIT_ASKPASS "<prompt>"` with
    // SCRUTINY_CRED_SOCK set (the credential broker). Handle and exit
    // before anything else -- this is a short-lived child, not the
    // server.
    if (const char* sock = ::getenv("SCRUTINY_CRED_SOCK");
        sock != nullptr && argc == 2 &&
        std::strcmp(argv[1], "--version") != 0 &&
        std::strcmp(argv[1], "--rpc-stdio") != 0) {
        return askpassMain(sock, argv[1]);
    }

    // The Mac launches us as `scrutiny-agent --rpc-stdio`. Accept it (and
    // bare invocation) so the bootstrap exec line is stable. The Phase-3
    // RemoteHost config drives `--log <path>` / `--log-level <lvl>` on
    // the bootstrap exec line; default is no logging (off).
    //
    // `--allow-root <path>` (repeatable) narrows the fs sandbox: every
    // path passed to fs.readFile / fs.listDirectory must, after
    // symlink resolution, lie inside one of these roots. Relative
    // values anchor under $HOME (same convention as the cache/clone
    // roots). If no --allow-root is given the floor is $HOME.
    std::string wantLogPath;
    LogLevel wantLogLevel = LogLevel::Info;  // default when --log is given
    std::vector<std::string> rawAllowedRoots;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0) {
            std::printf("%s proto %d\n", kAgentVersion, kProtocolVersion);
            return 0;
        }
        if (std::strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            wantLogPath = argv[++i];
        } else if (std::strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            wantLogLevel = parseLogLevel(argv[++i]);
        } else if (std::strcmp(argv[i], "--allow-root") == 0 && i + 1 < argc) {
            rawAllowedRoots.emplace_back(argv[++i]);
        }
        // --rpc-stdio (default mode) and anything else: fall through.
    }
    if (!wantLogPath.empty() && wantLogLevel != LogLevel::Off) {
        g_logPath = wantLogPath;
        g_logLevel.store(static_cast<int>(wantLogLevel),
                         std::memory_order_relaxed);
    }
    // Canonicalize the configured roots. A path that doesn't resolve
    // gets dropped with a stderr warning (rather than crashing): leaving
    // it in would deny *every* operation against that intended root.
    {
        std::vector<std::string> roots;
        for (const auto& raw : rawAllowedRoots) {
            const std::string anchored = resolveRoot(raw);
            char real[4096];
            if (::realpath(anchored.c_str(), real) != nullptr) {
                roots.emplace_back(real);
            } else {
                std::fprintf(stderr,
                    "scrutiny-agent: --allow-root %s does not resolve "
                    "(%s); ignoring\n", raw.c_str(), std::strerror(errno));
            }
        }
        if (roots.empty()) {
            const char* h = ::getenv("HOME");
            if (h != nullptr && *h != '\0') {
                char real[4096];
                if (::realpath(h, real) != nullptr) {
                    roots.emplace_back(real);
                }
            }
        }
        // Dedup (stable). Two --allow-root args resolving to the same
        // canonical path -> one entry; preserves caller order otherwise.
        std::vector<std::string> uniq;
        for (auto& r : roots) {
            if (std::find(uniq.begin(), uniq.end(), r) == uniq.end())
                uniq.emplace_back(std::move(r));
        }
        g_allowedRoots = std::move(uniq);
        std::fprintf(stderr, "scrutiny-agent: fs sandbox roots:");
        if (g_allowedRoots.empty()) {
            std::fprintf(stderr, " <none -- every fs call will be denied>");
        } else {
            for (const auto& r : g_allowedRoots)
                std::fprintf(stderr, " %s", r.c_str());
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
    }
    g_startTime = std::chrono::steady_clock::now();

    // Resolve our own absolute path: git's GIT_ASKPASS must be an
    // executable path, and we re-exec THIS binary as the askpass child.
    {
        char buf[4096];
#if defined(__linux__)
        ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) { buf[n] = '\0'; g_selfExe = buf; }
#elif defined(__APPLE__)
        uint32_t sz = sizeof(buf);
        if (_NSGetExecutablePath(buf, &sz) == 0) {
            char real[4096];
            g_selfExe = ::realpath(buf, real) ? real : buf;
        }
#endif
        if (g_selfExe.empty() && argc > 0) {
            char real[4096];
            g_selfExe = ::realpath(argv[0], real) ? real : argv[0];
        }
    }

    // A dead transport must not kill us mid-write; the writer detects
    // the short write and stops, then we exit cleanly.
    ::signal(SIGPIPE, SIG_IGN);

    // One-time libgit2 init on THIS thread (constructs git-manipulation's
    // magic-static single-threaded) BEFORE any worker exists, then decide
    // whether git work needs serialization. After this, a threadsafe
    // libgit2 lets distinct per-request repos run fully in parallel.
    gm_global_init();
    const bool gitThreadsafe = gm_libgit2_threadsafe();
    g_serializeGit.store(!gitThreadsafe, std::memory_order_relaxed);
    std::fprintf(stderr,
        "scrutiny-agent: libgit2 threads=%s; git lane=%s\n",
        gitThreadsafe ? "yes" : "no",
        gitThreadsafe ? "parallel (per-request repo)" : "serialized (fallback)");
    std::fflush(stderr);

    if (logOn(LogLevel::Info))
        logWrite(LogLevel::Info, std::string("agent start version=") +
                 kAgentVersion + " proto=" + std::to_string(kProtocolVersion) +
                 " pool=" + std::to_string(kPoolSize) + " libgit2_threads=" +
                 (gitThreadsafe ? "yes" : "no") + " logLevel=" +
                 logLevelName(g_logLevel.load(std::memory_order_relaxed)));

    g_writer.start();
    std::vector<std::thread> pool;
    pool.reserve(kPoolSize);
    for (int i = 0; i < kPoolSize; ++i) pool.emplace_back(workerLoop);

    // Reader loop (this thread). Reads frames, classifies, routes.
    while (true) {
        auto frame = readFrame();
        if (!frame) break;  // clean EOF -> transport closed
        handleIncoming(*frame);
    }

    // Shutdown: no more input. Let workers drain the queue and finish
    // in-flight jobs, then stop the single writer (flushing what it can).
    if (logOn(LogLevel::Info))
        logWrite(LogLevel::Info, "shutdown: stdin EOF (transport closed); "
                 "draining workers");
    g_queue.stop();
    for (auto& t : pool) t.join();
    // Stop all HEAD watches (joins their Core threads) BEFORE the writer
    // exits, so no watcher thread can enqueue onto a stopped writer.
    {
        std::vector<std::string> ids;
        {
            std::lock_guard<std::mutex> g(g_watchMu);
            for (auto& kv : g_watches) ids.push_back(kv.first);
        }
        for (const auto& id : ids) stopWatch(id);
    }
    g_writer.stop();
    return 0;
}
