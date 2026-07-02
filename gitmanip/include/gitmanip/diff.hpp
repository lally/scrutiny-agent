#pragma once

#include "types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gitmanip {

class Repository;
class Tree;
class Commit;

// Options for diff generation
struct DiffOptions {
    uint32_t context_lines = 3;
    bool ignore_whitespace = false;
    bool ignore_whitespace_change = false;
    bool ignore_whitespace_eol = false;
    bool include_untracked = false;
    bool recurse_untracked_dirs = false;
    std::vector<std::string> pathspec;  // Filter by paths
};

class Diff {
public:
    Diff(const Diff&) = delete;
    Diff& operator=(const Diff&) = delete;
    Diff(Diff&&) noexcept = default;
    Diff& operator=(Diff&&) noexcept = default;
    ~Diff() = default;

    [[nodiscard]] size_t num_deltas() const;
    [[nodiscard]] std::optional<DiffDelta> delta(size_t index) const;

    // Get all deltas
    [[nodiscard]] std::vector<DiffDelta> deltas() const;

    // Get hunks for a specific delta
    [[nodiscard]] std::vector<DiffHunk> hunks(size_t delta_index) const;

    // Get lines for a specific delta and hunk
    [[nodiscard]] std::vector<DiffLine> lines(size_t delta_index, size_t hunk_index) const;

    // Get patch text for a delta
    [[nodiscard]] std::string patch(size_t delta_index) const;

    // Get full patch text
    [[nodiscard]] std::string full_patch() const;

    // Statistics
    struct Stats {
        size_t files_changed;
        size_t insertions;
        size_t deletions;
    };
    [[nodiscard]] Stats stats() const;

    // Find similar files (renames/copies)
    Diff& find_similar(uint32_t rename_threshold = 50,
                       uint32_t copy_threshold = 50);

    // For libgit2 interop
    [[nodiscard]] git_diff* raw() const { return diff_.get(); }

private:
    friend class Repository;
    friend class Commit;
    Diff(Repository* repo, detail::GitPtr<git_diff> diff);

    Repository* repo_;
    detail::GitPtr<git_diff> diff_;
};

}  // namespace gitmanip
