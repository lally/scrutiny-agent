// HeadWatcher.cpp
// Core `grc_watch_*`: watch a clone's HEAD ref for external git changes
// (fetch/pull/checkout/rebase) and fire a debounced, coalesced callback.
// This is the agent-side equivalent of the Mac's BranchHeadWatcher:
// same HEAD-path resolution (regular clone or linked-worktree `.git`
// file with a `gitdir:` pointer), the same atomic-replace handling
// (git swaps HEAD via `HEAD.lock` -> HEAD rename, which deletes the
// watched inode -> re-arm on the new file), and the same ~250 ms
// debounce. Linux uses inotify (the real agent target); Darwin uses
// kqueue (host/CI tests). A self-pipe makes grc_watch_destroy prompt.

#include "GitReviewCore.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/inotify.h>
#include <limits.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#endif

namespace {

constexpr int kDebounceMs = 250;
constexpr int kRenameSettleMs = 50;

// Mirror of Swift `resolveHeadPath`: regular clone -> <clone>/.git/HEAD;
// linked worktree -> follow the `.git` file's `gitdir:` pointer.
std::string resolveHeadPath(const std::string& clonePath) {
    const std::string gitPath = clonePath + "/.git";
    std::error_code ec;
    const auto st = std::filesystem::status(gitPath, ec);
    if (ec) return "";
    if (std::filesystem::is_directory(st)) return gitPath + "/HEAD";
    if (!std::filesystem::is_regular_file(st)) return "";
    std::ifstream f(gitPath);
    if (!f) return "";
    std::string line;
    while (std::getline(f, line)) {
        // trim
        size_t a = line.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        size_t b = line.find_last_not_of(" \t\r\n");
        const std::string t = line.substr(a, b - a + 1);
        const std::string key = "gitdir:";
        if (t.rfind(key, 0) != 0) continue;
        std::string dir = t.substr(key.size());
        size_t c = dir.find_first_not_of(" \t");
        if (c == std::string::npos) return "";
        dir = dir.substr(c);
        const std::string resolved =
            (!dir.empty() && dir[0] == '/') ? dir : clonePath + "/" + dir;
        return resolved + "/HEAD";
    }
    return "";
}

std::string parentDir(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? "." : p.substr(0, s);
}

}  // namespace

struct GRCWatch {
    std::string clonePath;
    GRCHeadChangeCallback cb;
    void* ctx;
    std::atomic<bool> running{true};
    int wake[2] = {-1, -1};   // self-pipe: write -> wake/stop the loop
    std::thread th;

    // Returns true if a stop was requested (wake pipe readable) within
    // `ms`; false on timeout. Keeps grc_watch_destroy prompt even during
    // settle/backoff sleeps.
    bool waitStop(int ms) {
        struct pollfd p{wake[0], POLLIN, 0};
        int r = ::poll(&p, 1, ms);
        return r > 0 && (p.revents & POLLIN);
    }

    void loop() {
        // Arm/re-arm with bounded backoff; stay alive through a
        // transient missing HEAD (e.g. mid atomic-replace).
        while (running.load()) {
            std::string head = resolveHeadPath(clonePath);
            if (head.empty()) { if (waitStop(500)) return; continue; }
            if (!watchOne(head)) { if (waitStop(200)) return; continue; }
            // watchOne returns false to request a re-arm (atomic
            // replace) and true only when stopping.
            return;
        }
    }

    // Platform watch on `head`. Returns true when a stop was requested
    // (terminate the thread); false to request re-resolution + re-arm.
    bool watchOne(const std::string& head);

    // Debounced fire: after the first change, swallow further events for
    // ~kDebounceMs, then invoke the callback once. Returns true if a
    // stop was requested during the settle (caller should exit).
    bool fireDebounced() {
        if (waitStop(kDebounceMs)) return true;
        if (running.load() && cb) cb(ctx);
        return false;
    }
};

#if defined(__linux__)

bool GRCWatch::watchOne(const std::string& head) {
    int ifd = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ifd < 0) return false;  // re-arm later
    const std::string dir = parentDir(head);
    int wf = ::inotify_add_watch(
        ifd, head.c_str(),
        IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB);
    // Parent dir catches the HEAD.lock -> HEAD rename (which orphans the
    // file watch) and HEAD re-creation.
    int wd = ::inotify_add_watch(ifd, dir.c_str(),
                                 IN_MOVED_TO | IN_CREATE | IN_DELETE);
    if (wf < 0 && wd < 0) { ::close(ifd); return false; }

    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (running.load()) {
        struct pollfd p[2] = {{ifd, POLLIN, 0}, {wake[0], POLLIN, 0}};
        int r = ::poll(p, 2, -1);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (p[1].revents & POLLIN) { ::close(ifd); return true; }  // stop
        if (!(p[0].revents & POLLIN)) continue;

        bool changed = false, reArm = false;
        ssize_t n;
        while ((n = ::read(ifd, buf, sizeof(buf))) > 0) {
            for (char* q = buf; q < buf + n;) {
                auto* e = reinterpret_cast<struct inotify_event*>(q);
                if (e->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) reArm = true;
                if ((e->mask & (IN_MOVED_TO | IN_CREATE)) && e->len > 0 &&
                    std::string(e->name) == "HEAD") { reArm = true; changed = true; }
                if (e->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB))
                    changed = true;
                q += sizeof(struct inotify_event) + e->len;
            }
        }
        if (reArm) {
            ::close(ifd);
            if (waitStop(kRenameSettleMs)) return true;
            // Treat the swap as a change, then re-resolve+re-arm.
            if (running.load() && cb) cb(ctx);
            return false;
        }
        if (changed) {
            ::close(ifd);
            return fireDebounced();  // true=stop; false=re-arm fresh
        }
    }
    ::close(ifd);
    return true;
}

#elif defined(__APPLE__)

bool GRCWatch::watchOne(const std::string& head) {
    int kq = ::kqueue();
    if (kq < 0) return false;
    int hfd = ::open(head.c_str(), O_EVTONLY);
    if (hfd < 0) { ::close(kq); return false; }

    struct kevent ev[2];
    EV_SET(&ev[0], hfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND | NOTE_ATTRIB,
           0, nullptr);
    EV_SET(&ev[1], wake[0], EVFILT_READ, EV_ADD, 0, 0, nullptr);
    if (::kevent(kq, ev, 2, nullptr, 0, nullptr) < 0) {
        ::close(hfd); ::close(kq); return false;
    }

    while (running.load()) {
        struct kevent out[4];
        int r = ::kevent(kq, nullptr, 0, out, 4, nullptr);
        if (r < 0) { if (errno == EINTR) continue; break; }
        bool changed = false, reArm = false;
        for (int i = 0; i < r; ++i) {
            if (out[i].filter == EVFILT_READ) {  // wake -> stop
                ::close(hfd); ::close(kq); return true;
            }
            if (out[i].filter == EVFILT_VNODE) {
                if (out[i].fflags & (NOTE_DELETE | NOTE_RENAME)) reArm = true;
                if (out[i].fflags & (NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB))
                    changed = true;
            }
        }
        if (reArm) {
            ::close(hfd); ::close(kq);
            if (waitStop(kRenameSettleMs)) return true;
            if (running.load() && cb) cb(ctx);
            return false;  // re-resolve + re-arm
        }
        if (changed) {
            ::close(hfd); ::close(kq);
            return fireDebounced();  // true=stop; false=re-arm fresh
        }
    }
    ::close(hfd); ::close(kq);
    return true;
}

#else  // unsupported platform: inert watch (callback never fires)

bool GRCWatch::watchOne(const std::string&) {
    while (running.load()) { if (waitStop(1000)) return true; }
    return true;
}

#endif

extern "C" {

GRCWatch* grc_watch_head_create(const char* clone_path,
                                GRCHeadChangeCallback callback,
                                void* context) {
    if (clone_path == nullptr || callback == nullptr) return nullptr;
    // Must resolve now, or there is nothing to watch.
    if (resolveHeadPath(clone_path).empty()) return nullptr;
    auto* w = new GRCWatch();
    w->clonePath = clone_path;
    w->cb = callback;
    w->ctx = context;
    if (::pipe(w->wake) != 0) { delete w; return nullptr; }
    ::fcntl(w->wake[0], F_SETFL, O_NONBLOCK);
    w->th = std::thread([w] { w->loop(); });
    return w;
}

void grc_watch_destroy(GRCWatch* watch) {
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
}

}  // extern "C"
