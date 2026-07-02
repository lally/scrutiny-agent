#include "gitmanip/history.hpp"
#include "gitmanip/diff.hpp"
#include "gitmanip/error.hpp"
#include "gitmanip/repository.hpp"

#include <git2.h>
#include <fmt/format.h>

#include <algorithm>
#include <regex>
#include <set>

namespace gitmanip {

// FileHistory implementation
struct FileHistory::Impl {
    Repository* repo;
    std::vector<FileHistoryEntry> entries;
    std::string original_path;
    bool was_renamed = false;
};

FileHistory::FileHistory() : impl_(std::make_unique<Impl>()) {}
FileHistory::FileHistory(FileHistory&&) noexcept = default;
FileHistory& FileHistory::operator=(FileHistory&&) noexcept = default;
FileHistory::~FileHistory() = default;

FileHistory FileHistory::trace(Repository& repo, std::string_view path,
                                const FileHistoryOptions& options) {
    FileHistory history;
    history.impl_->repo = &repo;
    history.impl_->original_path = std::string(path);

    std::string current_path = std::string(path);
    std::set<std::string> seen_commits;

    // Create revision walker
    git_revwalk* walker = nullptr;
    int error = git_revwalk_new(&walker, repo.raw());
    detail::check_libgit2_error(error, "creating revision walker");

    // Start from HEAD or specified commit
    if (options.until_commit) {
        error = git_revwalk_push(walker, options.until_commit->raw());
    } else {
        error = git_revwalk_push_head(walker);
    }
    detail::check_libgit2_error(error, "pushing to walker");

    if (options.since_commit) {
        git_revwalk_hide(walker, options.since_commit->raw());
    }

    git_revwalk_sorting(walker, GIT_SORT_TIME);

    git_oid oid;
    size_t count = 0;

    while (git_revwalk_next(&oid, walker) == 0) {
        if (options.max_commits > 0 && count >= options.max_commits) break;

        std::string oid_str(GIT_OID_SHA1_HEXSIZE, '\0');
        git_oid_tostr(oid_str.data(), GIT_OID_SHA1_HEXSIZE + 1, &oid);

        if (seen_commits.count(oid_str)) continue;
        seen_commits.insert(oid_str);

        git_commit* commit = nullptr;
        error = git_commit_lookup(&commit, repo.raw(), &oid);
        if (error < 0) continue;

        // Get trees for this commit and its parent
        git_tree* commit_tree = nullptr;
        git_commit_tree(&commit_tree, commit);

        git_tree* parent_tree = nullptr;
        if (git_commit_parentcount(commit) > 0) {
            git_commit* parent = nullptr;
            git_commit_parent(&parent, commit, 0);
            if (parent) {
                git_commit_tree(&parent_tree, parent);
                git_commit_free(parent);
            }
        }

        // Create diff
        git_diff* diff = nullptr;
        git_diff_options diff_opts = GIT_DIFF_OPTIONS_INIT;

        if (options.follow_renames) {
            // We need to diff the whole tree to detect renames
            error = git_diff_tree_to_tree(&diff, repo.raw(), parent_tree, commit_tree, &diff_opts);
        } else {
            // Filter to just our file
            const char* pathspec = current_path.c_str();
            diff_opts.pathspec.strings = const_cast<char**>(&pathspec);
            diff_opts.pathspec.count = 1;
            error = git_diff_tree_to_tree(&diff, repo.raw(), parent_tree, commit_tree, &diff_opts);
        }

        if (parent_tree) git_tree_free(parent_tree);
        if (commit_tree) git_tree_free(commit_tree);

        if (error < 0 || !diff) {
            git_commit_free(commit);
            continue;
        }

        // Find renames if enabled
        if (options.follow_renames) {
            git_diff_find_options find_opts = GIT_DIFF_FIND_OPTIONS_INIT;
            find_opts.flags = GIT_DIFF_FIND_RENAMES | GIT_DIFF_FIND_COPIES;
            find_opts.rename_threshold = options.rename_threshold;
            git_diff_find_similar(diff, &find_opts);
        }

        // Look for our file in the diff
        size_t num_deltas = git_diff_num_deltas(diff);

        for (size_t i = 0; i < num_deltas; ++i) {
            const git_diff_delta* delta = git_diff_get_delta(diff, i);
            if (!delta) continue;

            std::string new_path = delta->new_file.path ? delta->new_file.path : "";
            std::string old_path = delta->old_file.path ? delta->old_file.path : "";

            bool matches_current = (new_path == current_path || old_path == current_path);

            if (!matches_current) continue;

            FileHistoryEntry entry;
            entry.commit_id = Oid(&oid);
            entry.path = current_path;

            switch (delta->status) {
                case GIT_DELTA_ADDED:
                    entry.change_type = FileHistoryEntry::ChangeType::Added;
                    break;
                case GIT_DELTA_DELETED:
                    entry.change_type = FileHistoryEntry::ChangeType::Deleted;
                    break;
                case GIT_DELTA_MODIFIED:
                    entry.change_type = FileHistoryEntry::ChangeType::Modified;
                    break;
                case GIT_DELTA_RENAMED:
                    entry.change_type = FileHistoryEntry::ChangeType::Renamed;
                    entry.previous_path = old_path;
                    if (options.follow_renames && new_path == current_path) {
                        current_path = old_path;
                        history.impl_->was_renamed = true;
                    }
                    break;
                case GIT_DELTA_COPIED:
                    entry.change_type = FileHistoryEntry::ChangeType::Copied;
                    entry.previous_path = old_path;
                    break;
                default:
                    entry.change_type = FileHistoryEntry::ChangeType::Modified;
                    break;
            }

            // Get line stats
            git_patch* patch = nullptr;
            if (git_patch_from_diff(&patch, diff, i) == 0) {
                size_t adds = 0, dels = 0;
                git_patch_line_stats(nullptr, &adds, &dels, patch);
                entry.lines_added = adds;
                entry.lines_deleted = dels;
                git_patch_free(patch);
            }

            history.impl_->entries.push_back(std::move(entry));
            ++count;
            break;
        }

        git_diff_free(diff);
        git_commit_free(commit);
    }

    git_revwalk_free(walker);

    // Set original path to the oldest path we found
    if (!history.impl_->entries.empty()) {
        history.impl_->original_path = current_path;
    }

    return history;
}

std::optional<Oid> FileHistory::find_introduction(Repository& repo, std::string_view path,
                                                   bool follow_renames) {
    FileHistoryOptions opts;
    opts.follow_renames = follow_renames;

    auto history = trace(repo, path, opts);

    // Find the "Added" entry, or the oldest entry
    for (auto it = history.impl_->entries.rbegin(); it != history.impl_->entries.rend(); ++it) {
        if (it->change_type == FileHistoryEntry::ChangeType::Added) {
            return it->commit_id;
        }
    }

    // If no explicit Add, return the oldest commit that has the file
    if (!history.impl_->entries.empty()) {
        return history.impl_->entries.back().commit_id;
    }

    return std::nullopt;
}

std::optional<Oid> FileHistory::find_deletion(Repository& repo, std::string_view path) {
    FileHistoryOptions opts;
    opts.follow_renames = false;

    auto history = trace(repo, path, opts);

    for (const auto& entry : history.impl_->entries) {
        if (entry.change_type == FileHistoryEntry::ChangeType::Deleted) {
            return entry.commit_id;
        }
    }

    return std::nullopt;
}

std::vector<std::string> FileHistory::all_paths(Repository& repo, std::string_view path) {
    FileHistoryOptions opts;
    opts.follow_renames = true;

    auto history = trace(repo, path, opts);

    std::vector<std::string> paths;
    std::set<std::string> seen;

    // Traverse in reverse (oldest first)
    for (auto it = history.impl_->entries.rbegin(); it != history.impl_->entries.rend(); ++it) {
        if (it->previous_path && seen.find(*it->previous_path) == seen.end()) {
            paths.push_back(*it->previous_path);
            seen.insert(*it->previous_path);
        }
        if (seen.find(it->path) == seen.end()) {
            paths.push_back(it->path);
            seen.insert(it->path);
        }
    }

    return paths;
}

size_t FileHistory::entry_count() const {
    return impl_->entries.size();
}

std::optional<FileHistoryEntry> FileHistory::entry(size_t index) const {
    if (index >= impl_->entries.size()) return std::nullopt;
    return impl_->entries[index];
}

const std::vector<FileHistoryEntry>& FileHistory::entries() const {
    return impl_->entries;
}

std::string FileHistory::original_path() const {
    return impl_->original_path;
}

bool FileHistory::was_renamed() const {
    return impl_->was_renamed;
}

std::optional<Oid> FileHistory::introduction_commit() const {
    if (impl_->entries.empty()) return std::nullopt;
    return impl_->entries.back().commit_id;
}

// CommitSearch implementation
std::vector<Commit> CommitSearch::by_message(Repository& repo, std::string_view text,
                                              const CommitSearchOptions& options) {
    std::vector<Commit> results;
    std::string search_text = options.case_insensitive
        ? std::string(text)
        : std::string(text);

    // Convert to lowercase for case-insensitive search
    std::string lower_search;
    if (options.case_insensitive) {
        lower_search = search_text;
        std::transform(lower_search.begin(), lower_search.end(), lower_search.begin(), ::tolower);
    }

    auto walker = repo.walk_commits();
    if (options.start_ref) {
        walker.push_ref(*options.start_ref);
    } else {
        walker.push_head();
    }
    walker.sort(CommitWalker::SortOrder::Time);

    for (auto&& commit : walker.walk()) {
        if (options.max_results > 0 && results.size() >= options.max_results) break;

        // Check date filters
        auto commit_time = commit.author().time;
        if (options.since && commit_time < *options.since) continue;
        if (options.until && commit_time > *options.until) continue;

        // Check author/committer filters
        if (options.author) {
            auto author = commit.author();
            bool matches = author.name.find(*options.author) != std::string::npos ||
                          author.email.find(*options.author) != std::string::npos;
            if (!matches) continue;
        }

        if (options.committer) {
            auto committer = commit.committer();
            bool matches = committer.name.find(*options.committer) != std::string::npos ||
                          committer.email.find(*options.committer) != std::string::npos;
            if (!matches) continue;
        }

        // Check message
        std::string message = commit.message();
        bool matches;

        if (options.case_insensitive) {
            std::string lower_message = message;
            std::transform(lower_message.begin(), lower_message.end(),
                          lower_message.begin(), ::tolower);
            matches = lower_message.find(lower_search) != std::string::npos;
        } else {
            matches = message.find(search_text) != std::string::npos;
        }

        if (matches) {
            results.push_back(std::move(commit));
        }
    }

    return results;
}

std::vector<Commit> CommitSearch::by_message_regex(Repository& repo, std::string_view pattern,
                                                    const CommitSearchOptions& options) {
    std::vector<Commit> results;

    std::regex::flag_type flags = std::regex::ECMAScript;
    if (options.case_insensitive) {
        flags |= std::regex::icase;
    }

    std::regex regex(std::string(pattern), flags);

    auto walker = repo.walk_commits();
    if (options.start_ref) {
        walker.push_ref(*options.start_ref);
    } else {
        walker.push_head();
    }
    walker.sort(CommitWalker::SortOrder::Time);

    for (auto&& commit : walker.walk()) {
        if (options.max_results > 0 && results.size() >= options.max_results) break;

        // Check date filters
        auto commit_time = commit.author().time;
        if (options.since && commit_time < *options.since) continue;
        if (options.until && commit_time > *options.until) continue;

        // Check author/committer filters
        if (options.author) {
            auto author = commit.author();
            bool matches = author.name.find(*options.author) != std::string::npos ||
                          author.email.find(*options.author) != std::string::npos;
            if (!matches) continue;
        }

        std::string message = commit.message();
        if (std::regex_search(message, regex)) {
            results.push_back(std::move(commit));
        }
    }

    return results;
}

std::vector<Commit> CommitSearch::touching_file(Repository& repo, std::string_view path,
                                                 bool follow_renames,
                                                 const CommitSearchOptions& options) {
    FileHistoryOptions hist_opts;
    hist_opts.follow_renames = follow_renames;
    hist_opts.max_commits = options.max_results;

    auto history = FileHistory::trace(repo, path, hist_opts);

    std::vector<Commit> results;
    for (const auto& entry : history.entries()) {
        results.push_back(repo.lookup_commit(entry.commit_id));
    }

    return results;
}

std::vector<Commit> CommitSearch::touching_files(Repository& repo,
                                                  const std::vector<std::string>& paths,
                                                  const CommitSearchOptions& options) {
    std::set<std::string> seen_commits;
    std::vector<Commit> results;

    for (const auto& path : paths) {
        auto file_commits = touching_file(repo, path, true, options);
        for (auto& commit : file_commits) {
            std::string id = commit.id().to_string();
            if (seen_commits.find(id) == seen_commits.end()) {
                seen_commits.insert(id);
                results.push_back(std::move(commit));
            }
        }
    }

    // Sort by time (most recent first)
    std::sort(results.begin(), results.end(), [](const Commit& a, const Commit& b) {
        return a.author().time > b.author().time;
    });

    // Apply max_results limit
    if (options.max_results > 0 && results.size() > options.max_results) {
        results.erase(results.begin() + static_cast<std::ptrdiff_t>(options.max_results), results.end());
    }

    return results;
}

std::vector<Commit> CommitSearch::by_content(Repository& repo, std::string_view text,
                                              const CommitSearchOptions& options) {
    std::vector<Commit> results;
    std::string search_text(text);

    auto walker = repo.walk_commits();
    if (options.start_ref) {
        walker.push_ref(*options.start_ref);
    } else {
        walker.push_head();
    }
    walker.sort(CommitWalker::SortOrder::Time);

    for (auto&& commit : walker.walk()) {
        if (options.max_results > 0 && results.size() >= options.max_results) break;

        // Check date filters
        auto commit_time = commit.author().time;
        if (options.since && commit_time < *options.since) continue;
        if (options.until && commit_time > *options.until) continue;

        // Get diff from parent
        auto diff = commit.diff_from_parent();
        std::string patch = diff.full_patch();

        // Simple pickaxe: check if the text appears in added/removed lines
        bool found = false;
        std::istringstream stream(patch);
        std::string line;

        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            char first = line[0];
            if (first == '+' || first == '-') {
                if (line.find(search_text) != std::string::npos) {
                    found = true;
                    break;
                }
            }
        }

        if (found) {
            results.push_back(std::move(commit));
        }
    }

    return results;
}

std::vector<Commit> CommitSearch::by_content_regex(Repository& repo, std::string_view pattern,
                                                    const CommitSearchOptions& options) {
    std::vector<Commit> results;

    std::regex::flag_type flags = std::regex::ECMAScript;
    if (options.case_insensitive) {
        flags |= std::regex::icase;
    }
    std::regex regex(std::string(pattern), flags);

    auto walker = repo.walk_commits();
    if (options.start_ref) {
        walker.push_ref(*options.start_ref);
    } else {
        walker.push_head();
    }
    walker.sort(CommitWalker::SortOrder::Time);

    for (auto&& commit : walker.walk()) {
        if (options.max_results > 0 && results.size() >= options.max_results) break;

        auto commit_time = commit.author().time;
        if (options.since && commit_time < *options.since) continue;
        if (options.until && commit_time > *options.until) continue;

        auto diff = commit.diff_from_parent();
        std::string patch = diff.full_patch();

        std::istringstream stream(patch);
        std::string line;
        bool found = false;

        while (std::getline(stream, line)) {
            if (line.empty()) continue;
            char first = line[0];
            if ((first == '+' || first == '-') && std::regex_search(line, regex)) {
                found = true;
                break;
            }
        }

        if (found) {
            results.push_back(std::move(commit));
        }
    }

    return results;
}

std::vector<Commit> CommitSearch::all(Repository& repo, const CommitSearchOptions& options) {
    std::vector<Commit> results;

    auto walker = repo.walk_commits();
    if (options.start_ref) {
        walker.push_ref(*options.start_ref);
    } else if (options.all_branches) {
        for (const auto& branch : repo.branch_names(true, true)) {
            walker.push_ref("refs/heads/" + branch);
        }
    } else {
        walker.push_head();
    }
    walker.sort(CommitWalker::SortOrder::Time);

    for (auto&& commit : walker.walk()) {
        if (options.max_results > 0 && results.size() >= options.max_results) break;

        auto commit_time = commit.author().time;
        if (options.since && commit_time < *options.since) continue;
        if (options.until && commit_time > *options.until) continue;

        if (options.author) {
            auto author = commit.author();
            bool matches = author.name.find(*options.author) != std::string::npos ||
                          author.email.find(*options.author) != std::string::npos;
            if (!matches) continue;
        }

        if (options.committer) {
            auto committer = commit.committer();
            bool matches = committer.name.find(*options.committer) != std::string::npos ||
                          committer.email.find(*options.committer) != std::string::npos;
            if (!matches) continue;
        }

        results.push_back(std::move(commit));
    }

    return results;
}

std::optional<Oid> CommitSearch::merge_base(Repository& repo, const Oid& oid1, const Oid& oid2) {
    git_oid base;
    int error = git_merge_base(&base, repo.raw(), oid1.raw(), oid2.raw());

    if (error == GIT_ENOTFOUND) {
        return std::nullopt;
    }
    detail::check_libgit2_error(error, "finding merge base");

    return Oid(&base);
}

bool CommitSearch::is_ancestor(Repository& repo, const Oid& ancestor, const Oid& descendant) {
    int result = git_graph_descendant_of(repo.raw(), descendant.raw(), ancestor.raw());
    if (result < 0) {
        detail::check_libgit2_error(result, "checking ancestry");
    }
    return result == 1;
}

}  // namespace gitmanip
