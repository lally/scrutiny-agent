// GitManipBridge.cpp
// C bridge implementation for git-manipulation library

#include "GitManipBridge.h"
#include <gitmanip/gitmanip.hpp>
#include <git2.h>
#include <memory>
#include <vector>
#include <string>
#include <cstring>
#include <thread>
#include <mutex>

// Thread-local error storage
thread_local std::string g_last_error;
std::mutex g_error_mutex;

// Wrapper structs
struct GMRepository {
    std::unique_ptr<gitmanip::Repository> repo;
    std::string path_cache;
    std::string workdir_cache;
    std::string head_oid_cache;
    std::string current_branch_cache;
};

struct GMRemoteInfo {
    std::string name;
    std::string url;
    std::string push_url;
};

struct GMBranchInfo {
    std::string name;
    std::string refname;
    std::string target_oid;
    bool is_remote;
    bool is_head;
    std::string upstream;
    std::string remote_name;
};

struct GMCommitInfo {
    std::string oid;
    std::string short_oid;
    std::string message;
    std::string summary;
    std::string author_name;
    std::string author_email;
    int64_t author_time;
    std::vector<std::string> parent_oids;
};

struct GMDiffLine {
    char origin;
    std::string content;
    int old_lineno;
    int new_lineno;
};

struct GMDiffHunk {
    int old_start;
    int old_lines;
    int new_start;
    int new_lines;
    std::string header;
    std::vector<GMDiffLine> lines;
};

struct GMFileDiff {
    int status;  // Maps to DiffDelta::Status
    std::string old_path;
    std::string new_path;
    std::string patch;
    std::vector<GMDiffHunk> hunks;
};

// Helper function to set error
static void set_error(const std::string& error) {
    std::lock_guard<std::mutex> lock(g_error_mutex);
    g_last_error = error;
}

// Repository operations
extern "C" {

GMRepository* gm_repository_open(const char* path) {
    try {
        auto gm_repo = new GMRepository();
        gm_repo->repo = std::make_unique<gitmanip::Repository>(
            gitmanip::Repository::open(path)
        );
        gm_repo->path_cache = gm_repo->repo->path().string();
        return gm_repo;
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

void gm_repository_free(GMRepository* repo) {
    delete repo;
}

const char* gm_repository_path(GMRepository* repo) {
    if (!repo) return nullptr;
    return repo->path_cache.c_str();
}

// The work tree, i.e. what a user calls "the repository". Distinct
// from gm_repository_path(), which is libgit2's gitdir ("<repo>/.git/").
// nullptr for a bare repository, which has no work tree.
const char* gm_repository_workdir(GMRepository* repo) {
    if (!repo) return nullptr;
    try {
        const auto workdir = repo->repo->workdir();
        if (workdir.empty()) return nullptr;
        repo->workdir_cache = workdir.string();
        return repo->workdir_cache.c_str();
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

bool gm_repository_is_bare(GMRepository* repo) {
    if (!repo) return false;
    try {
        return repo->repo->is_bare();
    } catch (const std::exception& e) {
        set_error(e.what());
        return false;
    }
}

const char* gm_repository_head_oid(GMRepository* repo) {
    if (!repo) return nullptr;
    try {
        repo->head_oid_cache = repo->repo->head_oid().to_string();
        return repo->head_oid_cache.c_str();
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

const char* gm_repository_current_branch(GMRepository* repo) {
    if (!repo) return nullptr;
    try {
        auto branch = gitmanip::Branch::current(*repo->repo);
        if (branch.has_value()) {
            repo->current_branch_cache = branch->name;
            return repo->current_branch_cache.c_str();
        }
        return nullptr;
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

// Remote operations
size_t gm_repository_remote_count(GMRepository* repo) {
    if (!repo) return 0;
    try {
        return gitmanip::Remote::list(*repo->repo).size();
    } catch (const std::exception& e) {
        set_error(e.what());
        return 0;
    }
}

GMRemoteInfo* gm_repository_get_remotes(GMRepository* repo, size_t* count) {
    if (!repo || !count) {
        if (count) *count = 0;
        return nullptr;
    }

    try {
        auto remotes = gitmanip::Remote::list(*repo->repo);
        *count = remotes.size();

        if (remotes.empty()) {
            return nullptr;
        }

        auto result = new GMRemoteInfo[remotes.size()];
        for (size_t i = 0; i < remotes.size(); ++i) {
            result[i].name = remotes[i].name;
            result[i].url = remotes[i].url;
            result[i].push_url = remotes[i].push_url;
        }

        return result;
    } catch (const std::exception& e) {
        set_error(e.what());
        *count = 0;
        return nullptr;
    }
}

void gm_remote_info_free(GMRemoteInfo* infos, size_t count) {
    delete[] infos;
}

GMRemoteInfo* gm_remote_info_at_index(GMRemoteInfo* infos, size_t index) {
    return &infos[index];
}

const char* gm_remote_info_get_name(GMRemoteInfo* info) {
    return info ? info->name.c_str() : nullptr;
}

const char* gm_remote_info_get_url(GMRemoteInfo* info) {
    return info ? info->url.c_str() : nullptr;
}

const char* gm_remote_info_get_push_url(GMRemoteInfo* info) {
    return info ? info->push_url.c_str() : nullptr;
}

// Branch operations
size_t gm_repository_branch_count(GMRepository* repo, bool local, bool remote) {
    if (!repo) return 0;
    try {
        return gitmanip::Branch::list(*repo->repo, local, remote).size();
    } catch (const std::exception& e) {
        set_error(e.what());
        return 0;
    }
}

GMBranchInfo* gm_repository_get_branches(GMRepository* repo, bool local, bool remote, size_t* count) {
    if (!repo || !count) {
        if (count) *count = 0;
        return nullptr;
    }

    try {
        auto branches = gitmanip::Branch::list(*repo->repo, local, remote);
        *count = branches.size();

        if (branches.empty()) {
            return nullptr;
        }

        auto result = new GMBranchInfo[branches.size()];
        for (size_t i = 0; i < branches.size(); ++i) {
            result[i].name = branches[i].name;
            result[i].refname = branches[i].refname;
            result[i].target_oid = branches[i].target.to_string();
            result[i].is_remote = branches[i].is_remote;
            result[i].is_head = branches[i].is_head;
            result[i].upstream = branches[i].upstream.value_or("");
            result[i].remote_name = branches[i].remote_name.value_or("");
        }

        return result;
    } catch (const std::exception& e) {
        set_error(e.what());
        *count = 0;
        return nullptr;
    }
}

void gm_branch_info_free(GMBranchInfo* infos, size_t count) {
    delete[] infos;
}

GMBranchInfo* gm_branch_info_at_index(GMBranchInfo* infos, size_t index) {
    return &infos[index];
}

const char* gm_branch_info_get_name(GMBranchInfo* info) {
    return info ? info->name.c_str() : nullptr;
}

const char* gm_branch_info_get_refname(GMBranchInfo* info) {
    return info ? info->refname.c_str() : nullptr;
}

const char* gm_branch_info_get_target_oid(GMBranchInfo* info) {
    return info ? info->target_oid.c_str() : nullptr;
}

bool gm_branch_info_is_remote(GMBranchInfo* info) {
    return info ? info->is_remote : false;
}

bool gm_branch_info_is_head(GMBranchInfo* info) {
    return info ? info->is_head : false;
}

const char* gm_branch_info_get_upstream(GMBranchInfo* info) {
    return (info && !info->upstream.empty()) ? info->upstream.c_str() : nullptr;
}

const char* gm_branch_info_get_remote_name(GMBranchInfo* info) {
    return (info && !info->remote_name.empty()) ? info->remote_name.c_str() : nullptr;
}

// Commit operations
GMCommitInfo* gm_repository_get_commits(GMRepository* repo, const char* branch, size_t limit, size_t* count) {
    if (!repo || !count) {
        if (count) *count = 0;
        return nullptr;
    }

    try {
        std::vector<gitmanip::Commit> commits;

        // Walk commits from branch or HEAD
        auto walker = repo->repo->walk_commits();
        if (branch && strlen(branch) > 0) {
            // If branch name doesn't start with "refs/", qualify it as a local branch
            std::string ref = branch;
            if (ref.find("refs/") != 0) {
                ref = "refs/heads/" + ref;
            }
            walker.push_ref(ref.c_str());
        } else {
            walker.push_head();
        }

        size_t collected = 0;
        for (auto&& commit : walker.walk()) {
            if (collected >= limit) break;
            commits.push_back(std::move(commit));
            collected++;
        }

        *count = commits.size();
        if (commits.empty()) {
            return nullptr;
        }

        auto result = new GMCommitInfo[commits.size()];
        for (size_t i = 0; i < commits.size(); ++i) {
            const auto& commit = commits[i];
            result[i].oid = commit.id().to_string();
            result[i].short_oid = commit.id().short_id();
            result[i].message = commit.message();
            result[i].summary = commit.summary();
            result[i].author_name = commit.author().name;
            result[i].author_email = commit.author().email;
            // Convert chrono time_point to Unix timestamp
            auto duration = commit.author().time.time_since_epoch();
            result[i].author_time = std::chrono::duration_cast<std::chrono::seconds>(duration).count();

            // Collect parent OIDs
            for (size_t j = 0; j < commit.parent_count(); ++j) {
                result[i].parent_oids.push_back(commit.parent_id(j).to_string());
            }
        }

        return result;
    } catch (const std::exception& e) {
        set_error(e.what());
        *count = 0;
        return nullptr;
    }
}

void gm_commit_info_free(GMCommitInfo* infos, size_t count) {
    delete[] infos;
}

GMCommitInfo* gm_commit_info_at_index(GMCommitInfo* infos, size_t index) {
    return &infos[index];
}

const char* gm_commit_info_get_oid(GMCommitInfo* info) {
    return info ? info->oid.c_str() : nullptr;
}

const char* gm_commit_info_get_short_oid(GMCommitInfo* info) {
    return info ? info->short_oid.c_str() : nullptr;
}

const char* gm_commit_info_get_message(GMCommitInfo* info) {
    return info ? info->message.c_str() : nullptr;
}

const char* gm_commit_info_get_summary(GMCommitInfo* info) {
    return info ? info->summary.c_str() : nullptr;
}

const char* gm_commit_info_get_author_name(GMCommitInfo* info) {
    return info ? info->author_name.c_str() : nullptr;
}

const char* gm_commit_info_get_author_email(GMCommitInfo* info) {
    return info ? info->author_email.c_str() : nullptr;
}

int64_t gm_commit_info_get_author_time(GMCommitInfo* info) {
    return info ? info->author_time : 0;
}

size_t gm_commit_info_get_parent_count(GMCommitInfo* info) {
    return info ? info->parent_oids.size() : 0;
}

const char* gm_commit_info_get_parent_oid(GMCommitInfo* info, size_t index) {
    if (!info || index >= info->parent_oids.size()) {
        return nullptr;
    }
    return info->parent_oids[index].c_str();
}

// Status operations
bool gm_repository_has_uncommitted_changes(GMRepository* repo) {
    if (!repo) return false;
    try {
        // "Uncommitted" = anything in HEAD that doesn't match the
        // working tree. That's two separate libgit2 diffs: HEAD-tree
        // -> index (staged) and index -> workdir (unstaged). Either
        // having deltas means the tree is dirty. The first leg is
        // skipped on a HEAD-less repo (initial commit) to avoid
        // throwing.
        if (repo->repo->diff_index_to_workdir().num_deltas() > 0) {
            return true;
        }
        try {
            auto head_oid = repo->repo->head_oid();
            auto head_tree = repo->repo->lookup_commit(head_oid).tree();
            if (repo->repo->diff_tree_to_index(head_tree).num_deltas() > 0) {
                return true;
            }
        } catch (...) {
            // No HEAD yet -- only the workdir check matters.
        }
        return false;
    } catch (const std::exception& e) {
        set_error(e.what());
        return false;
    }
}

bool gm_repository_checkout_branch(GMRepository* repo, const char* branch) {
    if (!repo || !branch) {
        set_error("Invalid arguments");
        return false;
    }
    try {
        std::string refname = "refs/heads/";
        refname += branch;
        // Set HEAD to the branch ref, then materialize the working
        // tree. gitmanip's checkout_head defaults to a SAFE strategy
        // (force = false), which refuses to overwrite uncommitted
        // changes. The caller surfaces that as an error rather than
        // letting libgit2 silently destroy work.
        repo->repo->set_head(refname);
        repo->repo->checkout_head(/*force=*/false);
        return true;
    } catch (const std::exception& e) {
        set_error(e.what());
        return false;
    }
}

size_t gm_repository_get_ahead_behind(GMRepository* repo, const char* branch, size_t* ahead, size_t* behind) {
    if (!repo || !branch || !ahead || !behind) {
        if (ahead) *ahead = 0;
        if (behind) *behind = 0;
        return 0;
    }

    try {
        // Get the branch and its upstream
        auto branch_info = gitmanip::Branch::get(*repo->repo, branch, false);
        if (!branch_info.has_value()) {
            *ahead = 0;
            *behind = 0;
            return 0;
        }

        if (!branch_info->upstream.has_value()) {
            *ahead = 0;
            *behind = 0;
            return 0;
        }

        // Count commits ahead and behind
        auto local_commit = repo->repo->lookup_commit(branch_info->target);
        auto upstream_ref = "refs/remotes/" + branch_info->upstream.value();
        auto upstream_commit = repo->repo->lookup_commit(upstream_ref);

        // Count commits from local to upstream (behind)
        *behind = 0;
        for ([[maybe_unused]] const auto& commit : repo->repo->walk_commits()
            .push(upstream_commit.id())
            .hide(local_commit.id())
            .walk()) {
            (*behind)++;
        }

        // Count commits from upstream to local (ahead)
        *ahead = 0;
        for ([[maybe_unused]] const auto& commit : repo->repo->walk_commits()
            .push(local_commit.id())
            .hide(upstream_commit.id())
            .walk()) {
            (*ahead)++;
        }

        return *ahead + *behind;
    } catch (const std::exception& e) {
        set_error(e.what());
        *ahead = 0;
        *behind = 0;
        return 0;
    }
}

// Diff operations
GMFileDiff* gm_commit_get_diff(GMRepository* repo, const char* commit_oid, size_t* count) {
    if (!repo || !commit_oid || !count) {
        set_error("Invalid arguments");
        *count = 0;
        return nullptr;
    }

    try {
        // Lookup commit
        auto oid = gitmanip::Oid(commit_oid);
        auto commit = repo->repo->lookup_commit(oid);

        // Get trees
        auto commit_tree = commit.tree();
        gitmanip::Tree parent_tree = commit.parent_count() > 0
            ? repo->repo->lookup_commit(commit.parent_id(0)).tree()
            : repo->repo->empty_tree();

        // Create diff
        auto diff = repo->repo->diff_tree_to_tree(parent_tree, commit_tree);

        // Extract deltas
        auto deltas = diff.deltas();
        auto* result = new GMFileDiff[deltas.size()];

        for (size_t i = 0; i < deltas.size(); i++) {
            const auto& delta = deltas[i];

            // Map status
            result[i].status = static_cast<int>(delta.status);
            result[i].old_path = delta.old_path;
            result[i].new_path = delta.new_path;
            result[i].patch = diff.patch(i);

            // Extract hunks
            auto hunks = diff.hunks(i);
            for (size_t j = 0; j < hunks.size(); j++) {
                const auto& hunk = hunks[j];
                GMDiffHunk gm_hunk;
                gm_hunk.old_start = hunk.old_start;
                gm_hunk.old_lines = hunk.old_lines;
                gm_hunk.new_start = hunk.new_start;
                gm_hunk.new_lines = hunk.new_lines;
                gm_hunk.header = hunk.header;

                // Extract lines
                auto lines = diff.lines(i, j);
                for (const auto& line : lines) {
                    GMDiffLine gm_line;
                    gm_line.origin = static_cast<char>(line.origin);
                    gm_line.content = line.content;
                    gm_line.old_lineno = line.old_lineno;
                    gm_line.new_lineno = line.new_lineno;
                    gm_hunk.lines.push_back(std::move(gm_line));
                }

                result[i].hunks.push_back(std::move(gm_hunk));
            }
        }

        *count = deltas.size();
        return result;
    } catch (const std::exception& e) {
        set_error(e.what());
        *count = 0;
        return nullptr;
    }
}

// Extract deltas/hunks/lines from a gitmanip::Diff into a heap-allocated
// GMFileDiff[]. Returns nullptr and sets *count = 0 on empty/error.
// Shared by gm_commit_get_diff and gm_repository_get_workdir_diff so
// the wire shape is identical between commit-vs-parent and working-tree
// diffs.
static GMFileDiff* extract_file_diffs(const gitmanip::Diff& diff, size_t* count) {
    try {
        auto deltas = diff.deltas();
        if (deltas.empty()) {
            *count = 0;
            return nullptr;
        }
        auto* result = new GMFileDiff[deltas.size()];
        for (size_t i = 0; i < deltas.size(); i++) {
            const auto& delta = deltas[i];
            result[i].status = static_cast<int>(delta.status);
            result[i].old_path = delta.old_path;
            result[i].new_path = delta.new_path;
            result[i].patch = diff.patch(i);

            auto hunks = diff.hunks(i);
            for (size_t j = 0; j < hunks.size(); j++) {
                const auto& hunk = hunks[j];
                GMDiffHunk gm_hunk;
                gm_hunk.old_start = hunk.old_start;
                gm_hunk.old_lines = hunk.old_lines;
                gm_hunk.new_start = hunk.new_start;
                gm_hunk.new_lines = hunk.new_lines;
                gm_hunk.header = hunk.header;

                auto lines = diff.lines(i, j);
                for (const auto& line : lines) {
                    GMDiffLine gm_line;
                    gm_line.origin = static_cast<char>(line.origin);
                    gm_line.content = line.content;
                    gm_line.old_lineno = line.old_lineno;
                    gm_line.new_lineno = line.new_lineno;
                    gm_hunk.lines.push_back(std::move(gm_line));
                }

                result[i].hunks.push_back(std::move(gm_hunk));
            }
        }
        *count = deltas.size();
        return result;
    } catch (const std::exception& e) {
        set_error(e.what());
        *count = 0;
        return nullptr;
    }
}

GMFileDiff* gm_repository_get_workdir_diff(GMRepository* repo, size_t* count) {
    if (!repo || !count) {
        set_error("Invalid arguments");
        if (count) *count = 0;
        return nullptr;
    }
    try {
        gitmanip::DiffOptions opts;
        opts.include_untracked = true;
        opts.recurse_untracked_dirs = true;
        auto diff = repo->repo->diff_index_to_workdir(opts);
        return extract_file_diffs(diff, count);
    } catch (const std::exception& e) {
        set_error(e.what());
        *count = 0;
        return nullptr;
    }
}

GMFileDiff* gm_repository_get_staged_diff(GMRepository* repo, size_t* count) {
    if (!repo || !count) {
        set_error("Invalid arguments");
        if (count) *count = 0;
        return nullptr;
    }
    try {
        // HEAD's tree, falling back to empty_tree() when the repo has
        // no HEAD yet (fresh repo with only an index). The DiffOptions
        // defaults are fine here -- staged content is by definition
        // tracked, so include_untracked doesn't apply.
        gitmanip::Tree from_tree = [&] {
            try {
                auto head_oid = repo->repo->head_oid();
                return repo->repo->lookup_commit(head_oid).tree();
            } catch (...) {
                return repo->repo->empty_tree();
            }
        }();
        auto diff = repo->repo->diff_tree_to_index(from_tree);
        return extract_file_diffs(diff, count);
    } catch (const std::exception& e) {
        set_error(e.what());
        *count = 0;
        return nullptr;
    }
}

void gm_file_diff_free(GMFileDiff* diffs, size_t count) {
    delete[] diffs;
}

GMFileDiff* gm_file_diff_at_index(GMFileDiff* diffs, size_t index) {
    return &diffs[index];
}

int gm_file_diff_get_status(GMFileDiff* diff) {
    return diff ? diff->status : 0;
}

const char* gm_file_diff_get_old_path(GMFileDiff* diff) {
    return diff ? diff->old_path.c_str() : nullptr;
}

const char* gm_file_diff_get_new_path(GMFileDiff* diff) {
    return diff ? diff->new_path.c_str() : nullptr;
}

const char* gm_file_diff_get_patch(GMFileDiff* diff) {
    return diff ? diff->patch.c_str() : nullptr;
}

size_t gm_file_diff_get_hunk_count(GMFileDiff* diff) {
    return diff ? diff->hunks.size() : 0;
}

GMDiffHunk* gm_file_diff_get_hunk(GMFileDiff* diff, size_t index) {
    if (!diff || index >= diff->hunks.size()) return nullptr;
    return &diff->hunks[index];
}

// Diff hunk operations
int gm_diff_hunk_get_old_start(GMDiffHunk* hunk) {
    return hunk ? hunk->old_start : 0;
}

int gm_diff_hunk_get_old_lines(GMDiffHunk* hunk) {
    return hunk ? hunk->old_lines : 0;
}

int gm_diff_hunk_get_new_start(GMDiffHunk* hunk) {
    return hunk ? hunk->new_start : 0;
}

int gm_diff_hunk_get_new_lines(GMDiffHunk* hunk) {
    return hunk ? hunk->new_lines : 0;
}

const char* gm_diff_hunk_get_header(GMDiffHunk* hunk) {
    return hunk ? hunk->header.c_str() : nullptr;
}

size_t gm_diff_hunk_get_line_count(GMDiffHunk* hunk) {
    return hunk ? hunk->lines.size() : 0;
}

GMDiffLine* gm_diff_hunk_get_line(GMDiffHunk* hunk, size_t index) {
    if (!hunk || index >= hunk->lines.size()) return nullptr;
    return &hunk->lines[index];
}

// Diff line operations
char gm_diff_line_get_origin(GMDiffLine* line) {
    return line ? line->origin : ' ';
}

const char* gm_diff_line_get_content(GMDiffLine* line) {
    return line ? line->content.c_str() : nullptr;
}

int gm_diff_line_get_old_lineno(GMDiffLine* line) {
    return line ? line->old_lineno : -1;
}

int gm_diff_line_get_new_lineno(GMDiffLine* line) {
    return line ? line->new_lineno : -1;
}

// Error handling
const char* gm_get_last_error() {
    return g_last_error.c_str();
}

void gm_clear_error() {
    std::lock_guard<std::mutex> lock(g_error_mutex);
    g_last_error.clear();
}

// Thread-safety / one-time init (see GitManipBridge.h).
void gm_global_init(void) {
    // Force gitmanip::Repository::open's function-local
    // `static LibGit2Init init;` to be constructed on THIS (startup)
    // thread. "/" is never a git repository, so git_repository_open
    // fails fast -- but the magic-static is constructed before that
    // failure, so libgit2's one-time init is done here, single-threaded,
    // and no worker thread can ever race it.
    try {
        gitmanip::Repository::open("/");
    } catch (...) {
        // Expected: "/" is not a repo. The static is now constructed.
    }
    // Don't leave the startup probe's error in this thread's slot.
    {
        std::lock_guard<std::mutex> lock(g_error_mutex);
        g_last_error.clear();
    }
}

bool gm_libgit2_threadsafe(void) {
    return (git_libgit2_features() & GIT_FEATURE_THREADS) != 0;
}

} // extern "C"