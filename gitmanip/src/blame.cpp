#include "gitmanip/blame.hpp"
#include "gitmanip/error.hpp"
#include "gitmanip/repository.hpp"

#include <git2.h>
#include <fmt/format.h>

namespace gitmanip {

struct Blame::Impl {
    Repository* repo;
    git_blame* blame = nullptr;

    ~Impl() {
        if (blame) {
            git_blame_free(blame);
        }
    }
};

Blame::Blame(Repository* repo, git_blame* blame)
    : impl_(std::make_unique<Impl>()) {
    impl_->repo = repo;
    impl_->blame = blame;
}

Blame::Blame(Blame&&) noexcept = default;
Blame& Blame::operator=(Blame&&) noexcept = default;
Blame::~Blame() = default;

namespace {

BlameHunk convert_hunk(const git_blame_hunk* hunk) {
    BlameHunk result;
    result.lines_in_hunk = hunk->lines_in_hunk;
    result.final_commit_id = Oid(&hunk->final_commit_id);
    result.start_line = hunk->final_start_line_number;
    result.orig_commit_id = Oid(&hunk->orig_commit_id);
    result.orig_path = hunk->orig_path ? hunk->orig_path : "";
    result.orig_start_line = hunk->orig_start_line_number;
    result.boundary = hunk->boundary != 0;

    if (hunk->final_signature) {
        result.final_signature.name = hunk->final_signature->name ? hunk->final_signature->name : "";
        result.final_signature.email = hunk->final_signature->email ? hunk->final_signature->email : "";
        result.final_signature.time = std::chrono::system_clock::from_time_t(hunk->final_signature->when.time);
        result.final_signature.offset_minutes = hunk->final_signature->when.offset;
    }

    if (hunk->orig_signature) {
        result.orig_signature.name = hunk->orig_signature->name ? hunk->orig_signature->name : "";
        result.orig_signature.email = hunk->orig_signature->email ? hunk->orig_signature->email : "";
        result.orig_signature.time = std::chrono::system_clock::from_time_t(hunk->orig_signature->when.time);
        result.orig_signature.offset_minutes = hunk->orig_signature->when.offset;
    }

    return result;
}

}  // namespace

Blame Blame::file(Repository& repo, std::string_view path, const BlameOptions& options) {
    git_blame_options opts = GIT_BLAME_OPTIONS_INIT;

    if (options.ignore_whitespace) {
        opts.flags |= GIT_BLAME_IGNORE_WHITESPACE;
    }
    if (options.track_copies_same_file) {
        opts.flags |= GIT_BLAME_TRACK_COPIES_SAME_FILE;
    }
    if (options.track_copies_same_commit) {
        opts.flags |= GIT_BLAME_TRACK_COPIES_SAME_COMMIT_MOVES;
    }
    if (options.track_copies_any_commit) {
        opts.flags |= GIT_BLAME_TRACK_COPIES_ANY_COMMIT_COPIES;
    }

    if (options.newest_commit) {
        opts.newest_commit = *options.newest_commit->raw();
    }
    if (options.oldest_commit) {
        opts.oldest_commit = *options.oldest_commit->raw();
    }

    opts.min_line = static_cast<uint32_t>(options.min_line);
    opts.max_line = static_cast<uint32_t>(options.max_line);

    git_blame* blame = nullptr;
    int error = git_blame_file(&blame, repo.raw(), std::string(path).c_str(), &opts);
    detail::check_libgit2_error(error, fmt::format("blaming file {}", path));

    return Blame(&repo, blame);
}

Blame Blame::file_at(Repository& repo, std::string_view path, const Oid& commit_oid,
                     const BlameOptions& options) {
    BlameOptions opts_copy = options;
    opts_copy.newest_commit = commit_oid;
    return file(repo, path, opts_copy);
}

size_t Blame::hunk_count() const {
    return git_blame_get_hunk_count(impl_->blame);
}

std::optional<BlameHunk> Blame::hunk(size_t index) const {
    const git_blame_hunk* hunk = git_blame_get_hunk_byindex(impl_->blame, static_cast<uint32_t>(index));
    if (!hunk) return std::nullopt;
    return convert_hunk(hunk);
}

std::vector<BlameHunk> Blame::hunks() const {
    std::vector<BlameHunk> result;
    size_t count = hunk_count();
    result.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const git_blame_hunk* hunk = git_blame_get_hunk_byindex(impl_->blame, static_cast<uint32_t>(i));
        if (hunk) {
            result.push_back(convert_hunk(hunk));
        }
    }

    return result;
}

std::optional<BlameHunk> Blame::line(size_t line_number) const {
    const git_blame_hunk* hunk = git_blame_get_hunk_byline(impl_->blame, line_number);
    if (!hunk) return std::nullopt;
    return convert_hunk(hunk);
}

std::optional<Oid> Blame::line_commit(size_t line_number) const {
    auto hunk_opt = line(line_number);
    if (!hunk_opt) return std::nullopt;
    return hunk_opt->final_commit_id;
}

}  // namespace gitmanip
