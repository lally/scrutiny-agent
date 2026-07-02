#pragma once

/// @file status.hpp
/// @brief Working directory and sync status queries.
///
/// This header provides types and functions for querying repository status:
/// - Working directory status (staged, unstaged, untracked files)
/// - Sync status relative to upstream (ahead/behind counts and commits)
///
/// @section status_example Example Usage
/// @code{.cpp}
/// auto repo = gitmanip::Repository::open("/path/to/repo");
///
/// // Get working directory status
/// auto entries = gitmanip::Status::list(repo);
/// for (const auto& entry : entries) {
///     std::cout << entry.path;
///     if (entry.index_status) std::cout << " [staged: " << to_string(*entry.index_status) << "]";
///     if (entry.workdir_status) std::cout << " [unstaged: " << to_string(*entry.workdir_status) << "]";
///     std::cout << "\n";
/// }
///
/// // Get sync status for current branch
/// if (auto sync = gitmanip::Status::sync(repo)) {
///     std::cout << "Ahead: " << sync->ahead.size() << ", Behind: " << sync->behind.size() << "\n";
/// }
/// @endcode

#include "types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace gitmanip {

class Repository;

/// @brief Status of a file in the index or working directory.
enum class FileState {
    New,         ///< File is new (added/untracked)
    Modified,    ///< File content changed
    Deleted,     ///< File was deleted
    Renamed,     ///< File was renamed
    Copied,      ///< File was copied
    TypeChange,  ///< File type changed (e.g., file to symlink)
    Ignored,     ///< File is ignored by .gitignore
    Conflicted,  ///< File has merge conflicts
};

/// @brief Convert FileState to string representation.
[[nodiscard]] inline const char* to_string(FileState state) {
    switch (state) {
        case FileState::New: return "new";
        case FileState::Modified: return "modified";
        case FileState::Deleted: return "deleted";
        case FileState::Renamed: return "renamed";
        case FileState::Copied: return "copied";
        case FileState::TypeChange: return "typechange";
        case FileState::Ignored: return "ignored";
        case FileState::Conflicted: return "conflicted";
    }
    return "unknown";
}

/// @brief Status entry for a single file.
///
/// A file can have status in the index (staged changes) and/or in the
/// working directory (unstaged changes). If both are set, the file has
/// both staged and unstaged modifications.
struct StatusEntry {
    /// @brief Path to the file relative to repository root.
    std::string path;

    /// @brief Old path if file was renamed or copied (in index).
    std::optional<std::string> old_path;

    /// @brief Status in the index (staged changes).
    /// nullopt means no staged changes for this file.
    std::optional<FileState> index_status;

    /// @brief Status in the working directory (unstaged changes).
    /// nullopt means no unstaged changes for this file.
    std::optional<FileState> workdir_status;

    /// @brief Similarity percentage for renames/copies (0-100).
    uint32_t similarity = 0;
};

/// @brief Options for status queries.
struct StatusOptions {
    /// @brief Include untracked files.
    bool include_untracked = true;

    /// @brief Recurse into untracked directories.
    bool recurse_untracked_dirs = true;

    /// @brief Include ignored files.
    bool include_ignored = false;

    /// @brief Detect renames.
    bool detect_renames = true;

    /// @brief Rename detection threshold (0-100).
    uint32_t rename_threshold = 50;

    /// @brief Paths to filter (empty = all paths).
    std::vector<std::string> pathspec;
};

/// @brief Synchronization status relative to upstream.
///
/// Describes how a local branch differs from its upstream tracking branch.
struct SyncStatus {
    /// @brief Name of the upstream branch (e.g., "origin/main").
    std::string upstream;

    /// @brief Commits on local branch that are not on upstream.
    /// These are the commits that would be pushed.
    std::vector<Oid> ahead;

    /// @brief Commits on upstream that are not on local branch.
    /// These are the commits that would be fetched/pulled.
    std::vector<Oid> behind;

    /// @brief Number of commits ahead (convenience accessor).
    [[nodiscard]] size_t ahead_count() const { return ahead.size(); }

    /// @brief Number of commits behind (convenience accessor).
    [[nodiscard]] size_t behind_count() const { return behind.size(); }

    /// @brief Check if local and upstream are in sync.
    [[nodiscard]] bool is_synced() const { return ahead.empty() && behind.empty(); }
};

/// @brief Working directory and sync status queries.
class Status {
public:
    /// @brief Get the status of all files in the working directory.
    ///
    /// Returns status entries for files that have staged changes,
    /// unstaged changes, or are untracked (based on options).
    ///
    /// @param repo The repository.
    /// @param options Status query options.
    /// @return Vector of status entries.
    ///
    /// @code{.cpp}
    /// auto entries = gitmanip::Status::list(repo);
    /// for (const auto& entry : entries) {
    ///     if (entry.index_status && entry.workdir_status) {
    ///         std::cout << entry.path << " is partially staged\n";
    ///     }
    /// }
    /// @endcode
    [[nodiscard]] static std::vector<StatusEntry> list(Repository& repo,
                                                        const StatusOptions& options = {});

    /// @brief Get synchronization status for the current branch.
    ///
    /// Returns the ahead/behind status relative to the upstream tracking
    /// branch. Returns nullopt if:
    /// - HEAD is detached
    /// - Current branch has no upstream configured
    ///
    /// @param repo The repository.
    /// @return Sync status, or nullopt if not applicable.
    ///
    /// @code{.cpp}
    /// if (auto sync = gitmanip::Status::sync(repo)) {
    ///     if (!sync->is_synced()) {
    ///         std::cout << sync->ahead_count() << " to push, "
    ///                   << sync->behind_count() << " to pull\n";
    ///     }
    /// }
    /// @endcode
    [[nodiscard]] static std::optional<SyncStatus> sync(Repository& repo);

    /// @brief Get synchronization status for a specific branch.
    ///
    /// @param repo The repository.
    /// @param branch_name Local branch name.
    /// @return Sync status, or nullopt if branch has no upstream.
    ///
    /// @code{.cpp}
    /// if (auto sync = gitmanip::Status::sync(repo, "feature")) {
    ///     for (const auto& oid : sync->ahead) {
    ///         auto commit = repo.lookup_commit(oid);
    ///         std::cout << "To push: " << commit.summary() << "\n";
    ///     }
    /// }
    /// @endcode
    [[nodiscard]] static std::optional<SyncStatus> sync(Repository& repo,
                                                         std::string_view branch_name);

    /// @brief Check if there are any uncommitted changes.
    ///
    /// This is a fast check that returns true if there are any staged
    /// or unstaged changes (excluding untracked files).
    ///
    /// @param repo The repository.
    /// @return true if there are uncommitted changes.
    [[nodiscard]] static bool has_changes(Repository& repo);

    /// @brief Check if there are staged changes.
    ///
    /// @param repo The repository.
    /// @return true if there are staged changes.
    [[nodiscard]] static bool has_staged(Repository& repo);

    /// @brief Check if there are unstaged changes.
    ///
    /// @param repo The repository.
    /// @return true if there are unstaged changes to tracked files.
    [[nodiscard]] static bool has_unstaged(Repository& repo);

    /// @brief Check if there are untracked files.
    ///
    /// @param repo The repository.
    /// @return true if there are untracked files.
    [[nodiscard]] static bool has_untracked(Repository& repo);
};

}  // namespace gitmanip
