// DeclarationSearch.cpp -- see DeclarationSearch.hpp.
#include "lsp/DeclarationSearch.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace gitreview {
namespace lsp {

namespace {

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0])) != 0) return false;
    return std::all_of(s.begin(), s.end(), isIdentChar);
}

std::string lowerExt(const std::string& file) {
    const auto dot = file.rfind('.');
    const auto slash = file.rfind('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    std::string e = file.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

std::string trimLeft(const std::string& s) {
    const auto p = s.find_first_not_of(" \t");
    return p == std::string::npos ? "" : s.substr(p);
}

std::string trimRight(const std::string& s) {
    const auto p = s.find_last_not_of(" \t\r");
    return p == std::string::npos ? "" : s.substr(0, p + 1);
}

bool startsWithKeyword(const std::string& line, std::initializer_list<const char*> words) {
    const std::string t = trimLeft(line);
    for (const char* w : words) {
        const std::size_t n = std::strlen(w);
        if (t.compare(0, n, w) == 0 && (t.size() == n || !isIdentChar(t[n]))) return true;
    }
    return false;
}

/// One declaration-shaped pattern: the regex (with "N" already
/// substituted) and the rank it awards. Some rules carry extra
/// line-level checks that regex alone expresses badly.
struct Rule {
    std::regex re;
    int rank;
    enum Extra { None, NotForwardDecl, CFunctionDef, CVariableDef } extra = None;
};

std::string sub(const std::string& pattern, const std::string& name) {
    std::string out = pattern;
    const std::string placeholder = "<NAME>";
    std::size_t p = 0;
    while ((p = out.find(placeholder, p)) != std::string::npos) {
        out.replace(p, placeholder.size(), name);
        p += name.size();
    }
    return out;
}

Rule rule(const char* pattern, const std::string& name, int rank, Rule::Extra extra = Rule::None) {
    return Rule{std::regex(sub(pattern, name), std::regex::ECMAScript), rank, extra};
}

std::vector<Rule> rulesFor(Language language, const std::string& name) {
    std::vector<Rule> r;
    switch (language) {
    case Language::Swift:
        r.push_back(rule(R"(\b(?:class|struct|enum|protocol|actor|typealias|associatedtype|extension|macro)\s+<NAME>\b)", name, 3));
        r.push_back(rule(R"(\bfunc\s+<NAME>\s*[(<])", name, 2));
        r.push_back(rule(R"(\b(?:var|let|case)\s+<NAME>\b)", name, 1));
        break;
    case Language::C:
    case Language::Cpp:
        r.push_back(rule(R"(\b(?:class|struct|enum(?:\s+class)?|union|namespace)\s+<NAME>\b)", name, 3, Rule::NotForwardDecl));
        r.push_back(rule(R"(@(?:interface|implementation|protocol)\s+<NAME>\b)", name, 3));
        r.push_back(rule(R"(#\s*define\s+<NAME>\b)", name, 3));
        r.push_back(rule(R"(\busing\s+<NAME>\s*=)", name, 3));
        r.push_back(rule(R"(\btypedef\b.*\b<NAME>\s*;)", name, 3));
        r.push_back(rule(R"(^\s*[-+]\s*\([^)]*\)\s*<NAME>\b)", name, 2));
        r.push_back(rule(R"((?:^|[\s*&:~])<NAME>\s*\()", name, 2, Rule::CFunctionDef));
        r.push_back(rule(R"([\w>*&]\s+\**<NAME>\s*(?:=(?!=)|;|\[|\{))", name, 1, Rule::CVariableDef));
        break;
    case Language::Python:
        r.push_back(rule(R"(^\s*class\s+<NAME>\b)", name, 3));
        r.push_back(rule(R"(^\s*(?:async\s+)?def\s+<NAME>\s*\()", name, 2));
        r.push_back(rule(R"(^\s*<NAME>\s*(?::[^=]+)?=(?!=))", name, 1));
        break;
    case Language::Go:
        r.push_back(rule(R"(^\s*type\s+<NAME>\b)", name, 3));
        r.push_back(rule(R"(^\s*func\s+(?:\([^)]*\)\s*)?<NAME>\s*[(\[])", name, 2));
        r.push_back(rule(R"(^\s*(?:var|const)\s+<NAME>\b)", name, 1));
        r.push_back(rule(R"(^\s*<NAME>\s*(?:,\s*\w+\s*)?:=)", name, 1));
        break;
    case Language::Rust:
        r.push_back(rule(R"(\b(?:struct|enum|trait|type|mod|union)\s+<NAME>\b)", name, 3));
        r.push_back(rule(R"(\bmacro_rules!\s*<NAME>\b)", name, 3));
        r.push_back(rule(R"(\bfn\s+<NAME>\s*[(<])", name, 2));
        r.push_back(rule(R"(\b(?:const|static|let)\s+(?:mut\s+)?<NAME>\b)", name, 1));
        break;
    case Language::JavaScript:
    case Language::TypeScript:
        r.push_back(rule(R"(\b(?:class|interface|type|enum|namespace)\s+<NAME>\b)", name, 3));
        r.push_back(rule(R"(\bfunction\s*\*?\s*<NAME>\s*[(<])", name, 2));
        r.push_back(rule(R"(\b<NAME>\s*[:=]\s*(?:async\s*)?(?:function\b|\([^)]*\)\s*(?::[^=]*)?=>|[A-Za-z_$][\w$]*\s*=>))", name, 2));
        r.push_back(rule(R"(^\s*(?:(?:export|static|async|public|private|protected|readonly|get|set|override)\s+)*<NAME>\s*\([^)]*\)\s*(?::[^{;]+)?\{)", name, 2));
        r.push_back(rule(R"(\b(?:const|let|var)\s+<NAME>\b)", name, 1));
        break;
    default:
        r.push_back(rule(R"(\b(?:class|struct|enum|interface|trait|object|record|type|typealias|module|namespace|protocol|actor|union)\s+<NAME>\b)", name, 3));
        r.push_back(rule(R"(\b(?:def|fn|func|function|fun|sub|proc|method|defp|defmacro)\s+<NAME>\b)", name, 2));
        r.push_back(rule(R"(^\s*(?:[\w<>\[\],.?:*&]+\s+)+<NAME>\s*\([^;]*$)", name, 2));
        r.push_back(rule(R"(\b(?:val|var|let|const|static|final|my|our|local)\s+<NAME>\b)", name, 1));
        r.push_back(rule(R"(^\s*<NAME>\s*=(?!=))", name, 1));
        break;
    }
    return r;
}

bool passesExtra(const Rule& rule, const std::string& line, const std::string& name, const std::smatch& m) {
    switch (rule.extra) {
    case Rule::None:
        return true;
    case Rule::NotForwardDecl: {
        // `struct N;` / `class N;` announce, they do not define.
        const std::string t = trimRight(line);
        return !(t.size() > 0 && t.back() == ';' && t.find('{') == std::string::npos);
    }
    case Rule::CVariableDef: {
        // `int count = 0;` yes; `return count;` / `class Foo;` /
        // `extern int count;` no: the line's first word and the word
        // before the name decide.
        if (startsWithKeyword(line, {"extern", "return", "throw", "delete", "goto", "case", "friend", "using"}))
            return false;
        const std::size_t col = findWord(line, name, static_cast<std::size_t>(m.position(0)));
        if (col == std::string::npos) return false;
        std::size_t e = col;
        while (e > 0 && (line[e - 1] == ' ' || line[e - 1] == '\t' || line[e - 1] == '*' || line[e - 1] == '&')) --e;
        std::size_t b = e;
        while (b > 0 && isIdentChar(line[b - 1])) --b;
        static const std::unordered_set<std::string> kNotATypeword = {
            "class", "struct", "enum", "union", "namespace", "return", "new", "delete", "throw",
            "typedef", "using", "friend", "goto", "case", "else", "sizeof", "extern", "co_return",
            "co_yield", "await", "yield", "import", "export", "throws"};
        return kNotATypeword.count(line.substr(b, e - b)) == 0;
    }
    case Rule::CFunctionDef: {
        // A definition line: no statement terminator, not a control
        // statement or a call inside an expression.
        if (line.find(';') != std::string::npos) return false;
        if (startsWithKeyword(line, {"return", "if", "else", "while", "for", "switch", "case",
                                     "do", "throw", "new", "delete", "sizeof", "co_return", "co_await"}))
            return false;
        const std::string before = line.substr(0, static_cast<std::size_t>(m.position(0)));
        if (before.find('=') != std::string::npos || before.find('(') != std::string::npos ||
            before.find("return ") != std::string::npos)
            return false;
        // `foo(` must be the identifier itself, not `.foo(` / `->foo(`.
        const std::size_t col = findWord(line, name, static_cast<std::size_t>(m.position(0)));
        if (col == std::string::npos) return false;
        if (col >= 1 && line[col - 1] == '.') return false;
        if (col >= 2 && line[col - 2] == '-' && line[col - 1] == '>') return false;
        return true;
    }
    }
    return true;
}

const std::unordered_set<std::string>& skippedDirs() {
    static const std::unordered_set<std::string> s = {
        "node_modules", "DerivedData", "Pods", "build", "Build", "_build", "target",
        "dist", "out", "__pycache__", "venv", "bin", "obj", "vendor"};
    return s;
}

bool looksBinary(const std::string& head) {
    return head.find('\0') != std::string::npos;
}

struct FileVisit {
    std::string path;
    std::string content;
};

/// Walk the workspace, visiting text files whose extension is in `exts`
/// (any extension when `exts` is empty). Bounded by `limits`.
template <typename F>
void forEachSourceFile(const std::string& workspacePath, const std::vector<std::string>& exts,
                       const TextSearchLimits& limits, F&& visit) {
    std::error_code ec;
    std::size_t visited = 0;
    fs::recursive_directory_iterator it(workspacePath,
                                        fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    while (!ec && it != end) {
        const fs::directory_entry& e = *it;
        const std::string name = e.path().filename().string();
        if (e.is_directory(ec)) {
            if ((!name.empty() && name[0] == '.') || skippedDirs().count(name) > 0) {
                it.disable_recursion_pending();
            }
            it.increment(ec);
            continue;
        }
        if (!e.is_regular_file(ec)) { it.increment(ec); continue; }
        if (++visited > limits.maxFiles) {
            spdlog::info("[DeclarationSearch] stopped after {} files under {}", limits.maxFiles, workspacePath);
            return;
        }
        const std::string ext = lowerExt(name);
        if (!exts.empty() && std::find(exts.begin(), exts.end(), ext) == exts.end()) {
            it.increment(ec);
            continue;
        }
        std::error_code sec;
        const auto size = e.file_size(sec);
        if (sec || size > limits.maxFileBytes) { it.increment(ec); continue; }
        std::ifstream in(e.path(), std::ios::binary);
        if (in) {
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (!looksBinary(content.substr(0, 4096))) {
                if (!visit(FileVisit{e.path().string(), std::move(content)})) return;
            }
        }
        it.increment(ec);
    }
}

std::string parentDir(const std::string& path) {
    const auto p = path.rfind('/');
    return p == std::string::npos ? "" : path.substr(0, p);
}

LSPLocation makeLocation(const std::string& path, int line, std::size_t col, std::size_t len) {
    LSPLocation loc;
    loc.uri = toFileUri(path);
    loc.range.start.line = line;
    loc.range.start.character = static_cast<int>(col);
    loc.range.end.line = line;
    loc.range.end.character = static_cast<int>(col + len);
    return loc;
}

struct Ranked {
    LSPLocation location;
    int rank;
    int proximity;  // 0 = requesting file, 1 = same directory, 2 = elsewhere
};

int proximityOf(const std::string& path, const std::string& requestingFile) {
    if (requestingFile.empty()) return 2;
    if (path == requestingFile) return 0;
    if (parentDir(path) == parentDir(requestingFile)) return 1;
    return 2;
}

}  // namespace

std::string toFileUri(const std::string& path) {
    if (path.compare(0, 7, "file://") == 0) return path;
    return "file://" + path;
}

std::string fromFileUri(const std::string& uri) {
    if (uri.compare(0, 7, "file://") == 0) return uri.substr(7);
    return uri;
}

std::string identifierAt(const std::string& content, int line, int character) {
    if (line < 0 || character < 0) return "";
    std::size_t pos = 0;
    for (int cur = 0; cur < line; ++cur) {
        const auto nl = content.find('\n', pos);
        if (nl == std::string::npos) return "";
        pos = nl + 1;
    }
    auto end = content.find('\n', pos);
    if (end == std::string::npos) end = content.size();
    const std::string l = content.substr(pos, end - pos);
    std::size_t c = static_cast<std::size_t>(character);
    if (c >= l.size() || !isIdentChar(l[c])) {
        if (c >= 1 && c - 1 < l.size() && isIdentChar(l[c - 1])) c -= 1;
        else return "";
    }
    std::size_t s = c;
    while (s > 0 && isIdentChar(l[s - 1])) --s;
    std::size_t e = c;
    while (e < l.size() && isIdentChar(l[e])) ++e;
    const std::string word = l.substr(s, e - s);
    return isIdentifier(word) ? word : "";
}

std::vector<std::string> extensionsForSearch(Language language, const std::string& requestingFile) {
    switch (language) {
    case Language::Swift:      return {".swift"};
    case Language::C:
    case Language::Cpp:        return {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".hxx",
                                       ".m", ".mm", ".inl", ".ipp"};
    case Language::Python:     return {".py", ".pyi"};
    case Language::JavaScript:
    case Language::TypeScript: return {".js", ".jsx", ".mjs", ".cjs", ".ts", ".tsx", ".mts", ".cts"};
    case Language::Go:         return {".go"};
    case Language::Rust:       return {".rs"};
    default: {
        const std::string e = lowerExt(requestingFile);
        if (!e.empty()) return {e};
        return {};
    }
    }
}

std::size_t findWord(const std::string& line, const std::string& name, std::size_t from) {
    if (name.empty()) return std::string::npos;
    std::size_t p = line.find(name, from);
    while (p != std::string::npos) {
        const bool leftOk = p == 0 || !isIdentChar(line[p - 1]);
        const std::size_t after = p + name.size();
        const bool rightOk = after >= line.size() || !isIdentChar(line[after]);
        if (leftOk && rightOk) return p;
        p = line.find(name, p + 1);
    }
    return std::string::npos;
}

int declarationRank(Language language, const std::string& line, const std::string& name) {
    if (!isIdentifier(name) || findWord(line, name) == std::string::npos) return 0;
    int best = 0;
    for (const Rule& r : rulesFor(language, name)) {
        if (r.rank <= best) continue;
        std::smatch m;
        if (std::regex_search(line, m, r.re) && passesExtra(r, line, name, m)) best = r.rank;
    }
    return best;
}

std::vector<LSPLocation> findDeclarations(const std::string& workspacePath, Language language,
                                          const std::string& requestingFile, const std::string& name,
                                          const TextSearchLimits& limits) {
    std::vector<LSPLocation> out;
    if (!isIdentifier(name)) return out;
    const auto rules = rulesFor(language, name);
    const auto exts = extensionsForSearch(language, requestingFile);
    const std::string reqPath = fromFileUri(requestingFile);
    std::vector<Ranked> found;

    forEachSourceFile(workspacePath, exts, limits, [&](const FileVisit& f) {
        if (f.content.find(name) == std::string::npos) return true;
        std::istringstream in(f.content);
        std::string line;
        int lineNo = 0;
        for (; std::getline(in, line); ++lineNo) {
            if (line.find(name) == std::string::npos) continue;
            int best = 0;
            for (const Rule& r : rules) {
                if (r.rank <= best) continue;
                std::smatch m;
                if (std::regex_search(line, m, r.re) && passesExtra(r, line, name, m)) best = r.rank;
            }
            if (best == 0) continue;
            const std::size_t col = findWord(line, name);
            found.push_back(Ranked{makeLocation(f.path, lineNo, col, name.size()), best,
                                   proximityOf(f.path, reqPath)});
        }
        return true;
    });

    std::stable_sort(found.begin(), found.end(), [](const Ranked& a, const Ranked& b) {
        if (a.rank != b.rank) return a.rank > b.rank;
        if (a.proximity != b.proximity) return a.proximity < b.proximity;
        if (a.location.uri != b.location.uri) return a.location.uri < b.location.uri;
        return a.location.range.start.line < b.location.range.start.line;
    });
    for (const Ranked& r : found) {
        if (out.size() >= limits.maxResults) break;
        out.push_back(r.location);
    }
    spdlog::info("[DeclarationSearch] '{}' under {}: {} declaration candidates", name, workspacePath, out.size());
    return out;
}

std::vector<LSPLocation> findOccurrences(const std::string& workspacePath, Language language,
                                         const std::string& requestingFile, const std::string& name,
                                         bool includeDeclarations, const TextSearchLimits& limits) {
    std::vector<LSPLocation> out;
    if (!isIdentifier(name)) return out;
    const auto rules = rulesFor(language, name);
    const auto exts = extensionsForSearch(language, requestingFile);
    const std::string reqPath = fromFileUri(requestingFile);
    std::vector<Ranked> found;

    forEachSourceFile(workspacePath, exts, limits, [&](const FileVisit& f) {
        if (f.content.find(name) == std::string::npos) return true;
        std::istringstream in(f.content);
        std::string line;
        int lineNo = 0;
        for (; std::getline(in, line); ++lineNo) {
            std::size_t col = findWord(line, name);
            if (col == std::string::npos) continue;
            if (!includeDeclarations) {
                bool isDecl = false;
                for (const Rule& r : rules) {
                    std::smatch m;
                    if (std::regex_search(line, m, r.re) && passesExtra(r, line, name, m)) { isDecl = true; break; }
                }
                if (isDecl) continue;
            }
            while (col != std::string::npos) {
                found.push_back(Ranked{makeLocation(f.path, lineNo, col, name.size()), 0,
                                       proximityOf(f.path, reqPath)});
                col = findWord(line, name, col + name.size());
            }
        }
        return found.size() < limits.maxResults * 4;
    });

    std::stable_sort(found.begin(), found.end(), [](const Ranked& a, const Ranked& b) {
        if (a.proximity != b.proximity) return a.proximity < b.proximity;
        if (a.location.uri != b.location.uri) return a.location.uri < b.location.uri;
        if (a.location.range.start.line != b.location.range.start.line)
            return a.location.range.start.line < b.location.range.start.line;
        return a.location.range.start.character < b.location.range.start.character;
    });
    for (const Ranked& r : found) {
        if (out.size() >= limits.maxResults) break;
        out.push_back(r.location);
    }
    spdlog::info("[DeclarationSearch] '{}' under {}: {} occurrences", name, workspacePath, out.size());
    return out;
}

}  // namespace lsp
}  // namespace gitreview
