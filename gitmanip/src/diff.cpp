#include "gitmanip/diff.hpp"
#include "gitmanip/error.hpp"
#include "gitmanip/repository.hpp"

#include <git2.h>
#include <fmt/format.h>

namespace gitmanip {

// Diff implementation
Diff::Diff(Repository* repo, detail::GitPtr<git_diff> diff)
    : repo_(repo), diff_(std::move(diff)) {}

size_t Diff::num_deltas() const {
    return git_diff_num_deltas(diff_.get());
}

namespace {

DiffDelta::Status convert_status(git_delta_t status) {
    switch (status) {
        case GIT_DELTA_UNMODIFIED: return DiffDelta::Status::Unmodified;
        case GIT_DELTA_ADDED: return DiffDelta::Status::Added;
        case GIT_DELTA_DELETED: return DiffDelta::Status::Deleted;
        case GIT_DELTA_MODIFIED: return DiffDelta::Status::Modified;
        case GIT_DELTA_RENAMED: return DiffDelta::Status::Renamed;
        case GIT_DELTA_COPIED: return DiffDelta::Status::Copied;
        case GIT_DELTA_IGNORED: return DiffDelta::Status::Ignored;
        case GIT_DELTA_UNTRACKED: return DiffDelta::Status::Untracked;
        case GIT_DELTA_TYPECHANGE: return DiffDelta::Status::Typechange;
        case GIT_DELTA_UNREADABLE: return DiffDelta::Status::Unreadable;
        case GIT_DELTA_CONFLICTED: return DiffDelta::Status::Conflicted;
        default: return DiffDelta::Status::Unmodified;
    }
}

DiffDelta convert_delta(const git_diff_delta* delta) {
    DiffDelta result;
    result.status = convert_status(delta->status);
    result.old_path = delta->old_file.path ? delta->old_file.path : "";
    result.new_path = delta->new_file.path ? delta->new_file.path : "";
    result.old_oid = Oid(&delta->old_file.id);
    result.new_oid = Oid(&delta->new_file.id);
    result.old_mode = delta->old_file.mode;
    result.new_mode = delta->new_file.mode;
    result.similarity = delta->similarity;
    return result;
}

}  // namespace

std::optional<DiffDelta> Diff::delta(size_t index) const {
    const git_diff_delta* d = git_diff_get_delta(diff_.get(), index);
    if (!d) return std::nullopt;
    return convert_delta(d);
}

std::vector<DiffDelta> Diff::deltas() const {
    std::vector<DiffDelta> result;
    size_t count = num_deltas();
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const git_diff_delta* d = git_diff_get_delta(diff_.get(), i);
        if (d) {
            result.push_back(convert_delta(d));
        }
    }
    return result;
}

std::vector<DiffHunk> Diff::hunks(size_t delta_index) const {
    std::vector<DiffHunk> result;

    git_patch* patch = nullptr;
    int error = git_patch_from_diff(&patch, diff_.get(), delta_index);
    if (error < 0) return result;

    size_t num_hunks = git_patch_num_hunks(patch);
    result.reserve(num_hunks);

    for (size_t i = 0; i < num_hunks; ++i) {
        const git_diff_hunk* hunk = nullptr;
        size_t lines_in_hunk = 0;

        if (git_patch_get_hunk(&hunk, &lines_in_hunk, patch, i) == 0 && hunk) {
            DiffHunk h;
            h.old_start = hunk->old_start;
            h.old_lines = hunk->old_lines;
            h.new_start = hunk->new_start;
            h.new_lines = hunk->new_lines;
            h.header = std::string(hunk->header, hunk->header_len);
            result.push_back(std::move(h));
        }
    }

    git_patch_free(patch);
    return result;
}

std::vector<DiffLine> Diff::lines(size_t delta_index, size_t hunk_index) const {
    std::vector<DiffLine> result;

    git_patch* patch = nullptr;
    int error = git_patch_from_diff(&patch, diff_.get(), delta_index);
    if (error < 0) return result;

    const git_diff_hunk* hunk = nullptr;
    size_t lines_in_hunk = 0;

    if (git_patch_get_hunk(&hunk, &lines_in_hunk, patch, hunk_index) != 0) {
        git_patch_free(patch);
        return result;
    }

    result.reserve(lines_in_hunk);

    for (size_t i = 0; i < lines_in_hunk; ++i) {
        const git_diff_line* line = nullptr;
        if (git_patch_get_line_in_hunk(&line, patch, hunk_index, i) == 0 && line) {
            DiffLine l;
            l.origin = static_cast<DiffLine::Origin>(line->origin);
            l.content = std::string(line->content, line->content_len);
            l.old_lineno = line->old_lineno;
            l.new_lineno = line->new_lineno;
            result.push_back(std::move(l));
        }
    }

    git_patch_free(patch);
    return result;
}

std::string Diff::patch(size_t delta_index) const {
    git_patch* patch = nullptr;
    int error = git_patch_from_diff(&patch, diff_.get(), delta_index);
    detail::check_libgit2_error(error, "creating patch from diff");

    git_buf buf = GIT_BUF_INIT;
    error = git_patch_to_buf(&buf, patch);
    git_patch_free(patch);

    if (error < 0) {
        git_buf_dispose(&buf);
        detail::check_libgit2_error(error, "converting patch to buffer");
    }

    std::string result(buf.ptr, buf.size);
    git_buf_dispose(&buf);
    return result;
}

std::string Diff::full_patch() const {
    std::string result;
    size_t count = num_deltas();
    for (size_t i = 0; i < count; ++i) {
        result += patch(i);
    }
    return result;
}

Diff::Stats Diff::stats() const {
    git_diff_stats* stats = nullptr;
    int error = git_diff_get_stats(&stats, diff_.get());
    detail::check_libgit2_error(error, "getting diff stats");

    Stats result;
    result.files_changed = git_diff_stats_files_changed(stats);
    result.insertions = git_diff_stats_insertions(stats);
    result.deletions = git_diff_stats_deletions(stats);

    git_diff_stats_free(stats);
    return result;
}

Diff& Diff::find_similar(uint32_t rename_threshold, uint32_t copy_threshold) {
    git_diff_find_options opts = GIT_DIFF_FIND_OPTIONS_INIT;
    opts.flags = GIT_DIFF_FIND_RENAMES | GIT_DIFF_FIND_COPIES;
    opts.rename_threshold = rename_threshold;
    opts.copy_threshold = copy_threshold;

    int error = git_diff_find_similar(diff_.get(), &opts);
    detail::check_libgit2_error(error, "finding similar files in diff");

    return *this;
}

}  // namespace gitmanip
