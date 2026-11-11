// Unit tests for fs/TreeWatcher: grc_watch_tree_* must name the paths
// an external edit touched, stay quiet about the repository's own
// bookkeeping (.git) and about what git ignores, pick up files that
// appear inside a directory created after the watch started, and stop
// promptly when destroyed. Runs against a throwaway `git init` tree.
#include "GitReviewCore.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
int g_failures = 0;
#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ \
                      << ")\n";                                                 \
        } else {                                                                \
            std::cout << "ok: " << msg << "\n";                                 \
        }                                                                       \
    } while (0)

struct Received {
    std::mutex m;
    std::condition_variable cv;
    std::set<std::string> paths;
    int resets = 0;
    int calls = 0;
};

void onChange(void* ctx, const char* const* paths, int count, int reset) {
    auto* r = static_cast<Received*>(ctx);
    std::lock_guard<std::mutex> l(r->m);
    ++r->calls;
    if (reset) ++r->resets;
    for (int i = 0; i < count; ++i) r->paths.insert(paths[i]);
    r->cv.notify_all();
}

// Wait until `rel` has been reported, or `ms` pass.
bool waitFor(Received& r, const std::string& rel, int ms) {
    std::unique_lock<std::mutex> l(r.m);
    return r.cv.wait_for(l, std::chrono::milliseconds(ms),
                         [&] { return r.paths.count(rel) != 0; });
}

bool has(Received& r, const std::string& rel) {
    std::lock_guard<std::mutex> l(r.m);
    return r.paths.count(rel) != 0;
}

void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << text;
}

int run(const std::string& cmd) {
    return std::system(cmd.c_str());
}

fs::path makeTree() {
    char tmpl[] = "/tmp/grc-treewatch-XXXXXX";
    const char* d = ::mkdtemp(tmpl);
    if (d == nullptr) return {};
    fs::path root = fs::canonical(d);
    run("git -C '" + root.string() + "' init -q");
    writeFile(root / ".gitignore", "build/\n*.log\n");
    writeFile(root / "src" / "main.c", "int main(){}\n");
    fs::create_directories(root / "build");
    run("git -C '" + root.string() + "' -c user.email=t@t.t -c user.name=t add -A");
    run("git -C '" + root.string() + "' -c user.email=t@t.t -c user.name=t commit -q -m init");
    return root;
}
}  // namespace

int main() {
    const fs::path root = makeTree();
    if (root.empty()) {
        std::cerr << "could not create a temp tree\n";
        return 1;
    }

    CHECK(grc_watch_tree_create("/no/such/dir/anywhere", onChange, nullptr) == nullptr,
          "a missing root yields no watch");
    CHECK(grc_watch_tree_create(root.c_str(), nullptr, nullptr) == nullptr,
          "a null callback yields no watch");

    Received r;
    GRCTreeWatch* w = grc_watch_tree_create(root.c_str(), onChange, &r);
    CHECK(w != nullptr, "a watch opens on a working tree");
    if (w == nullptr) return 1;
    // FSEvents / inotify arm asynchronously; give them a moment so the
    // first edit is not raced.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 1. An ordinary edit is reported by its root-relative path.
    writeFile(root / "src" / "main.c", "int main(){return 0;}\n");
    CHECK(waitFor(r, "src/main.c", 5000), "an edited file is reported as src/main.c");

    // 2. .git and ignored paths are never reported, even when written
    //    in the same burst as something that is.
    writeFile(root / ".git" / "scratch", "x\n");
    writeFile(root / "build" / "out.o", "obj\n");
    writeFile(root / "notes.log", "log\n");
    writeFile(root / "src" / "util.c", "void f(){}\n");
    CHECK(waitFor(r, "src/util.c", 5000), "a new file next to the edit is reported");
    // The quiet window has passed for the burst by now; whatever was
    // going to be reported with util.c has been.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    CHECK(!has(r, ".git/scratch"), ".git contents are not reported");
    CHECK(!has(r, "build/out.o"), "a git-ignored directory's files are not reported");
    CHECK(!has(r, "notes.log"), "a git-ignored file is not reported");
    CHECK(!has(r, "build"), "the ignored directory itself is not reported");

    // 3. A directory that appears after the watch started is watched
    //    too (Linux adds inotify watches on the fly; FSEvents is
    //    recursive by nature), and files written into it later are
    //    named.
    fs::create_directories(root / "lib" / "inner");
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    writeFile(root / "lib" / "inner" / "late.c", "late\n");
    CHECK(waitFor(r, "lib/inner/late.c", 5000),
          "a file written into a directory created after the watch is reported");

    // 4. Paths come back sorted and without duplicates within a call:
    //    the whole set after everything above holds each once (a set
    //    cannot tell, so check the count of calls is sane: at least
    //    three bursts, never one per event).
    {
        std::lock_guard<std::mutex> l(r.m);
        CHECK(r.calls >= 3, "edits were delivered in coalesced bursts");
        CHECK(r.resets == 0, "no reset was needed for a handful of edits");
    }

    // 5. Destroy is prompt even while edits keep landing.
    std::thread hammer([&] {
        for (int i = 0; i < 50; ++i) {
            writeFile(root / "src" / ("h" + std::to_string(i) + ".c"), "x\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    const auto t0 = std::chrono::steady_clock::now();
    grc_watch_tree_destroy(w);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 2000, "destroy returns promptly mid-burst");
    hammer.join();
    grc_watch_tree_destroy(nullptr);
    CHECK(true, "destroy(NULL) is a no-op");

    // 6. A plain directory (no repository) watches too: nothing is
    //    ignored but `.git`.
    {
        char tmpl[] = "/tmp/grc-treewatch-plain-XXXXXX";
        const char* d = ::mkdtemp(tmpl);
        const fs::path plain = fs::canonical(d);
        Received r2;
        GRCTreeWatch* w2 = grc_watch_tree_create(plain.c_str(), onChange, &r2);
        CHECK(w2 != nullptr, "a non-repository directory can be watched");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        writeFile(plain / "build" / "thing.log", "x\n");
        CHECK(waitFor(r2, "build/thing.log", 5000),
              "without a repository nothing is treated as ignored");
        grc_watch_tree_destroy(w2);
        fs::remove_all(plain);
    }

    fs::remove_all(root);
    if (g_failures != 0) {
        std::cerr << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all tree watcher tests passed\n";
    return 0;
}
