// declaration_search_tests.cpp -- the text-search fallback behind
// go-to-definition / find-references when no language server answers.
// Pure: temp workspaces, no server. Same style as lsp_workspace_tests.

#include "lsp/DeclarationSearch.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
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
        path = fs::temp_directory_path() / ("declsearch-" + std::to_string(::getpid()) + "-" +
                                            std::to_string(std::rand()));
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};
void touch(const fs::path& p, const std::string& body = "") {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << body;
}
std::string uriOf(const fs::path& p) { return "file://" + p.string(); }

void testIdentifierAtSelectsTheWordUnderAndJustPastTheCursor() {
    const std::string src = "let a = 1\n    let renderer = DocumentSliceBuilder(model: m)\n";
    CHECK(identifierAt(src, 1, 19) == "DocumentSliceBuilder", "cursor on the first letter");
    CHECK(identifierAt(src, 1, 30) == "DocumentSliceBuilder", "cursor inside the word");
    CHECK(identifierAt(src, 1, 39) == "DocumentSliceBuilder", "cursor just past the word");
    CHECK(identifierAt(src, 1, 5) == "let", "keyword is still an identifier for the scanner");
    CHECK(identifierAt(src, 1, 40) == "model", "cursor on the next identifier selects that one");
    CHECK(identifierAt(src, 0, 6) == "", "cursor on '=' with a space before it selects nothing");
    CHECK(identifierAt(src, 5, 0) == "", "line past the end selects nothing");
    CHECK(identifierAt(src, -1, 0) == "", "negative line selects nothing");
    CHECK(identifierAt("x = 42\n", 0, 4) == "", "a number is not an identifier");
}

void testFindWordRespectsBoundaries() {
    CHECK(findWord("myFoo Foo(Foo)", "Foo") == 6, "first whole-word match, not the suffix of myFoo");
    CHECK(findWord("myFoo Foo(Foo)", "Foo", 7) == 10, "search from an offset");
    CHECK(findWord("Foobar", "Foo") == std::string::npos, "prefix of a longer word is not a match");
}

void testSwiftDeclarationRanks() {
    CHECK(declarationRank(Language::Swift, "struct DocumentSliceBuilder {", "DocumentSliceBuilder") == 3, "struct");
    CHECK(declarationRank(Language::Swift, "final class Foo: Bar {", "Foo") == 3, "class");
    CHECK(declarationRank(Language::Swift, "extension Foo {", "Foo") == 3, "extension");
    CHECK(declarationRank(Language::Swift, "    func render(_ x: Int) -> Int {", "render") == 2, "func");
    CHECK(declarationRank(Language::Swift, "    func render<T>(_ x: T) {", "render") == 2, "generic func");
    CHECK(declarationRank(Language::Swift, "    @State private var isLoading = false", "isLoading") == 1, "var");
    CHECK(declarationRank(Language::Swift, "    case connecting", "connecting") == 1, "enum case");
    CHECK(declarationRank(Language::Swift, "    let b = DocumentSliceBuilder(model: m)", "DocumentSliceBuilder") == 0, "a use is not a declaration");
    CHECK(declarationRank(Language::Swift, "    return render(x)", "render") == 0, "a call is not a declaration");
    CHECK(declarationRank(Language::Swift, "struct DocumentSliceBuilderX {", "DocumentSliceBuilder") == 0, "whole word only");
}

void testCFamilyDeclarationRanks() {
    CHECK(declarationRank(Language::Cpp, "class LSPClient {", "LSPClient") == 3, "class");
    CHECK(declarationRank(Language::Cpp, "class LSPClient;", "LSPClient") == 0, "forward declaration is not a definition");
    CHECK(declarationRank(Language::Cpp, "struct Rule { int rank; };", "Rule") == 3, "one-line struct");
    CHECK(declarationRank(Language::Cpp, "enum class Language {", "Language") == 3, "enum class");
    CHECK(declarationRank(Language::Cpp, "#define GRC_MAX 4", "GRC_MAX") == 3, "macro");
    CHECK(declarationRank(Language::Cpp, "using Callback = std::function<void()>;", "Callback") == 3, "using alias");
    CHECK(declarationRank(Language::C, "typedef struct Node Node;", "Node") == 3, "typedef");
    CHECK(declarationRank(Language::Cpp, "LSPError LSPClient::gotoDefinition(const std::string& uri,", "gotoDefinition") == 2, "member definition");
    CHECK(declarationRank(Language::C, "static int helper(int x) {", "helper") == 2, "static function");
    CHECK(declarationRank(Language::Cpp, "    auto e = gotoDefinition(uri, pos);", "gotoDefinition") == 0, "call in an initializer");
    CHECK(declarationRank(Language::Cpp, "    return helper(1);", "helper") == 0, "return helper(1) is a call");
    CHECK(declarationRank(Language::Cpp, "    helper(1);", "helper") == 0, "statement call has a semicolon");
    CHECK(declarationRank(Language::Cpp, "    client->gotoDefinition(uri);", "gotoDefinition") == 0, "arrow call");
    CHECK(declarationRank(Language::Cpp, "- (void)applicationDidFinishLaunching:(NSNotification *)n {", "applicationDidFinishLaunching") == 2, "objc method");
    CHECK(declarationRank(Language::Cpp, "@interface Foo : NSObject", "Foo") == 3, "objc interface");
    CHECK(declarationRank(Language::Cpp, "    int count = 0;", "count") == 1, "variable");
    CHECK(declarationRank(Language::Cpp, "    return count;", "count") == 0, "return is not a type");
    CHECK(declarationRank(Language::Cpp, "extern int count;", "count") == 0, "extern declares, it does not define");
}

void testOtherLanguageRanks() {
    CHECK(declarationRank(Language::Python, "class Thing(Base):", "Thing") == 3, "py class");
    CHECK(declarationRank(Language::Python, "    async def run(self):", "run") == 2, "py async def");
    CHECK(declarationRank(Language::Python, "LIMIT = 5", "LIMIT") == 1, "py module constant");
    CHECK(declarationRank(Language::Python, "    x = run()", "run") == 0, "py call");
    CHECK(declarationRank(Language::Go, "type Server struct {", "Server") == 3, "go type");
    CHECK(declarationRank(Language::Go, "func (s *Server) Start() error {", "Start") == 2, "go method");
    CHECK(declarationRank(Language::Go, "func NewServer() *Server {", "NewServer") == 2, "go func");
    CHECK(declarationRank(Language::Go, "\tsrv := NewServer()", "NewServer") == 0, "go call");
    CHECK(declarationRank(Language::Rust, "pub struct Config {", "Config") == 3, "rust struct");
    CHECK(declarationRank(Language::Rust, "    pub fn load(path: &Path) -> Result<Self> {", "load") == 2, "rust fn");
    CHECK(declarationRank(Language::Rust, "    let mut buf = Vec::new();", "buf") == 1, "rust let");
    CHECK(declarationRank(Language::TypeScript, "export class Store {", "Store") == 3, "ts class");
    CHECK(declarationRank(Language::TypeScript, "export interface Props {", "Props") == 3, "ts interface");
    CHECK(declarationRank(Language::TypeScript, "const load = async (id: string) => {", "load") == 2, "ts arrow fn");
    CHECK(declarationRank(Language::JavaScript, "function load(id) {", "load") == 2, "js function");
    CHECK(declarationRank(Language::TypeScript, "  private render(): void {", "render") == 2, "ts method");
    CHECK(declarationRank(Language::TypeScript, "  await load(id);", "load") == 0, "ts call");
    CHECK(declarationRank(Language::Unknown, "public class Widget extends Base {", "Widget") == 3, "generic class (java)");
    CHECK(declarationRank(Language::Unknown, "    public static void main(String[] args) {", "main") == 2, "generic typed method");
    CHECK(declarationRank(Language::Unknown, "fun render(x: Int): Int {", "render") == 2, "generic fun (kotlin)");
    CHECK(declarationRank(Language::Unknown, "    val total = 3", "total") == 1, "generic val");
}

void testExtensionsFollowTheLanguageFamily() {
    auto sw = extensionsForSearch(Language::Swift, "/x/a.swift");
    CHECK(sw.size() == 1 && sw[0] == ".swift", "swift family");
    auto cpp = extensionsForSearch(Language::Cpp, "/x/a.cpp");
    CHECK(std::find(cpp.begin(), cpp.end(), ".h") != cpp.end(), "c++ includes headers");
    CHECK(std::find(cpp.begin(), cpp.end(), ".mm") != cpp.end(), "c++ includes objc++");
    auto unk = extensionsForSearch(Language::Unknown, "/x/Main.java");
    CHECK(unk.size() == 1 && unk[0] == ".java", "unknown language: the requesting file's extension");
    CHECK(extensionsForSearch(Language::Unknown, "Makefile").empty(), "no extension: any text file");
}

void testFindDeclarationsRanksAndScopesTheWorkspace() {
    TempDir t;
    touch(t.path / "GitReviewApp" / "Models" / "DocumentRenderModel.swift",
          "import Foundation\n\nstruct DocumentSliceBuilder {\n    let model: Int\n}\n");
    touch(t.path / "GitReviewApp" / "Views" / "Slice.swift",
          "struct SliceView {\n    var builder: DocumentSliceBuilder\n    func make() { let b = DocumentSliceBuilder(model: 1) }\n}\n");
    touch(t.path / "GitReviewApp" / "Views" / "Ext.swift",
          "extension DocumentSliceBuilder {\n    func extra() {}\n}\n");
    // Noise that must be skipped: hidden dirs, build output, other languages, binaries.
    touch(t.path / ".build" / "gen.swift", "struct DocumentSliceBuilder {}\n");
    touch(t.path / "build" / "gen.swift", "struct DocumentSliceBuilder {}\n");
    touch(t.path / "node_modules" / "x" / "gen.swift", "struct DocumentSliceBuilder {}\n");
    touch(t.path / "notes.md", "struct DocumentSliceBuilder is documented here\n");
    touch(t.path / "bin.swift", std::string("struct DocumentSliceBuilder {\0\0", 30));

    const std::string req = uriOf(t.path / "GitReviewApp" / "Views" / "Slice.swift");
    auto locs = findDeclarations(t.path.string(), Language::Swift, req, "DocumentSliceBuilder");
    CHECK(locs.size() == 2, "the struct and its extension, nothing from build/hidden/other files (got " + std::to_string(locs.size()) + ")");
    if (locs.size() >= 2) {
        // Both rank 3; the one in the requesting file's directory (Views/Ext.swift) comes first.
        CHECK(locs[0].uri == uriOf(t.path / "GitReviewApp" / "Views" / "Ext.swift"), "same-directory candidate first: " + locs[0].uri);
        CHECK(locs[1].uri == uriOf(t.path / "GitReviewApp" / "Models" / "DocumentRenderModel.swift"), "then the other");
        CHECK(locs[1].range.start.line == 2 && locs[1].range.start.character == 7, "range points at the name on the struct line");
        CHECK(locs[1].range.end.character == 7 + 20, "range spans the identifier");
    }

    // A requesting file that declares the symbol itself ranks first.
    const std::string req2 = uriOf(t.path / "GitReviewApp" / "Models" / "DocumentRenderModel.swift");
    auto locs2 = findDeclarations(t.path.string(), Language::Swift, req2, "DocumentSliceBuilder");
    CHECK(!locs2.empty() && locs2[0].uri == req2, "requesting file wins ties");

    // Lower-ranked declarations trail type declarations.
    touch(t.path / "GitReviewApp" / "Views" / "Var.swift", "let DocumentSliceBuilder = 3\n");
    auto locs3 = findDeclarations(t.path.string(), Language::Swift, req, "DocumentSliceBuilder");
    CHECK(locs3.size() == 3 && locs3.back().uri == uriOf(t.path / "GitReviewApp" / "Views" / "Var.swift"),
          "a `let` of the same name ranks below the type declarations");

    CHECK(findDeclarations(t.path.string(), Language::Swift, req, "").empty(), "empty name -> nothing");
    CHECK(findDeclarations(t.path.string(), Language::Swift, req, "a.b").empty(), "non-identifier -> nothing");
    CHECK(findDeclarations(t.path.string(), Language::Swift, req, "NoSuchThing").empty(), "unknown name -> nothing");
}

void testFindOccurrencesListsUsesAndCanDropDeclarations() {
    TempDir t;
    touch(t.path / "a.swift", "struct Foo {}\nlet x = Foo()\nlet y = Foo(); let z = Foo()\n");
    touch(t.path / "b.swift", "extension Foo {}\nfunc make() -> Foo { Foo() }\n");
    touch(t.path / "c.py", "Foo = 1\n");
    const std::string req = uriOf(t.path / "b.swift");
    auto all = findOccurrences(t.path.string(), Language::Swift, req, "Foo", true);
    CHECK(all.size() == 7, "every whole-word Swift occurrence incl. declarations (got " + std::to_string(all.size()) + ")");
    CHECK(!all.empty() && all[0].uri == req, "requesting file first");
    auto uses = findOccurrences(t.path.string(), Language::Swift, req, "Foo", false);
    CHECK(uses.size() == 5, "declaration lines dropped: 3 uses in a.swift + 2 on b.swift's func line (got " + std::to_string(uses.size()) + ")");
    if (all.size() >= 2) {
        // Two hits on one line keep distinct columns.
        bool sawTwoOnOneLine = false;
        for (std::size_t i = 1; i < all.size(); ++i)
            if (all[i].uri == all[i - 1].uri && all[i].range.start.line == all[i - 1].range.start.line &&
                all[i].range.start.character > all[i - 1].range.start.character)
                sawTwoOnOneLine = true;
        CHECK(sawTwoOnOneLine, "multiple occurrences on one line are separate locations");
    }
}

void testLimitsBoundTheWalk() {
    TempDir t;
    for (int i = 0; i < 30; ++i) touch(t.path / ("f" + std::to_string(i) + ".swift"), "struct Foo {}\n");
    TextSearchLimits limits;
    limits.maxFiles = 10;
    auto locs = findDeclarations(t.path.string(), Language::Swift, "", "Foo", limits);
    CHECK(locs.size() <= 10, "maxFiles caps the files visited (got " + std::to_string(locs.size()) + ")");
    limits.maxFiles = 1000;
    limits.maxResults = 4;
    locs = findDeclarations(t.path.string(), Language::Swift, "", "Foo", limits);
    CHECK(locs.size() == 4, "maxResults caps the answer");
    limits.maxResults = 500;
    limits.maxFileBytes = 4;
    locs = findDeclarations(t.path.string(), Language::Swift, "", "Foo", limits);
    CHECK(locs.empty(), "files over maxFileBytes are skipped");
}

void testUriHelpers() {
    CHECK(toFileUri("/a/b.swift") == "file:///a/b.swift", "path -> uri");
    CHECK(toFileUri("file:///a/b.swift") == "file:///a/b.swift", "uri passes through");
    CHECK(fromFileUri("file:///a/b.swift") == "/a/b.swift", "uri -> path");
    CHECK(fromFileUri("/a/b.swift") == "/a/b.swift", "path passes through");
}
}  // namespace

int main() {
    testIdentifierAtSelectsTheWordUnderAndJustPastTheCursor();
    testFindWordRespectsBoundaries();
    testSwiftDeclarationRanks();
    testCFamilyDeclarationRanks();
    testOtherLanguageRanks();
    testExtensionsFollowTheLanguageFamily();
    testFindDeclarationsRanksAndScopesTheWorkspace();
    testFindOccurrencesListsUsesAndCanDropDeclarations();
    testLimitsBoundTheWalk();
    testUriHelpers();
    if (g_failures == 0) std::cout << "declaration_search_tests: all passed" << std::endl;
    else std::cerr << "declaration_search_tests: " << g_failures << " failure(s)" << std::endl;
    return g_failures == 0 ? 0 : 1;
}
