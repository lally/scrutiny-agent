#pragma once

/// @file blame.hpp
/// @brief Git blame/annotation support for tracking line-by-line authorship.
///
/// This header provides functionality to determine which commit last modified
/// each line of a file, similar to `git blame` or `git annotate`.
///
/// @section blame_example Example Usage
/// @code{.cpp}
/// auto repo = gitmanip::Repository::open("/path/to/repo");
///
/// // Get blame for a file at HEAD
/// auto blame = gitmanip::Blame::file(repo, "src/main.cpp");
///
/// // Iterate over all hunks (contiguous lines from same commit)
/// for (const auto& hunk : blame.hunks()) {
///     std::cout << "Lines " << hunk.start_line << "-" << hunk.end_line()
///               << " from commit " << hunk.final_commit_id.short_id()
///               << " by " << hunk.final_signature.name << "\n";
/// }
///
/// // Get blame for a specific line
/// auto line_blame = blame.line(42);
/// if (line_blame) {
///     std::cout << "Line 42 last modified by: " << line_blame->final_signature.name << "\n";
/// }
///
/// // Blame with options (ignore whitespace, track moves)
/// gitmanip::BlameOptions opts;
/// opts.ignore_whitespace = true;
/// opts.track_copies_same_file = true;
/// auto blame2 = gitmanip::Blame::file(repo, "src/main.cpp", opts);
/// @endcode

#include "types.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

struct git_blame;

namespace gitmanip {

class Repository;
class Commit;

/// @brief Options for controlling blame behavior.
///
/// These options mirror the flags available in `git blame`:
/// - Whitespace handling affects whether whitespace-only changes are attributed
/// - Copy/move detection can trace lines to their original source
/// - Date ranges can limit the blame to specific time periods
struct BlameOptions {
    /// @brief Ignore whitespace when comparing lines.
    /// Equivalent to `git blame -w`.
    bool ignore_whitespace = false;

    /// @brief Track lines moved within the same file.
    /// Equivalent to `git blame -M`.
    bool track_copies_same_file = false;

    /// @brief Track lines moved from other files in the same commit.
    /// Equivalent to `git blame -C`.
    bool track_copies_same_commit = false;

    /// @brief Track lines copied from other files in any commit.
    /// Equivalent to `git blame -C -C`.
    bool track_copies_any_commit = false;

    /// @brief Only consider commits newer than this OID.
    /// Lines modified before this commit will show this commit as the source.
    std::optional<Oid> newest_commit;

    /// @brief Only consider commits older than this OID.
    std::optional<Oid> oldest_commit;

    /// @brief Start line for partial blame (1-indexed, inclusive).
    /// Set to 0 to start from the beginning.
    size_t min_line = 0;

    /// @brief End line for partial blame (1-indexed, inclusive).
    /// Set to 0 to blame to the end of file.
    size_t max_line = 0;
};

/// @brief A contiguous range of lines attributed to a single commit.
///
/// A blame hunk represents one or more consecutive lines that were all
/// last modified by the same commit. The hunk contains both the "final"
/// information (where the lines are now) and the "original" information
/// (where the lines came from, which may differ due to renames/copies).
struct BlameHunk {
    /// @brief Number of lines in this hunk.
    size_t lines_in_hunk = 0;

    /// @brief The OID of the commit that last modified these lines.
    Oid final_commit_id;

    /// @brief Starting line number in the final file (1-indexed).
    size_t start_line = 0;

    /// @brief The signature of the committer in the final commit.
    Signature final_signature;

    /// @brief The OID of the commit where these lines originated.
    /// May differ from final_commit_id if the lines were moved/copied.
    Oid orig_commit_id;

    /// @brief The path of the file where these lines originated.
    /// May differ from the blamed file if the file was renamed.
    std::string orig_path;

    /// @brief Starting line number in the original file (1-indexed).
    size_t orig_start_line = 0;

    /// @brief The signature of the author in the original commit.
    Signature orig_signature;

    /// @brief Whether this hunk is from a boundary commit.
    /// A boundary commit is the oldest commit in the blame range.
    bool boundary = false;

    /// @brief Calculate the ending line number (1-indexed, inclusive).
    [[nodiscard]] size_t end_line() const { return start_line + lines_in_hunk - 1; }

    /// @brief Check if a line number falls within this hunk.
    [[nodiscard]] bool contains_line(size_t line) const {
        return line >= start_line && line < start_line + lines_in_hunk;
    }
};

/// @brief Blame information for a file, showing which commit modified each line.
///
/// The Blame class provides access to per-line attribution data for a file.
/// It can be used to:
/// - Find who last modified each line of code
/// - Track the history of specific lines through renames and copies
/// - Identify when specific changes were introduced
///
/// @note Blame operations can be expensive for large files or long histories.
///       Consider using BlameOptions to limit the scope when possible.
class Blame {
public:
    Blame(const Blame&) = delete;
    Blame& operator=(const Blame&) = delete;
    Blame(Blame&&) noexcept;
    Blame& operator=(Blame&&) noexcept;
    ~Blame();

    /// @brief Create blame information for a file.
    ///
    /// @param repo The repository containing the file.
    /// @param path Path to the file relative to repository root.
    /// @param options Options controlling blame behavior.
    /// @return Blame object containing line-by-line attribution.
    /// @throws GitError if the file doesn't exist or blame fails.
    ///
    /// @code{.cpp}
    /// auto blame = gitmanip::Blame::file(repo, "src/main.cpp");
    /// @endcode
    [[nodiscard]] static Blame file(Repository& repo, std::string_view path,
                                    const BlameOptions& options = {});

    /// @brief Create blame information for a file at a specific commit.
    ///
    /// @param repo The repository containing the file.
    /// @param path Path to the file relative to repository root.
    /// @param commit_oid The commit to blame from.
    /// @param options Options controlling blame behavior.
    /// @return Blame object containing line-by-line attribution.
    ///
    /// @code{.cpp}
    /// auto blame = gitmanip::Blame::file_at(repo, "src/main.cpp", some_commit_oid);
    /// @endcode
    [[nodiscard]] static Blame file_at(Repository& repo, std::string_view path,
                                       const Oid& commit_oid,
                                       const BlameOptions& options = {});

    /// @brief Get the number of hunks in the blame.
    [[nodiscard]] size_t hunk_count() const;

    /// @brief Get a specific hunk by index.
    ///
    /// @param index Zero-based hunk index.
    /// @return The hunk at the given index, or nullopt if out of range.
    [[nodiscard]] std::optional<BlameHunk> hunk(size_t index) const;

    /// @brief Get all hunks as a vector.
    [[nodiscard]] std::vector<BlameHunk> hunks() const;

    /// @brief Get blame information for a specific line.
    ///
    /// @param line_number The line number (1-indexed).
    /// @return Blame hunk containing the line, or nullopt if line doesn't exist.
    ///
    /// @code{.cpp}
    /// if (auto hunk = blame.line(42)) {
    ///     std::cout << "Line 42 modified by " << hunk->final_signature.name << "\n";
    /// }
    /// @endcode
    [[nodiscard]] std::optional<BlameHunk> line(size_t line_number) const;

    /// @brief Get the commit that introduced a specific line.
    ///
    /// This is a convenience method that returns just the commit OID for a line.
    ///
    /// @param line_number The line number (1-indexed).
    /// @return The OID of the commit that last modified this line.
    [[nodiscard]] std::optional<Oid> line_commit(size_t line_number) const;

private:
    Blame(Repository* repo, git_blame* blame);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gitmanip
