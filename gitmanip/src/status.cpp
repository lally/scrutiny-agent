#include "gitmanip/status.hpp"
#include "gitmanip/error.hpp"
#include "gitmanip/refs.hpp"
#include "gitmanip/repository.hpp"

#include <git2.h>
#include <fmt/format.h>

namespace gitmanip {

namespace {

FileState convert_index_status(unsigned int flags) {
    if (flags & GIT_STATUS_INDEX_NEW) return FileState::New;
    if (flags & GIT_STATUS_INDEX_MODIFIED) return FileState::Modified;
    if (flags & GIT_STATUS_INDEX_DELETED) return FileState::Deleted;
    if (flags & GIT_STATUS_INDEX_RENAMED) return FileState::Renamed;
    if (flags & GIT_STATUS_INDEX_TYPECHANGE) return FileState::TypeChange;
    return FileState::Modified;  // fallback
}

FileState convert_workdir_status(unsigned int flags) {
    if (flags & GIT_STATUS_WT_NEW) return FileState::New;
    if (flags & GIT_STATUS_WT_MODIFIED) return FileState::Modified;
    if (flags & GIT_STATUS_WT_DELETED) return FileState::Deleted;
    if (flags & GIT_STATUS_WT_RENAMED) return FileState::Renamed;
    if (flags & GIT_STATUS_WT_TYPECHANGE) return FileState::TypeChange;
    if (flags & GIT_STATUS_IGNORED) return FileState::Ignored;
    if (flags & GIT_STATUS_CONFLICTED) return FileState::Conflicted;
    return FileState::Modified;  // fallback
}

bool has_index_status(unsigned int flags) {
    return (flags & (GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED |
                     GIT_STATUS_INDEX_DELETED | GIT_STATUS_INDEX_RENAMED |
                     GIT_STATUS_INDEX_TYPECHANGE)) != 0;
}

bool has_workdir_status(unsigned int flags) {
    return (flags & (GIT_STATUS_WT_NEW | GIT_STATUS_WT_MODIFIED |
                     GIT_STATUS_WT_DELETED | GIT_STATUS_WT_RENAMED |
                     GIT_STATUS_WT_TYPECHANGE | GIT_STATUS_CONFLICTED)) != 0;
}

}  // namespace

std::vector<StatusEntry> Status::list(Repository& repo, const StatusOptions& options) {
    std::vector<StatusEntry> result;

    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    opts.flags = GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
                 GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;

    if (options.include_untracked) {
        opts.flags |= GIT_STATUS_OPT_INCLUDE_UNTRACKED;
    }
    if (options.recurse_untracked_dirs) {
        opts.flags |= GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
    }
    if (options.include_ignored) {
        opts.flags |= GIT_STATUS_OPT_INCLUDE_IGNORED;
    }
    if (options.detect_renames) {
        opts.flags |= GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
                      GIT_STATUS_OPT_RENAMES_INDEX_TO_WORKDIR;
    }

    opts.rename_threshold = options.rename_threshold;

    // Set up pathspec if provided
    std::vector<char*> pathspec_strings;
    if (!options.pathspec.empty()) {
        pathspec_strings.reserve(options.pathspec.size());
        for (const auto& path : options.pathspec) {
            pathspec_strings.push_back(const_cast<char*>(path.c_str()));
        }
        opts.pathspec.strings = pathspec_strings.data();
        opts.pathspec.count = pathspec_strings.size();
    }

    git_status_list* status_list = nullptr;
    int error = git_status_list_new(&status_list, repo.raw(), &opts);
    detail::check_libgit2_error(error, "getting status list");

    size_t count = git_status_list_entrycount(status_list);

    for (size_t i = 0; i < count; ++i) {
        const git_status_entry* entry = git_status_byindex(status_list, i);
        if (!entry) continue;

        StatusEntry status_entry;

        // Determine path
        if (entry->head_to_index && entry->head_to_index->new_file.path) {
            status_entry.path = entry->head_to_index->new_file.path;
            if (entry->head_to_index->old_file.path &&
                std::string(entry->head_to_index->old_file.path) != status_entry.path) {
                status_entry.old_path = entry->head_to_index->old_file.path;
            }
            status_entry.similarity = entry->head_to_index->similarity;
        } else if (entry->index_to_workdir && entry->index_to_workdir->new_file.path) {
            status_entry.path = entry->index_to_workdir->new_file.path;
        }

        if (status_entry.path.empty()) continue;

        // Index status
        if (has_index_status(entry->status)) {
            status_entry.index_status = convert_index_status(entry->status);
        }

        // Working directory status
        if (has_workdir_status(entry->status)) {
            status_entry.workdir_status = convert_workdir_status(entry->status);
        }

        // Handle ignored files specially
        if (entry->status & GIT_STATUS_IGNORED) {
            status_entry.workdir_status = FileState::Ignored;
        }

        result.push_back(std::move(status_entry));
    }

    git_status_list_free(status_list);
    return result;
}

std::optional<SyncStatus> Status::sync(Repository& repo) {
    // Get current branch
    auto current = Branch::current(repo);
    if (!current) {
        return std::nullopt;  // HEAD is detached
    }

    return sync(repo, current->name);
}

std::optional<SyncStatus> Status::sync(Repository& repo, std::string_view branch_name) {
    // Get the upstream branch
    auto upstream_info = Branch::upstream(repo, branch_name);
    if (!upstream_info) {
        return std::nullopt;  // No upstream configured
    }

    // Get local branch info
    auto local_info = Branch::get(repo, branch_name, false);
    if (!local_info) {
        return std::nullopt;
    }

    SyncStatus result;
    result.upstream = upstream_info->name;

    // Use git_graph_ahead_behind to get the counts first
    size_t ahead_count = 0;
    size_t behind_count = 0;

    int error = git_graph_ahead_behind(&ahead_count, &behind_count,
                                        repo.raw(),
                                        local_info->target.raw(),
                                        upstream_info->target.raw());
    detail::check_libgit2_error(error, "computing ahead/behind");

    // If there are commits ahead, walk them
    if (ahead_count > 0) {
        git_revwalk* walker = nullptr;
        error = git_revwalk_new(&walker, repo.raw());
        detail::check_libgit2_error(error, "creating revision walker");

        // Push local, hide upstream
        git_revwalk_push(walker, local_info->target.raw());
        git_revwalk_hide(walker, upstream_info->target.raw());
        git_revwalk_sorting(walker, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);

        git_oid oid;
        while (git_revwalk_next(&oid, walker) == 0) {
            result.ahead.push_back(Oid(&oid));
        }

        git_revwalk_free(walker);
    }

    // If there are commits behind, walk them
    if (behind_count > 0) {
        git_revwalk* walker = nullptr;
        error = git_revwalk_new(&walker, repo.raw());
        detail::check_libgit2_error(error, "creating revision walker");

        // Push upstream, hide local
        git_revwalk_push(walker, upstream_info->target.raw());
        git_revwalk_hide(walker, local_info->target.raw());
        git_revwalk_sorting(walker, GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);

        git_oid oid;
        while (git_revwalk_next(&oid, walker) == 0) {
            result.behind.push_back(Oid(&oid));
        }

        git_revwalk_free(walker);
    }

    return result;
}

bool Status::has_changes(Repository& repo) {
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    opts.flags = 0;  // Only tracked files

    git_status_list* status = nullptr;
    int error = git_status_list_new(&status, repo.raw(), &opts);
    detail::check_libgit2_error(error, "getting status list");

    size_t count = git_status_list_entrycount(status);
    git_status_list_free(status);

    return count > 0;
}

bool Status::has_staged(Repository& repo) {
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.show = GIT_STATUS_SHOW_INDEX_ONLY;

    git_status_list* status = nullptr;
    int error = git_status_list_new(&status, repo.raw(), &opts);
    detail::check_libgit2_error(error, "getting status list");

    size_t count = git_status_list_entrycount(status);
    git_status_list_free(status);

    return count > 0;
}

bool Status::has_unstaged(Repository& repo) {
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.show = GIT_STATUS_SHOW_WORKDIR_ONLY;
    opts.flags = 0;  // Only tracked files

    git_status_list* status = nullptr;
    int error = git_status_list_new(&status, repo.raw(), &opts);
    detail::check_libgit2_error(error, "getting status list");

    // Check for actual modifications (not untracked)
    size_t count = git_status_list_entrycount(status);
    bool has_unstaged_changes = false;

    for (size_t i = 0; i < count && !has_unstaged_changes; ++i) {
        const git_status_entry* entry = git_status_byindex(status, i);
        if (entry && (entry->status & (GIT_STATUS_WT_MODIFIED |
                                       GIT_STATUS_WT_DELETED |
                                       GIT_STATUS_WT_RENAMED |
                                       GIT_STATUS_WT_TYPECHANGE))) {
            has_unstaged_changes = true;
        }
    }

    git_status_list_free(status);
    return has_unstaged_changes;
}

bool Status::has_untracked(Repository& repo) {
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.show = GIT_STATUS_SHOW_WORKDIR_ONLY;
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED;

    git_status_list* status = nullptr;
    int error = git_status_list_new(&status, repo.raw(), &opts);
    detail::check_libgit2_error(error, "getting status list");

    size_t count = git_status_list_entrycount(status);
    bool has_untracked_files = false;

    for (size_t i = 0; i < count && !has_untracked_files; ++i) {
        const git_status_entry* entry = git_status_byindex(status, i);
        if (entry && (entry->status & GIT_STATUS_WT_NEW)) {
            has_untracked_files = true;
        }
    }

    git_status_list_free(status);
    return has_untracked_files;
}

}  // namespace gitmanip
