// Unit tests for lsp/LSPWorkspace: the "try harder" discovery that
// hands clangd a compile database and sourcekit-lsp an Xcode index
// store. Pure filesystem logic against temp dirs; no server is run
// and generation is disabled (timeout 0) so cmake is never invoked.
#include "lsp/LSPClient.hpp"
#include "lsp/LSPWorkspace.hpp"

#include <cstdio>
#include <ctime>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace gitreview::lsp;

namespace {
int g_failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::cerr << "FAIL " << __FUNCTION__ << ": " << msg << std::endl;   \
        }                                                                       \
    } while (0)

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("lspws-" + std::to_string(::getpid()) + "-" +
                                            std::to_string(std::rand()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};
void touch(const fs::path& p, const std::string& body = "") {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}

void testFindCompileCommandsAtRootAndOneLevelDown() {
    TempDir t;
    CHECK(!findCompileCommandsDir(t.path.string()), "empty workspace has none");
    touch(t.path / "zz-other" / "compile_commands.json", "[]");
    CHECK(findCompileCommandsDir(t.path.string()) == (t.path / "zz-other").string(),
          "a single subdir database is found");
    touch(t.path / "build" / "compile_commands.json", "[]");
    CHECK(findCompileCommandsDir(t.path.string()) == (t.path / "build").string(),
          "build/ is preferred over other names");
    touch(t.path / "compile_commands.json", "[]");
    CHECK(findCompileCommandsDir(t.path.string()) == t.path.string(),
          "the workspace root wins over subdirs");
    touch(t.path / ".hidden" / "compile_commands.json", "[]");
    CHECK(findCompileCommandsDir((t.path).string()) == t.path.string(), "dot-dirs never considered");
}

void testFindConfiguredCMakeBuildDirs() {
    TempDir t;
    touch(t.path / "build" / "CMakeCache.txt", "");
    touch(t.path / "build-rel" / "CMakeCache.txt", "");
    touch(t.path / "build-rel" / "compile_commands.json", "[]");   // already has one
    touch(t.path / "src" / "main.cpp", "");
    auto dirs = findConfiguredCMakeBuildDirs(t.path.string());
    CHECK(dirs.size() == 1 && dirs[0] == (t.path / "build").string(),
          "only configured dirs still lacking a database");
    CHECK(!hasCMakeProject(t.path.string()), "no CMakeLists yet");
    touch(t.path / "CMakeLists.txt", "project(x)");
    CHECK(hasCMakeProject(t.path.string()), "CMakeLists at root detected");
}

void testQueryDriversFromCompileDb() {
    TempDir t;
    CHECK(queryDriversFromCompileDb(t.path.string()).empty(), "no database -> no drivers");
    touch(t.path / "compile_commands.json",
          "[{\"directory\":\"/w\",\"command\":\"/usr/bin/c++ -std=c++23 -c a.cpp\",\"file\":\"a.cpp\"},"
          " {\"directory\":\"/w\",\"arguments\":[\"/usr/bin/clang-17\",\"-c\",\"b.c\"],\"file\":\"b.c\"},"
          " {\"directory\":\"/w\",\"command\":\"/usr/bin/c++ -c c.cpp\",\"file\":\"c.cpp\"},"
          " {\"directory\":\"/w\",\"command\":\"cc -c d.c\",\"file\":\"d.c\"}]");
    auto d = queryDriversFromCompileDb(t.path.string());
    CHECK(d.size() == 2 && d[0] == "/usr/bin/c++" && d[1] == "/usr/bin/clang-17",
          "distinct absolute drivers from command and arguments forms; relative 'cc' skipped");
    touch(t.path / "build" / "compile_commands.json", "not json");
    CHECK(queryDriversFromCompileDb((t.path / "build").string()).empty(), "malformed database -> none");
    // prepareWorkspace threads it through to clangd's argv.
    auto info = prepareWorkspace(t.path.string(), Language::Cpp, 0);
    bool hasQuery = false;
    for (auto& a : info.serverArguments) if (a == "--query-driver=/usr/bin/c++,/usr/bin/clang-17") hasQuery = true;
    CHECK(hasQuery, "clangd is allowed to query exactly the database's compilers");
}

void testGeneratedDirLivesInGitCache() {
    TempDir t;
    CHECK(generatedCompileDbDir(t.path.string()) == (t.path / ".scrutiny-lsp" / "compile-db").string(),
          "no .git -> sidecar dir");
    fs::create_directories(t.path / ".git");
    CHECK(generatedCompileDbDir(t.path.string()) == (t.path / ".git" / "lsp-cache" / "compile-db").string(),
          "with .git -> inside the ignored lsp-cache");
}

void testXcodeContainerAndPlist() {
    TempDir t;
    CHECK(!findXcodeContainer(t.path.string()), "none");
    fs::create_directories(t.path / "App.xcodeproj");
    CHECK(findXcodeContainer(t.path.string()) == (t.path / "App.xcodeproj").string(), "project found");
    fs::create_directories(t.path / "App.xcworkspace");
    CHECK(findXcodeContainer(t.path.string()) == (t.path / "App.xcworkspace").string(),
          "workspace preferred over project");
    const std::string plist =
        "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>\n"
        "\t<key>LastAccessedDate</key>\n\t<date>2026-09-12T05:22:34Z</date>\n"
        "\t<key>WorkspacePath</key>\n\t<string>/Users/me/proj/App.xcodeproj</string>\n"
        "</dict></plist>";
    CHECK(workspacePathFromInfoPlist(plist) == "/Users/me/proj/App.xcodeproj", "WorkspacePath parsed");
    CHECK(!workspacePathFromInfoPlist("<plist/>"), "absent key -> nullopt");
}

void testFindXcodeIndexStore() {
    TempDir t;
    const fs::path dd = t.path / "DerivedData";
    const std::string container = "/Users/me/proj/App.xcodeproj";
    auto mk = [&](const std::string& name, const std::string& wp, bool withStore, const char* storeRel) {
        touch(dd / name / "info.plist",
              "<plist><dict><key>WorkspacePath</key><string>" + wp + "</string></dict></plist>");
        if (withStore) fs::create_directories(dd / name / storeRel);
    };
    mk("Other-aaaa", "/Users/me/other/Other.xcodeproj", true, "Index.noindex/DataStore");
    mk("App-nostore", container, false, "");
    CHECK(!findXcodeIndexStore(container, dd.string()), "matching project without a DataStore -> none");
    mk("App-bbbb", container, true, "Index.noindex/DataStore");
    CHECK(findXcodeIndexStore(container, dd.string()) == (dd / "App-bbbb" / "Index.noindex" / "DataStore").string(),
          "the matching project's DataStore");
    CHECK(!findXcodeIndexStore(container, (t.path / "missing").string()), "missing DerivedData root -> none");
}

void testPrepareWorkspaceReportsWhatIsMissing() {
    TempDir t;
    touch(t.path / "main.cpp", "int main(){}");
    auto cpp = prepareWorkspace(t.path.string(), Language::Cpp, /*generateTimeoutSeconds=*/0);
    CHECK(!cpp.crossFileCapable, "no database -> not cross-file capable");
    CHECK(cpp.source == "none", "source none");
    CHECK(!cpp.notes.empty() && cpp.notes[0].find("compile_commands.json") != std::string::npos,
          "note explains the missing compile database");
    bool hasBg = false;
    for (auto& a : cpp.serverArguments) if (a == "--background-index") hasBg = true;
    CHECK(hasBg, "clangd always gets --background-index");

    touch(t.path / "build" / "compile_commands.json", "[]");
    auto found = prepareWorkspace(t.path.string(), Language::Cpp, 0);
    CHECK(found.source == "found" && found.crossFileCapable, "found database -> capable");
    CHECK(found.compileCommandsDir == (t.path / "build").string(), "dir recorded");
    bool hasDir = false;
    for (auto& a : found.serverArguments) if (a == "--compile-commands-dir=" + (t.path / "build").string()) hasDir = true;
    CHECK(hasDir, "clangd is pointed at the database");
    CHECK(found.notes.empty(), "nothing to complain about");

    TempDir s;
    touch(s.path / "Package.swift", "// swift-tools-version:5.9");
    auto pkg = prepareWorkspace(s.path.string(), Language::Swift, 0);
    CHECK(pkg.source == "package" && pkg.crossFileCapable, "SwiftPM self-configures");

    TempDir x;
    fs::create_directories(x.path / "App.xcodeproj");
    auto xc = prepareWorkspace(x.path.string(), Language::Swift, 0);
    CHECK(!xc.crossFileCapable && !xc.notes.empty() && xc.notes[0].find("Cmd+B") != std::string::npos,
          "Xcode project without an index says to build once");

    auto rs = prepareWorkspace(t.path.string(), Language::Rust, 0);
    CHECK(rs.source == "server" && rs.crossFileCapable, "rust-analyzer self-configures");

    auto j = found.toJson();
    CHECK(j["source"] == "found" && j["compileCommandsDir"].is_string() && j["indexStorePath"].is_null(),
          "JSON shape");
}
}  // namespace

int main() {
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    testFindCompileCommandsAtRootAndOneLevelDown();
    testFindConfiguredCMakeBuildDirs();
    testQueryDriversFromCompileDb();
    testGeneratedDirLivesInGitCache();
    testXcodeContainerAndPlist();
    testFindXcodeIndexStore();
    testPrepareWorkspaceReportsWhatIsMissing();
    if (g_failures == 0) { std::cout << "lsp_workspace_tests: all passed" << std::endl; return 0; }
    std::cerr << "lsp_workspace_tests: " << g_failures << " failure(s)" << std::endl;
    return 1;
}
