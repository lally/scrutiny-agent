// GitManipBridge.h
// C bridge for git-manipulation C++ library

#ifndef GIT_MANIP_BRIDGE_H
#define GIT_MANIP_BRIDGE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct GMRepository GMRepository;
typedef struct GMRemoteInfo GMRemoteInfo;
typedef struct GMBranchInfo GMBranchInfo;
typedef struct GMCommitInfo GMCommitInfo;
typedef struct GMFileDiff GMFileDiff;
typedef struct GMDiffHunk GMDiffHunk;
typedef struct GMDiffLine GMDiffLine;

// Repository operations
GMRepository* gm_repository_open(const char* path);
void gm_repository_free(GMRepository* repo);
const char* gm_repository_path(GMRepository* repo);
bool gm_repository_is_bare(GMRepository* repo);
const char* gm_repository_head_oid(GMRepository* repo);
const char* gm_repository_current_branch(GMRepository* repo);

// Remote operations
size_t gm_repository_remote_count(GMRepository* repo);
GMRemoteInfo* gm_repository_get_remotes(GMRepository* repo, size_t* count);
void gm_remote_info_free(GMRemoteInfo* infos, size_t count);
GMRemoteInfo* gm_remote_info_at_index(GMRemoteInfo* infos, size_t index);
const char* gm_remote_info_get_name(GMRemoteInfo* info);
const char* gm_remote_info_get_url(GMRemoteInfo* info);
const char* gm_remote_info_get_push_url(GMRemoteInfo* info);

// Branch operations
size_t gm_repository_branch_count(GMRepository* repo, bool local, bool remote);
GMBranchInfo* gm_repository_get_branches(GMRepository* repo, bool local, bool remote, size_t* count);
void gm_branch_info_free(GMBranchInfo* infos, size_t count);
GMBranchInfo* gm_branch_info_at_index(GMBranchInfo* infos, size_t index);
const char* gm_branch_info_get_name(GMBranchInfo* info);
const char* gm_branch_info_get_refname(GMBranchInfo* info);
const char* gm_branch_info_get_target_oid(GMBranchInfo* info);
bool gm_branch_info_is_remote(GMBranchInfo* info);
bool gm_branch_info_is_head(GMBranchInfo* info);
const char* gm_branch_info_get_upstream(GMBranchInfo* info);
const char* gm_branch_info_get_remote_name(GMBranchInfo* info);

// Commit operations
GMCommitInfo* gm_repository_get_commits(GMRepository* repo, const char* branch, size_t limit, size_t* count);
void gm_commit_info_free(GMCommitInfo* infos, size_t count);
GMCommitInfo* gm_commit_info_at_index(GMCommitInfo* infos, size_t index);
const char* gm_commit_info_get_oid(GMCommitInfo* info);
const char* gm_commit_info_get_short_oid(GMCommitInfo* info);
const char* gm_commit_info_get_message(GMCommitInfo* info);
const char* gm_commit_info_get_summary(GMCommitInfo* info);
const char* gm_commit_info_get_author_name(GMCommitInfo* info);
const char* gm_commit_info_get_author_email(GMCommitInfo* info);
int64_t gm_commit_info_get_author_time(GMCommitInfo* info);
size_t gm_commit_info_get_parent_count(GMCommitInfo* info);
const char* gm_commit_info_get_parent_oid(GMCommitInfo* info, size_t index);

// Status operations
bool gm_repository_has_uncommitted_changes(GMRepository* repo);

// Checkout an existing local branch (refs/heads/<branch>). Returns
// true on success. On failure (branch doesn't exist, working tree
// would be clobbered, etc.) returns false and the reason is in the
// thread-local last-error slot (`gm_get_last_error`). Uses libgit2's
// SAFE checkout strategy, so a dirty working tree that conflicts
// with the target produces a failure rather than silent data loss.
bool gm_repository_checkout_branch(GMRepository* repo, const char* branch);
size_t gm_repository_get_ahead_behind(GMRepository* repo, const char* branch, size_t* ahead, size_t* behind);

// Diff operations
GMFileDiff* gm_commit_get_diff(GMRepository* repo, const char* commit_oid, size_t* count);

// Working-tree diff: index -> workdir, including untracked files
// (recursed). Returns the same GMFileDiff shape as gm_commit_get_diff,
// so callers can reuse the same accessor functions to walk deltas /
// hunks / lines. Pair with gm_repository_has_uncommitted_changes(): the
// boolean was the existing check, this enumerates the actual files.
GMFileDiff* gm_repository_get_workdir_diff(GMRepository* repo, size_t* count);

// Staged diff: HEAD-tree -> index. Returns the same GMFileDiff shape.
// Falls back to empty-tree -> index when the repo has no HEAD yet
// (initial-commit case). Pairs with gm_repository_get_workdir_diff so
// callers can show staged-vs-unstaged separately while covering all
// "not yet committed" changes.
GMFileDiff* gm_repository_get_staged_diff(GMRepository* repo, size_t* count);
void gm_file_diff_free(GMFileDiff* diffs, size_t count);
GMFileDiff* gm_file_diff_at_index(GMFileDiff* diffs, size_t index);
int gm_file_diff_get_status(GMFileDiff* diff);
const char* gm_file_diff_get_old_path(GMFileDiff* diff);
const char* gm_file_diff_get_new_path(GMFileDiff* diff);
const char* gm_file_diff_get_patch(GMFileDiff* diff);
size_t gm_file_diff_get_hunk_count(GMFileDiff* diff);
GMDiffHunk* gm_file_diff_get_hunk(GMFileDiff* diff, size_t index);

// Diff hunk operations
int gm_diff_hunk_get_old_start(GMDiffHunk* hunk);
int gm_diff_hunk_get_old_lines(GMDiffHunk* hunk);
int gm_diff_hunk_get_new_start(GMDiffHunk* hunk);
int gm_diff_hunk_get_new_lines(GMDiffHunk* hunk);
const char* gm_diff_hunk_get_header(GMDiffHunk* hunk);
size_t gm_diff_hunk_get_line_count(GMDiffHunk* hunk);
GMDiffLine* gm_diff_hunk_get_line(GMDiffHunk* hunk, size_t index);

// Diff line operations
char gm_diff_line_get_origin(GMDiffLine* line);
const char* gm_diff_line_get_content(GMDiffLine* line);
int gm_diff_line_get_old_lineno(GMDiffLine* line);
int gm_diff_line_get_new_lineno(GMDiffLine* line);

// Error handling
const char* gm_get_last_error();
void gm_clear_error();

// Thread-safety / one-time initialization.
//
// gm_global_init() performs libgit2's one-time initialization on the
// CALLING thread. Call it exactly once at process startup, before any
// other thread uses gm_*: it deterministically constructs
// git-manipulation's function-local `static LibGit2Init` (a C++
// "magic static") on the startup thread so worker threads never run
// that one-time initializer concurrently. Cheap and safe to call again.
//
// gm_libgit2_threadsafe() reports whether the linked libgit2 was built
// with thread support (GIT_FEATURE_THREADS). When true (and after
// gm_global_init), distinct GMRepository* handles may be used
// concurrently on different threads -- each handle still used by only
// one thread at a time. When false, callers must serialize all gm_*.
void gm_global_init(void);
bool gm_libgit2_threadsafe(void);

#ifdef __cplusplus
}
#endif

#endif // GIT_MANIP_BRIDGE_H