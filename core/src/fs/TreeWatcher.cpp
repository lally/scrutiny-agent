// TreeWatcher.cpp
// Core `grc_watch_tree_*`: watch a working tree for external edits and
// fire a coalesced callback naming the paths that changed. The agent
// serves it as `watch.tree` / `watch.treeChanged`; the Mac's local
// backend calls it directly. Same lifecycle as the HEAD watch next
// door (a background thread, a self-pipe so destroy is prompt), but a
// different shape of answer: the HEAD watch says "something moved",
// this one says *what*, so a client can refresh the one file it shows
// instead of everything it holds.
//
// Linux: inotify, one watch per directory, added by an initial walk
// and on every directory creation. Darwin: one FSEvents stream over
// the root (per-file events). Both feed the same coalescing flush,
// which drops `.git` and git-ignored paths: libgit2's ignore rules are
// consulted when the root is a working tree, so a build directory's
// churn never reaches the client and never costs it a re-read.
#include "GitReviewCore.h"

#include <git2.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/stat.h>

#if defined(__linux__)
#include <sys/inotify.h>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#endif

namespace {

constexpr int kQuietMs = 250;       // coalesce until this quiet ...
constexpr int kMaxHoldMs = 1000;    // ... but never hold longer
constexpr size_t kMaxNamesPerNote = 256;   // beyond: reset
#if defined(__linux__)
constexpr size_t kMaxDirs = 32768;         // inotify watch budget
#endif

std::string canonical(const std::string& p) {
    char buf[PATH_MAX];
    if (::realpath(p.c_str(), buf) == nullptr) return "";
    return buf;
}

bool isDirectory(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// True when any '/'-separated component of `rel` is `.git`: the
// repository's own bookkeeping, or a submodule's.
bool underGitDir(const std::string& rel) {
    size_t start = 0;
    while (start <= rel.size()) {
        size_t slash = rel.find('/', start);
        const std::string comp =
            rel.substr(start, slash == std::string::npos ? std::string::npos
                                                          : slash - start);
        if (comp == ".git") return true;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return false;
}

}  // namespace

struct GRCTreeWatch {
    std::string root;   // canonical, no trailing slash
    GRCTreeChangeCallback cb = nullptr;
    void* ctx = nullptr;
    std::atomic<bool> running{true};
    int wake[2] = {-1, -1};
    std::thread th;

    // Ignore rules. Opened on the watcher thread and only ever used
    // there (libgit2 repository handles are not thread-safe).
    git_repository* repo = nullptr;

    void openIgnoreRules() {
        git_repository* r = nullptr;
        if (git_repository_open_ext(&r, root.c_str(),
                                    GIT_REPOSITORY_OPEN_NO_SEARCH,
                                    nullptr) == 0 &&
            r != nullptr && git_repository_is_bare(r) == 0) {
            repo = r;
        } else if (r != nullptr) {
            git_repository_free(r);
        }
    }

    void closeIgnoreRules() {
        if (repo != nullptr) git_repository_free(repo);
        repo = nullptr;
    }

    // Whether git ignores `rel` (a directory when `isDir`). libgit2
    // applies the parent-directory rules itself, so a file inside an
    // ignored directory answers true as well.
    bool ignored(const std::string& rel, bool isDir) {
        if (repo == nullptr || rel.empty()) return false;
        int ig = 0;
        const std::string probe = isDir ? rel + "/" : rel;
        if (git_ignore_path_is_ignored(&ig, repo, probe.c_str()) != 0)
            return false;
        return ig != 0;
    }

    // Deliver one coalesced batch: filter, sort, call back.
    void flush(std::set<std::string>& pending, bool& reset) {
        if (!running.load() || cb == nullptr) { pending.clear(); reset = false; return; }
        if (reset || pending.size() > kMaxNamesPerNote) {
            pending.clear();
            reset = false;
            cb(ctx, nullptr, 0, 1);
            return;
        }
        std::vector<std::string> keep;
        keep.reserve(pending.size());
        for (const auto& rel : pending) {
            if (rel.empty() || underGitDir(rel)) continue;
            const bool dir = isDirectory(root + "/" + rel);
            if (ignored(rel, dir)) continue;
            keep.push_back(rel);
        }
        pending.clear();
        if (keep.empty()) return;
        std::vector<const char*> ptrs;
        ptrs.reserve(keep.size());
        for (const auto& k : keep) ptrs.push_back(k.c_str());
        cb(ctx, ptrs.data(), static_cast<int>(ptrs.size()), 0);
    }

    bool waitStop(int ms) {
        struct pollfd p{wake[0], POLLIN, 0};
        int r = ::poll(&p, 1, ms);
        return r > 0 && (p.revents & POLLIN);
    }

    void loop();
};

#if defined(__linux__)

void GRCTreeWatch::loop() {
    openIgnoreRules();
    const int ifd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) { closeIgnoreRules(); return; }

    std::map<int, std::string> dirByWd;   // wd -> root-relative dir ("" = root)
    std::map<std::string, int> wdByDir;
    bool budgetExhausted = false;

    const uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                          IN_CLOSE_WRITE | IN_MODIFY | IN_ATTRIB |
                          IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;

    // Add a watch on `rel` (a directory) and everything under it that
    // is neither `.git` nor ignored. `newNames`, when given, collects
    // the entries found (a directory that appeared whole -- `cp -r`,
    // `git checkout` -- already holds files no event will name).
    std::function<void(const std::string&, std::set<std::string>*)> addTree =
        [&](const std::string& rel, std::set<std::string>* newNames) {
            if (!running.load() || budgetExhausted) return;
            if (wdByDir.count(rel) != 0) return;
            if (!rel.empty() && (underGitDir(rel) || ignored(rel, true))) return;
            if (dirByWd.size() >= kMaxDirs) { budgetExhausted = true; return; }
            const std::string abs = rel.empty() ? root : root + "/" + rel;
            const int wd = ::inotify_add_watch(ifd, abs.c_str(), mask);
            if (wd < 0) return;   // ENOSPC, EACCES, vanished, ...
            dirByWd[wd] = rel;
            wdByDir[rel] = wd;
            std::error_code ec;
            std::filesystem::directory_iterator it(abs, ec), end;
            for (; !ec && it != end; it.increment(ec)) {
                const std::string name = it->path().filename().string();
                if (name == ".git") continue;
                const std::string childRel = rel.empty() ? name : rel + "/" + name;
                if (newNames != nullptr) newNames->insert(childRel);
                std::error_code dec;
                if (it->is_directory(dec) && !it->is_symlink(dec))
                    addTree(childRel, newNames);
            }
        };
    addTree("", nullptr);

    std::set<std::string> pending;
    bool reset = false;
    bool holding = false;
    auto firstAt = std::chrono::steady_clock::now();
    auto lastAt = firstAt;
    alignas(struct inotify_event) char buf[64 * 1024];

    while (running.load()) {
        int timeout = -1;
        if (holding) {
            const auto now = std::chrono::steady_clock::now();
            const auto sinceFirst = std::chrono::duration_cast<std::chrono::milliseconds>(now - firstAt).count();
            const auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAt).count();
            const long untilQuiet = kQuietMs - sinceLast;
            const long untilHold = kMaxHoldMs - sinceFirst;
            timeout = static_cast<int>(std::max(0L, std::min(untilQuiet, untilHold)));
        }
        struct pollfd p[2] = {{ifd, POLLIN, 0}, {wake[0], POLLIN, 0}};
        const int r = ::poll(p, 2, timeout);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (p[1].revents & POLLIN) break;   // stop
        if (r > 0 && (p[0].revents & POLLIN)) {
            ssize_t n;
            while ((n = ::read(ifd, buf, sizeof(buf))) > 0) {
                for (char* q = buf; q < buf + n;) {
                    auto* e = reinterpret_cast<struct inotify_event*>(q);
                    q += sizeof(struct inotify_event) + e->len;
                    if (e->mask & IN_Q_OVERFLOW) { reset = true; continue; }
                    auto it = dirByWd.find(e->wd);
                    if (it == dirByWd.end()) continue;
                    const std::string dirRel = it->second;
                    if (e->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) {
                        wdByDir.erase(dirRel);
                        dirByWd.erase(it);
                        if (dirRel.empty()) reset = true;   // the root itself
                        continue;
                    }
                    if (e->len == 0) continue;
                    const std::string name(e->name);
                    if (name == ".git") continue;
                    const std::string rel = dirRel.empty() ? name : dirRel + "/" + name;
                    pending.insert(rel);
                    if ((e->mask & IN_ISDIR) && (e->mask & (IN_CREATE | IN_MOVED_TO)))
                        addTree(rel, &pending);
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (!holding) { holding = true; firstAt = now; }
            lastAt = now;
        }
        if (holding) {
            const auto now = std::chrono::steady_clock::now();
            const auto sinceFirst = std::chrono::duration_cast<std::chrono::milliseconds>(now - firstAt).count();
            const auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAt).count();
            if (sinceLast >= kQuietMs || sinceFirst >= kMaxHoldMs) {
                holding = false;
                flush(pending, reset);
            }
        }
    }
    ::close(ifd);
    closeIgnoreRules();
}

#elif defined(__APPLE__)

namespace {

// What the FSEvents callback hands the watcher thread. The callback
// runs on a private dispatch queue and only ever appends here; the
// watcher thread owns the ignore rules and does the filtering.
struct DarwinInbox {
    std::mutex m;
    std::condition_variable cv;
    std::set<std::string> pending;
    bool reset = false;
    bool stop = false;
    std::string root;
};

void fsEventsCallback(ConstFSEventStreamRef, void* info, size_t numEvents,
                      void* eventPaths, const FSEventStreamEventFlags* flags,
                      const FSEventStreamEventId*) {
    auto* inbox = static_cast<DarwinInbox*>(info);
    auto** paths = static_cast<char**>(eventPaths);
    std::lock_guard<std::mutex> l(inbox->m);
    for (size_t i = 0; i < numEvents; ++i) {
        const FSEventStreamEventFlags f = flags[i];
        if (f & (kFSEventStreamEventFlagMustScanSubDirs |
                 kFSEventStreamEventFlagKernelDropped |
                 kFSEventStreamEventFlagUserDropped |
                 kFSEventStreamEventFlagRootChanged)) {
            inbox->reset = true;
            continue;
        }
        std::string p = paths[i];
        while (!p.empty() && p.back() == '/') p.pop_back();
        if (p == inbox->root) { inbox->reset = true; continue; }
        if (p.size() <= inbox->root.size() ||
            p.compare(0, inbox->root.size(), inbox->root) != 0 ||
            p[inbox->root.size()] != '/')
            continue;
        inbox->pending.insert(p.substr(inbox->root.size() + 1));
    }
    inbox->cv.notify_one();
}

}  // namespace

void GRCTreeWatch::loop() {
    openIgnoreRules();
    DarwinInbox inbox;
    inbox.root = root;

    CFStringRef cfRoot = CFStringCreateWithCString(kCFAllocatorDefault, root.c_str(),
                                                   kCFStringEncodingUTF8);
    CFArrayRef cfPaths = CFArrayCreate(kCFAllocatorDefault,
                                       reinterpret_cast<const void**>(&cfRoot), 1,
                                       &kCFTypeArrayCallBacks);
    FSEventStreamContext sctx{0, &inbox, nullptr, nullptr, nullptr};
    FSEventStreamRef stream = FSEventStreamCreate(
        kCFAllocatorDefault, &fsEventsCallback, &sctx, cfPaths,
        kFSEventStreamEventIdSinceNow, 0.05,
        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
    CFRelease(cfPaths);
    CFRelease(cfRoot);
    dispatch_queue_t queue = dispatch_queue_create("info.fiverivers.scrutiny.treewatch",
                                                   DISPATCH_QUEUE_SERIAL);
    bool started = false;
    if (stream != nullptr) {
        FSEventStreamSetDispatchQueue(stream, queue);
        started = FSEventStreamStart(stream);
    }

    // A stop request arrives on the self-pipe (the C API) but the
    // inbox is condvar-driven: a tiny thread bridges the two.
    std::thread stopBridge([this, &inbox] {
        waitStop(-1);
        std::lock_guard<std::mutex> l(inbox.m);
        inbox.stop = true;
        inbox.cv.notify_all();
    });

    if (started) {
        std::unique_lock<std::mutex> l(inbox.m);
        while (!inbox.stop) {
            if (inbox.pending.empty() && !inbox.reset) {
                inbox.cv.wait(l);
                continue;
            }
            // Hold until quiet or the cap, whichever first.
            const auto firstAt = std::chrono::steady_clock::now();
            while (!inbox.stop) {
                const size_t before = inbox.pending.size();
                const bool resetBefore = inbox.reset;
                const auto now = std::chrono::steady_clock::now();
                const auto sinceFirst = std::chrono::duration_cast<std::chrono::milliseconds>(now - firstAt).count();
                if (sinceFirst >= kMaxHoldMs) break;
                const long remaining = std::min<long>(kQuietMs, kMaxHoldMs - sinceFirst);
                inbox.cv.wait_for(l, std::chrono::milliseconds(remaining));
                if (inbox.pending.size() == before && inbox.reset == resetBefore) break;
            }
            if (inbox.stop) break;
            std::set<std::string> batch;
            batch.swap(inbox.pending);
            bool reset = inbox.reset;
            inbox.reset = false;
            l.unlock();
            flush(batch, reset);
            l.lock();
        }
    } else {
        // Could not start the stream: stay alive (inert) until stopped,
        // like the HEAD watch on an unsupported platform.
        std::unique_lock<std::mutex> l(inbox.m);
        inbox.cv.wait(l, [&] { return inbox.stop; });
    }

    // Make sure the bridge thread has seen the stop (destroy wrote the
    // pipe before joining us; if the loop exited for another reason,
    // write it ourselves).
    if (wake[1] >= 0) { const char b = 1; (void)!::write(wake[1], &b, 1); }
    stopBridge.join();

    if (stream != nullptr) {
        if (started) FSEventStreamStop(stream);
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
    }
    dispatch_release(queue);
    closeIgnoreRules();
}

#else  // unsupported platform: inert watch (callback never fires)

void GRCTreeWatch::loop() {
    while (running.load()) { if (waitStop(1000)) return; }
}

#endif

extern "C" {

GRCTreeWatch* grc_watch_tree_create(const char* root_path,
                                    GRCTreeChangeCallback callback,
                                    void* context) {
    if (root_path == nullptr || callback == nullptr) return nullptr;
    const std::string root = canonical(root_path);
    if (root.empty() || !isDirectory(root)) return nullptr;
    git_libgit2_init();
    auto* w = new GRCTreeWatch();
    w->root = root;
    w->cb = callback;
    w->ctx = context;
    if (::pipe(w->wake) != 0) { delete w; git_libgit2_shutdown(); return nullptr; }
    ::fcntl(w->wake[0], F_SETFL, O_NONBLOCK);
    ::fcntl(w->wake[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(w->wake[1], F_SETFD, FD_CLOEXEC);
    w->th = std::thread([w] { w->loop(); });
    return w;
}

void grc_watch_tree_destroy(GRCTreeWatch* watch) {
    if (watch == nullptr) return;
    watch->running.store(false);
    if (watch->wake[1] >= 0) {
        const char b = 1;
        ssize_t wr = ::write(watch->wake[1], &b, 1);
        (void)wr;
    }
    if (watch->th.joinable()) watch->th.join();
    if (watch->wake[0] >= 0) ::close(watch->wake[0]);
    if (watch->wake[1] >= 0) ::close(watch->wake[1]);
    delete watch;
    git_libgit2_shutdown();
}

}  // extern "C"
