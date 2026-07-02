#pragma once

/// @file history.hpp
/// @brief File history tracking and commit search functionality.
///
/// This header provides functionality to:
/// - Find which commit introduced a specific file
/// - Track file history through renames
/// - Search commit messages
/// - Find commits that modified specific files
///
/// @section history_example Example Usage
/// @code{.cpp}
/// auto repo = gitmanip::Repository::open("/path/to/repo");
///
/// // Find when a file was first added
/// auto intro = gitmanip::FileHistory::find_introduction(repo, "src/new_feature.cpp");
/// if (intro) {
///     std::cout << "File introduced in commit " << intro->short_id() << "\n";
/// }
///
/// // Get full history of a file (follows renames)
/// auto history = gitmanip::FileHistory::trace(repo, "src/renamed_file.cpp");
/// for (const auto& entry : history.entries()) {
///     std::cout << entry.commit_id.short_id() << " " << entry.path << "\n";
/// }
///
/// // Search for commits by message
/// auto results = gitmanip::CommitSearch::by_message(repo, "bug fix");
/// for (auto commit : results) {
///     std::cout << commit.summary() << "\n";
/// }
///
/// // Find commits that touched a file
/// auto commits = gitmanip::CommitSearch::touching_file(repo, "src/main.cpp");
/// @endcode

#include "commit.hpp"
#include "types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace gitmanip {

class Repository;
class Commit;

/// @brief Options for file history tracing.
struct FileHistoryOptions {
    /// @brief Follow file renames/copies.
    /// Equivalent to `git log --follow`.
    bool follow_renames = true;

    /// @brief Similarity threshold for rename detection (0-100).
    /// Files must be at least this similar to be considered a rename.
    uint32_t rename_threshold = 50;

    /// @brief Maximum number of commits to examine.
    /// Set to 0 for unlimited.
    size_t max_commits = 0;

    /// @brief Only include commits after this one (exclusive).
    std::optional<Oid> since_commit;

    /// @brief Only include commits before this one (inclusive).
    std::optional<Oid> until_commit;
};

/// @brief A single entry in a file's history.
struct FileHistoryEntry {
    /// @brief The commit that modified the file.
    Oid commit_id;

    /// @brief The path of the file in this commit.
    /// May differ from the current path if the file was renamed.
    std::string path;

    /// @brief The previous path if this commit renamed the file.
    std::optional<std::string> previous_path;

    /// @brief What type of change was made to the file.
    enum class ChangeType {
        Added,      ///< File was created in this commit
        Modified,   ///< File content was changed
        Renamed,    ///< File was renamed (possibly with modifications)
        Copied,     ///< File was copied from another file
        Deleted,    ///< File was deleted in this commit
    };
    ChangeType change_type;

    /// @brief Lines added in this commit (for non-binary files).
    size_t lines_added = 0;

    /// @brief Lines deleted in this commit (for non-binary files).
    size_t lines_deleted = 0;
};

/// @brief File history tracking with rename following.
///
/// FileHistory provides methods to trace the complete history of a file,
/// including tracking through renames and copies. This is useful for:
/// - Finding when a file was introduced
/// - Understanding how a file evolved over time
/// - Tracking a file's original name before renames
///
/// @note History tracing can be expensive for files with long histories.
///       Use FileHistoryOptions::max_commits to limit scope when needed.
class FileHistory {
public:
    FileHistory(const FileHistory&) = delete;
    FileHistory& operator=(const FileHistory&) = delete;
    FileHistory(FileHistory&&) noexcept;
    FileHistory& operator=(FileHistory&&) noexcept;
    ~FileHistory();

    /// @brief Trace the complete history of a file.
    ///
    /// Returns all commits that modified the file, optionally following
    /// renames to trace the file's complete history.
    ///
    /// @param repo The repository to search.
    /// @param path Current path to the file.
    /// @param options Options controlling history tracing.
    /// @return FileHistory object containing all history entries.
    ///
    /// @code{.cpp}
    /// auto history = gitmanip::FileHistory::trace(repo, "src/utils.cpp");
    /// std::cout << "File has " << history.entry_count() << " modifications\n";
    /// @endcode
    [[nodiscard]] static FileHistory trace(Repository& repo, std::string_view path,
                                           const FileHistoryOptions& options = {});

    /// @brief Find the commit that introduced a file.
    ///
    /// This finds the oldest commit where the file exists, following
    /// renames if enabled. This is the commit that first added the file
    /// (or the file it was renamed/copied from).
    ///
    /// @param repo The repository to search.
    /// @param path Current path to the file.
    /// @param follow_renames Whether to follow renames to find the original file.
    /// @return OID of the commit that introduced the file, or nullopt if not found.
    ///
    /// @code{.cpp}
    /// if (auto oid = gitmanip::FileHistory::find_introduction(repo, "src/new.cpp")) {
    ///     auto commit = repo.lookup_commit(*oid);
    ///     std::cout << "File introduced by: " << commit.author().name << "\n";
    /// }
    /// @endcode
    [[nodiscard]] static std::optional<Oid> find_introduction(
        Repository& repo, std::string_view path, bool follow_renames = true);

    /// @brief Find the commit that deleted a file.
    ///
    /// @param repo The repository to search.
    /// @param path Path where the file used to exist.
    /// @return OID of the commit that deleted the file, or nullopt if file exists or never existed.
    [[nodiscard]] static std::optional<Oid> find_deletion(
        Repository& repo, std::string_view path);

    /// @brief Get all original paths this file has had.
    ///
    /// If the file was renamed multiple times, this returns all paths
    /// in chronological order (oldest first).
    ///
    /// @param repo The repository to search.
    /// @param path Current path to the file.
    /// @return Vector of all paths the file has had, including current.
    ///
    /// @code{.cpp}
    /// auto paths = gitmanip::FileHistory::all_paths(repo, "src/new_name.cpp");
    /// // paths might be: ["src/original.cpp", "src/old_name.cpp", "src/new_name.cpp"]
    /// @endcode
    [[nodiscard]] static std::vector<std::string> all_paths(
        Repository& repo, std::string_view path);

    /// @brief Get the number of history entries.
    [[nodiscard]] size_t entry_count() const;

    /// @brief Get a specific history entry by index.
    ///
    /// @param index Zero-based index (0 = most recent).
    /// @return The history entry, or nullopt if out of range.
    [[nodiscard]] std::optional<FileHistoryEntry> entry(size_t index) const;

    /// @brief Get all history entries.
    ///
    /// Entries are ordered from most recent to oldest.
    [[nodiscard]] const std::vector<FileHistoryEntry>& entries() const;

    /// @brief Get the original path (before any renames).
    ///
    /// @return The oldest known path for this file.
    [[nodiscard]] std::string original_path() const;

    /// @brief Check if the file was ever renamed.
    [[nodiscard]] bool was_renamed() const;

    /// @brief Get the commit that introduced the file.
    ///
    /// @return OID of the introducing commit, or nullopt if history is empty.
    [[nodiscard]] std::optional<Oid> introduction_commit() const;

private:
    FileHistory();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief Options for commit searching.
struct CommitSearchOptions {
    /// @brief Maximum number of results to return.
    /// Set to 0 for unlimited.
    size_t max_results = 100;

    /// @brief Only search commits after this date.
    std::optional<std::chrono::system_clock::time_point> since;

    /// @brief Only search commits before this date.
    std::optional<std::chrono::system_clock::time_point> until;

    /// @brief Only search commits by this author (name or email).
    std::optional<std::string> author;

    /// @brief Only search commits by this committer (name or email).
    std::optional<std::string> committer;

    /// @brief Starting point for the search (default: HEAD).
    std::optional<std::string> start_ref;

    /// @brief Whether to search all branches.
    bool all_branches = false;

    /// @brief Case-insensitive matching for text searches.
    bool case_insensitive = true;
};

/// @brief Search for commits by various criteria.
///
/// CommitSearch provides methods to find commits based on:
/// - Message content (including regex)
/// - Files modified
/// - Author/committer
/// - Content changes (pickaxe search)
///
/// @section search_examples Examples
/// @code{.cpp}
/// // Find all commits mentioning "bug fix"
/// auto fixes = gitmanip::CommitSearch::by_message(repo, "bug fix");
///
/// // Find commits with regex pattern
/// auto tickets = gitmanip::CommitSearch::by_message_regex(repo, "JIRA-\\d+");
///
/// // Find commits that modified a file
/// auto file_commits = gitmanip::CommitSearch::touching_file(repo, "src/main.cpp");
///
/// // Find commits that added/removed specific text
/// auto text_commits = gitmanip::CommitSearch::by_content(repo, "TODO");
///
/// // Find commits by author
/// gitmanip::CommitSearchOptions opts;
/// opts.author = "alice@example.com";
/// auto alice_commits = gitmanip::CommitSearch::all(repo, opts);
/// @endcode
class CommitSearch {
public:
    /// @brief Search for commits whose message contains a string.
    ///
    /// @param repo The repository to search.
    /// @param text Text to search for in commit messages.
    /// @param options Search options (max results, date range, etc.).
    /// @return Vector of matching commits (most recent first).
    [[nodiscard]] static std::vector<Commit> by_message(
        Repository& repo, std::string_view text,
        const CommitSearchOptions& options = {});

    /// @brief Search for commits whose message matches a regex.
    ///
    /// @param repo The repository to search.
    /// @param pattern Regex pattern to match against commit messages.
    /// @param options Search options.
    /// @return Vector of matching commits (most recent first).
    ///
    /// @code{.cpp}
    /// // Find commits referencing JIRA tickets
    /// auto commits = gitmanip::CommitSearch::by_message_regex(repo, "PROJ-\\d+");
    /// @endcode
    [[nodiscard]] static std::vector<Commit> by_message_regex(
        Repository& repo, std::string_view pattern,
        const CommitSearchOptions& options = {});

    /// @brief Find all commits that modified a specific file.
    ///
    /// @param repo The repository to search.
    /// @param path Path to the file.
    /// @param follow_renames Follow file renames.
    /// @param options Search options.
    /// @return Vector of commits that touched the file.
    [[nodiscard]] static std::vector<Commit> touching_file(
        Repository& repo, std::string_view path,
        bool follow_renames = true,
        const CommitSearchOptions& options = {});

    /// @brief Find all commits that modified any of the specified files.
    ///
    /// @param repo The repository to search.
    /// @param paths Paths to search for.
    /// @param options Search options.
    /// @return Vector of commits that touched any of the files.
    [[nodiscard]] static std::vector<Commit> touching_files(
        Repository& repo, const std::vector<std::string>& paths,
        const CommitSearchOptions& options = {});

    /// @brief Find commits that added or removed specific text (pickaxe search).
    ///
    /// This is equivalent to `git log -S "text"`. It finds commits where
    /// the number of occurrences of the text changed.
    ///
    /// @param repo The repository to search.
    /// @param text Text to search for in diffs.
    /// @param options Search options.
    /// @return Vector of commits that added/removed the text.
    ///
    /// @code{.cpp}
    /// // Find when "deprecated_function" was added or removed
    /// auto commits = gitmanip::CommitSearch::by_content(repo, "deprecated_function");
    /// @endcode
    [[nodiscard]] static std::vector<Commit> by_content(
        Repository& repo, std::string_view text,
        const CommitSearchOptions& options = {});

    /// @brief Find commits that match a regex in their diffs (pickaxe regex).
    ///
    /// This is equivalent to `git log -G "pattern"`. It finds commits where
    /// added or removed lines match the pattern.
    ///
    /// @param repo The repository to search.
    /// @param pattern Regex pattern to match in diffs.
    /// @param options Search options.
    /// @return Vector of commits with matching diff content.
    [[nodiscard]] static std::vector<Commit> by_content_regex(
        Repository& repo, std::string_view pattern,
        const CommitSearchOptions& options = {});

    /// @brief Get all commits matching the given options.
    ///
    /// This is a general-purpose method that applies all filters in
    /// CommitSearchOptions.
    ///
    /// @param repo The repository to search.
    /// @param options Search options (author, date range, etc.).
    /// @return Vector of matching commits.
    [[nodiscard]] static std::vector<Commit> all(
        Repository& repo, const CommitSearchOptions& options = {});

    /// @brief Find the merge base (common ancestor) of two commits.
    ///
    /// @param repo The repository.
    /// @param oid1 First commit OID.
    /// @param oid2 Second commit OID.
    /// @return OID of the merge base, or nullopt if none exists.
    [[nodiscard]] static std::optional<Oid> merge_base(
        Repository& repo, const Oid& oid1, const Oid& oid2);

    /// @brief Check if a commit is an ancestor of another.
    ///
    /// @param repo The repository.
    /// @param ancestor Potential ancestor commit.
    /// @param descendant Potential descendant commit.
    /// @return true if ancestor is an ancestor of descendant.
    [[nodiscard]] static bool is_ancestor(
        Repository& repo, const Oid& ancestor, const Oid& descendant);
};

}  // namespace gitmanip
