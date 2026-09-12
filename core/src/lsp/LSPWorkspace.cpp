#include "lsp/LSPWorkspace.hpp"
#include "lsp/LSPClient.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace gitreview::lsp {

namespace {

bool isFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}
bool isDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

// Run `argv` in `cwd` with stdout/stderr to /dev/null, killing it after
// `timeoutSeconds`. Returns the exit status, or -1 if it could not be
// run / timed out. Used only for `cmake` (compile-database generation);
// never inherits the agent's stdio, which is the RPC channel.
int runBounded(const std::vector<std::string>& argv, const std::string& cwd,
               int timeoutSeconds) {
    if (argv.empty() || timeoutSeconds <= 0) return -1;
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
        }
        if (!cwd.empty()) { if (::chdir(cwd.c_str()) != 0) ::_exit(126); }
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        ::_exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    int status = 0;
    while (true) {
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (r < 0) return -1;
        if (std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool onPath(const std::string& exe) {
    const char* path = std::getenv("PATH");
    std::string p = path != nullptr ? path : "/usr/local/bin:/usr/bin:/bin";
    std::stringstream ss(p);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        if (::access((dir + "/" + exe).c_str(), X_OK) == 0) return true;
    }
    return false;
}

}  // namespace

nlohmann::json WorkspaceInfo::toJson() const {
    nlohmann::json j{
        {"language", static_cast<int>(language)},
        {"workspacePath", workspacePath},
        {"serverArguments", serverArguments},
        {"initializationOptions", initializationOptions},
        {"source", source},
        {"crossFileCapable", crossFileCapable},
        {"notes", notes},
    };
    j["compileCommandsDir"] = compileCommandsDir ? nlohmann::json(*compileCommandsDir) : nlohmann::json(nullptr);
    j["indexStorePath"] = indexStorePath ? nlohmann::json(*indexStorePath) : nlohmann::json(nullptr);
    return j;
}

std::optional<std::string> findCompileCommandsDir(const std::string& workspacePath) {
    const fs::path ws(workspacePath);
    if (isFile(ws / "compile_commands.json")) return ws.string();
    // One level down, deterministic order (so "build" beats "build-old"
    // only by name, which is at least stable). Skip dot-dirs.
    std::vector<std::string> candidates;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(ws, ec)) {
        if (!e.is_directory(ec)) continue;
        const std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (isFile(e.path() / "compile_commands.json")) candidates.push_back(e.path().string());
    }
    if (candidates.empty()) return std::nullopt;
    std::sort(candidates.begin(), candidates.end());
    // Prefer the conventional names when present.
    for (const char* pref : {"build", "out", "cmake-build-debug", "cmake-build-release"}) {
        const std::string want = (ws / pref).string();
        if (std::find(candidates.begin(), candidates.end(), want) != candidates.end()) return want;
    }
    return candidates.front();
}

std::vector<std::string> findConfiguredCMakeBuildDirs(const std::string& workspacePath) {
    std::vector<std::string> dirs;
    const fs::path ws(workspacePath);
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(ws, ec)) {
        if (!e.is_directory(ec)) continue;
        const std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        if (isFile(e.path() / "CMakeCache.txt") && !isFile(e.path() / "compile_commands.json"))
            dirs.push_back(e.path().string());
    }
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

std::vector<std::string> queryDriversFromCompileDb(const std::string& compileCommandsDir) {
    std::vector<std::string> drivers;
    std::ifstream in(fs::path(compileCommandsDir) / "compile_commands.json");
    if (!in) return drivers;
    nlohmann::json db = nlohmann::json::parse(in, nullptr, false);
    if (db.is_discarded() || !db.is_array()) return drivers;
    for (const auto& e : db) {
        std::string exe;
        if (auto a = e.find("arguments"); a != e.end() && a->is_array() && !a->empty() && (*a)[0].is_string()) {
            exe = (*a)[0].get<std::string>();
        } else if (auto c = e.find("command"); c != e.end() && c->is_string()) {
            const std::string cmd = c->get<std::string>();
            const auto sp = cmd.find(' ');
            exe = sp == std::string::npos ? cmd : cmd.substr(0, sp);
        }
        if (exe.empty() || exe[0] != '/') continue;   // relative names: clangd can't query them safely
        if (std::find(drivers.begin(), drivers.end(), exe) == drivers.end()) drivers.push_back(exe);
    }
    std::sort(drivers.begin(), drivers.end());
    return drivers;
}

bool hasCMakeProject(const std::string& workspacePath) {
    return isFile(fs::path(workspacePath) / "CMakeLists.txt");
}

std::string generatedCompileDbDir(const std::string& workspacePath) {
    const fs::path ws(workspacePath);
    if (isDir(ws / ".git")) return (ws / ".git" / "lsp-cache" / "compile-db").string();
    return (ws / ".scrutiny-lsp" / "compile-db").string();
}

std::optional<std::string> findXcodeContainer(const std::string& workspacePath) {
    const fs::path ws(workspacePath);
    std::optional<std::string> project;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(ws, ec)) {
        const std::string name = e.path().filename().string();
        if (name.size() > 12 && name.ends_with(".xcworkspace")) return e.path().string();
        if (name.size() > 10 && name.ends_with(".xcodeproj") && !project) project = e.path().string();
    }
    return project;
}

std::optional<std::string> workspacePathFromInfoPlist(const std::string& plistXml) {
    // <key>WorkspacePath</key>\n<string>...</string>
    static const std::regex re(R"(<key>WorkspacePath</key>\s*<string>([^<]*)</string>)");
    std::smatch m;
    if (std::regex_search(plistXml, m, re) && m.size() > 1) return m[1].str();
    return std::nullopt;
}

std::optional<std::string> findXcodeIndexStore(const std::string& container,
                                               const std::string& derivedDataRoot) {
    std::string root = derivedDataRoot;
    if (root.empty()) {
        const char* home = std::getenv("HOME");
        if (home == nullptr || *home == '\0') return std::nullopt;
        root = std::string(home) + "/Library/Developer/Xcode/DerivedData";
    }
    std::error_code ec;
    if (!isDir(root)) return std::nullopt;
    // Newest-first would need mtimes; a project usually has one live
    // DerivedData dir, and any match with a DataStore is usable.
    std::optional<std::string> best;
    fs::file_time_type bestTime{};
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec)) continue;
        const fs::path plist = e.path() / "info.plist";
        if (!isFile(plist)) continue;
        std::ifstream in(plist);
        std::stringstream buf;
        buf << in.rdbuf();
        const auto wp = workspacePathFromInfoPlist(buf.str());
        if (!wp || *wp != container) continue;
        for (const char* rel : {"Index.noindex/DataStore", "Index/DataStore"}) {
            const fs::path store = e.path() / rel;
            if (!isDir(store)) continue;
            const auto t = fs::last_write_time(store, ec);
            if (!best || t > bestTime) { best = store.string(); bestTime = t; }
        }
    }
    return best;
}

WorkspaceInfo prepareWorkspace(const std::string& workspacePath, Language language,
                               int generateTimeoutSeconds) {
    WorkspaceInfo info;
    info.language = language;
    info.workspacePath = workspacePath;

    switch (language) {
    case Language::Cpp:
    case Language::C: {
        // Always-on clangd flags: background index for cross-file
        // answers, no header insertion side effects, quiet log.
        info.serverArguments = {"--background-index", "--header-insertion=never", "--log=error"};
        auto dir = findCompileCommandsDir(workspacePath);
        if (dir) {
            info.source = "found";
        } else {
            // A configured CMake build without the database: ask cmake
            // to add it. Cheap -- the cache holds toolchain + options.
            for (const auto& bd : findConfiguredCMakeBuildDirs(workspacePath)) {
                if (!onPath("cmake")) break;
                spdlog::info("[LSPWorkspace] generating compile_commands.json in {}", bd);
                const int rc = runBounded({"cmake", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON", bd},
                                          workspacePath, generateTimeoutSeconds);
                if (rc == 0 && isFile(fs::path(bd) / "compile_commands.json")) {
                    dir = bd;
                    info.source = "generated";
                    break;
                }
                info.notes.push_back("cmake could not regenerate " + bd + " (exit " + std::to_string(rc) + ")");
            }
        }
        if (!dir && hasCMakeProject(workspacePath) && generateTimeoutSeconds > 0) {
            if (onPath("cmake")) {
                const std::string out = generatedCompileDbDir(workspacePath);
                std::error_code ec;
                fs::create_directories(out, ec);
                spdlog::info("[LSPWorkspace] configuring {} into {} for a compile database", workspacePath, out);
                const int rc = runBounded({"cmake", "-S", workspacePath, "-B", out,
                                           "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"},
                                          workspacePath, generateTimeoutSeconds);
                if (rc == 0 && isFile(fs::path(out) / "compile_commands.json")) {
                    dir = out;
                    info.source = "generated";
                } else {
                    info.notes.push_back(
                        "cmake could not configure this project on its own (exit " + std::to_string(rc) +
                        "); if it needs a toolchain or package manager, configure it once yourself with "
                        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON.");
                }
            } else {
                info.notes.push_back("cmake is not installed here, so no compile database could be generated.");
            }
        }
        if (dir) {
            info.compileCommandsDir = dir;
            info.crossFileCapable = true;
            info.serverArguments.push_back("--compile-commands-dir=" + *dir);
            // Let clangd ask the project's own compilers for their
            // system include paths (GCC's libstdc++ lives where only
            // GCC knows). Restricted to the drivers the database names.
            const auto drivers = queryDriversFromCompileDb(*dir);
            if (!drivers.empty()) {
                std::string joined;
                for (const auto& d : drivers) joined += (joined.empty() ? "" : ",") + d;
                info.serverArguments.push_back("--query-driver=" + joined);
            }
        } else {
            info.notes.push_back(
                "No compile_commands.json: clangd can only see the current file, so references, "
                "definitions in other files and hover on external symbols come back empty. Generate one "
                "(CMake: -DCMAKE_EXPORT_COMPILE_COMMANDS=ON; Make/other: bear -- make) in the project "
                "root or a build/ directory and reopen the file.");
        }
        break;
    }
    case Language::Swift: {
        const fs::path ws(workspacePath);
        const bool hasPackage = isFile(ws / "Package.swift");
        const auto container = findXcodeContainer(workspacePath);
        // An Xcode project that Xcode has built has an index covering
        // the app's own sources; prefer it even when a Package.swift
        // sits beside it (a repo can carry both, and sourcekit-lsp
        // would otherwise index only the package and know nothing
        // about the app -- "running but returning nothing").
        const auto store = container ? findXcodeIndexStore(*container) : std::nullopt;
        if (!store && hasPackage) {
            info.source = "package";
            info.crossFileCapable = true;
            break;
        }
        if (!container) {
            info.notes.push_back(
                "No Package.swift or Xcode project at the workspace root: sourcekit-lsp has nothing to index.");
            break;
        }
        if (store) {
            info.indexStorePath = store;
            info.source = "found";
            info.crossFileCapable = true;
            const std::string db = isDir(ws / ".git")
                ? (ws / ".git" / "lsp-cache" / "sourcekit-index-db").string()
                : (ws / ".scrutiny-lsp" / "sourcekit-index-db").string();
            std::error_code ec;
            fs::create_directories(db, ec);
            // sourcekit-lsp configuration (same schema as
            // .sourcekit-lsp/config.json), passed per session so the
            // user's checkout and home directory stay untouched.
            info.initializationOptions = nlohmann::json{
                {"index", {{"indexStorePath", *store}, {"indexDatabasePath", db}}}};
        } else {
            info.notes.push_back(
                "Xcode project without an index: build it once in Xcode (Cmd+B) so DerivedData holds "
                "Index.noindex/DataStore for " + fs::path(*container).filename().string() +
                "; Scrutiny hands that index to sourcekit-lsp the next time the server starts.");
        }
        break;
    }
    case Language::Rust:
    case Language::Go:
    case Language::Python:
    case Language::TypeScript:
    case Language::JavaScript:
        info.source = "server";
        info.crossFileCapable = true;
        break;
    default:
        break;
    }
    return info;
}

}  // namespace gitreview::lsp
