#pragma once

/// @file commit.hpp
/// @brief Commit access and history walking.
///
/// This header provides the Commit class for accessing commit data and
/// the CommitWalker class for iterating over commit history.
///
/// @section commit_example Basic Usage
/// @code{.cpp}
/// auto repo = gitmanip::Repository::open("/path/to/repo");
/// auto commit = repo.lookup_commit("HEAD");
///
/// // Access commit data
/// std::cout << "SHA: " << commit.id().short_id() << "\n";
/// std::cout << "Message: " << commit.summary() << "\n";
/// std::cout << "Author: " << commit.author().name << "\n";
///
/// // Get the diff introduced by this commit
/// auto diff = commit.diff_from_parent();
/// std::cout << "Files changed: " << diff.stats().files_changed << "\n";
///
/// // For merge commits, get combined diff showing only merge-specific changes
/// if (commit.is_merge()) {
///     auto merge_changes = commit.merge_diff();
///     // Shows only conflict resolutions and "evil merge" changes
/// }
/// @endcode

#include "types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gitmanip {

class Repository;
class Tree;
class Diff;

/// @brief Represents a Git commit object.
///
/// A Commit provides access to all commit metadata (message, author, committer,
/// parents) as well as the tree snapshot and diff operations.
///
/// @note Commit objects hold a reference to their Repository and must not
///       outlive it.
class Commit {
public:
    Commit(const Commit&) = delete;
    Commit& operator=(const Commit&) = delete;
    Commit(Commit&&) noexcept = default;
    Commit& operator=(Commit&&) noexcept = default;
    ~Commit() = default;

    // =========================================================================
    // Basic Properties
    // =========================================================================

    /// @brief Get the commit's object ID (SHA).
    [[nodiscard]] Oid id() const;

    /// @brief Get the full commit message.
    [[nodiscard]] std::string message() const;

    /// @brief Get the first line of the commit message.
    [[nodiscard]] std::string summary() const;

    /// @brief Get the commit message body (everything after the first line).
    [[nodiscard]] std::string body() const;

    /// @brief Get the author signature.
    ///
    /// The author is the person who originally wrote the code.
    [[nodiscard]] Signature author() const;

    /// @brief Get the committer signature.
    ///
    /// The committer is the person who created this commit object.
    /// May differ from author (e.g., after cherry-pick or rebase).
    [[nodiscard]] Signature committer() const;

    /// @brief Get the tree (file snapshot) for this commit.
    [[nodiscard]] Tree tree() const;

    // =========================================================================
    // Parent Access
    // =========================================================================

    /// @brief Get the number of parent commits.
    ///
    /// - 0 for root commits
    /// - 1 for normal commits
    /// - 2+ for merge commits
    [[nodiscard]] size_t parent_count() const;

    /// @brief Check if this is a merge commit (has more than one parent).
    [[nodiscard]] bool is_merge() const { return parent_count() > 1; }

    /// @brief Check if this is a root commit (has no parents).
    [[nodiscard]] bool is_root() const { return parent_count() == 0; }

    /// @brief Get a parent commit by index.
    ///
    /// @param index Parent index (0 = first parent, 1 = second parent for merges).
    /// @return The parent commit.
    /// @throws GitError if index is out of range.
    ///
    /// For merge commits:
    /// - Parent 0 is the branch you were on (target branch)
    /// - Parent 1 is the branch you merged in (source branch)
    [[nodiscard]] Commit parent(size_t index = 0) const;

    /// @brief Get all parent commits.
    [[nodiscard]] std::vector<Commit> parents() const;

    /// @brief Get a parent's OID without loading the full commit.
    ///
    /// @param index Parent index.
    /// @return The parent's OID.
    /// @throws GitError if index is out of range.
    [[nodiscard]] Oid parent_id(size_t index = 0) const;

    // =========================================================================
    // Diff Operations
    // =========================================================================

    /// @brief Get diff between this commit and its first parent.
    ///
    /// For root commits (no parents), diffs against an empty tree.
    /// For merge commits, this shows changes from the target branch perspective
    /// (i.e., what the merge brought in from the source branch).
    ///
    /// @return Diff showing changes introduced by this commit.
    ///
    /// @code{.cpp}
    /// auto diff = commit.diff_from_parent();
    /// for (auto delta : diff.deltas()) {
    ///     std::cout << delta.new_path << "\n";
    /// }
    /// @endcode
    [[nodiscard]] Diff diff_from_parent() const;

    /// @brief Get diff between this commit and a specific parent.
    ///
    /// @param parent_index Index of the parent to diff against.
    /// @return Diff showing changes from that parent to this commit.
    /// @throws GitError if parent_index is out of range.
    ///
    /// For merge commits:
    /// - `diff_from_parent(0)` shows what was merged in (from source branch)
    /// - `diff_from_parent(1)` shows what was already on target branch
    ///
    /// @code{.cpp}
    /// if (commit.is_merge()) {
    ///     auto from_target = commit.diff_from_parent(0);  // Changes vs target
    ///     auto from_source = commit.diff_from_parent(1);  // Changes vs source
    /// }
    /// @endcode
    [[nodiscard]] Diff diff_from_parent(size_t parent_index) const;

    /// @brief Get combined diff for merge commits showing only merge-unique changes.
    ///
    /// Returns a diff containing only changes that are unique to this merge
    /// commit - content that differs from ALL parents. This captures:
    /// - Conflict resolutions (manual merge decisions)
    /// - "Evil merge" additions (code added during merge not in either parent)
    /// - "Evil merge" deletions (code removed during merge that was in a parent)
    ///
    /// For non-merge commits, this is equivalent to diff_from_parent().
    ///
    /// @return Diff containing only merge-specific changes.
    ///
    /// @code{.cpp}
    /// if (commit.is_merge()) {
    ///     auto unique_changes = commit.merge_diff();
    ///     if (unique_changes.num_deltas() > 0) {
    ///         std::cout << "Merge has unique changes (possible evil merge):\n";
    ///         std::cout << unique_changes.full_patch();
    ///     }
    /// }
    /// @endcode
    [[nodiscard]] Diff merge_diff() const;

    /// @brief Check if this merge commit has any unique changes.
    ///
    /// A "clean" merge has no unique changes - all content comes directly
    /// from one of the parents. Returns false for non-merge commits.
    ///
    /// @return true if merge_diff() would return a non-empty diff.
    [[nodiscard]] bool has_merge_changes() const;

    /// @brief Get diff between this commit and another commit.
    ///
    /// @param other The commit to compare to.
    /// @return Diff from this commit's tree to other's tree.
    [[nodiscard]] Diff diff_to(const Commit& other) const;

    // =========================================================================
    // Relationships
    // =========================================================================

    /// @brief Check if this commit is an ancestor of another.
    ///
    /// @param descendant Potential descendant commit.
    /// @return true if this commit is reachable from descendant by following parents.
    [[nodiscard]] bool is_ancestor_of(const Commit& descendant) const;

    // =========================================================================
    // Signature Operations
    // =========================================================================

    /// @brief Check if this commit is signed.
    ///
    /// @return true if the commit has a cryptographic signature.
    [[nodiscard]] bool is_signed() const;

    /// @brief Get the raw signature from this commit.
    ///
    /// @return The ASCII-armored signature, or nullopt if not signed.
    [[nodiscard]] std::optional<std::string> signature() const;

    /// @brief Get the data that was signed.
    ///
    /// This is the commit content without the signature, which can be
    /// used to verify the signature.
    ///
    /// @return The signed data, or nullopt if not signed.
    [[nodiscard]] std::optional<std::string> signed_data() const;

    /// @brief Get the signature field name (usually "gpgsig").
    ///
    /// @return The field name, or nullopt if not signed.
    [[nodiscard]] std::optional<std::string> signature_field() const;

    // =========================================================================
    // Repository Access
    // =========================================================================

    /// @brief Get the repository this commit belongs to.
    [[nodiscard]] Repository& repository() const { return *repo_; }

    /// @brief Access the underlying libgit2 commit pointer.
    /// @warning For advanced use only.
    [[nodiscard]] git_commit* raw() const { return commit_.get(); }

private:
    friend class Repository;
    friend class CommitWalker;
    Commit(Repository* repo, detail::GitPtr<git_commit> commit);

    Repository* repo_;
    detail::GitPtr<git_commit> commit_;
};

/// @brief Iterator for walking commit history.
///
/// CommitWalker provides a flexible way to traverse commit history with
/// various sorting options and the ability to include/exclude branches.
///
/// @section walker_example Example Usage
/// @code{.cpp}
/// // Walk all commits from HEAD
/// auto walker = repo.walk_commits();
/// walker.push_head();
/// walker.sort(gitmanip::CommitWalker::SortOrder::Time);
///
/// for (auto commit : walker.walk()) {
///     std::cout << commit.id().short_id() << " " << commit.summary() << "\n";
/// }
///
/// // Walk commits on feature branch not in main
/// auto commits = gitmanip::CommitWalker::between(repo, "main", "feature");
/// @endcode
class CommitWalker {
public:
    /// @brief Sort order for commit traversal.
    enum class SortOrder {
        None,         ///< No specific order
        Topological,  ///< Parents before children
        Time,         ///< Newest first by commit time
        Reverse,      ///< Reverse the order (oldest first)
    };

    explicit CommitWalker(Repository& repo);
    ~CommitWalker();

    CommitWalker(const CommitWalker&) = delete;
    CommitWalker& operator=(const CommitWalker&) = delete;
    CommitWalker(CommitWalker&&) noexcept;
    CommitWalker& operator=(CommitWalker&&) noexcept;

    // =========================================================================
    // Configuration
    // =========================================================================

    /// @brief Add a commit as a starting point for the walk.
    CommitWalker& push(const Oid& oid);

    /// @brief Add HEAD as a starting point.
    CommitWalker& push_head();

    /// @brief Add a reference as a starting point.
    CommitWalker& push_ref(std::string_view refname);

    /// @brief Hide a commit and its ancestors from the walk.
    CommitWalker& hide(const Oid& oid);

    /// @brief Hide a reference and its ancestors from the walk.
    CommitWalker& hide_ref(std::string_view refname);

    /// @brief Set the sort order for traversal.
    CommitWalker& sort(SortOrder order);

    /// @brief Only follow first parents (useful for main branch history).
    CommitWalker& simplify_first_parent();

    // =========================================================================
    // Iteration
    // =========================================================================

    /// @brief Walk commits and collect into a vector.
    ///
    /// @code{.cpp}
    /// for (auto& commit : walker.walk()) {
    ///     std::cout << commit.summary() << "\n";
    /// }
    /// @endcode
    [[nodiscard]] std::vector<Commit> walk();

    /// @brief Get commits between two refs (from..to, exclusive..inclusive).
    ///
    /// Returns commits reachable from to_ref but not from from_ref.
    /// Useful for getting commits on a feature branch not yet in main.
    ///
    /// @param repo The repository.
    /// @param from_ref Base reference (exclusive).
    /// @param to_ref Target reference (inclusive).
    /// @return Vector of commits in topological order (oldest first).
    ///
    /// @code{.cpp}
    /// // Commits on feature not in main
    /// auto commits = CommitWalker::between(repo, "main", "feature");
    /// @endcode
    static std::vector<Commit> between(Repository& repo,
                                       std::string_view from_ref,
                                       std::string_view to_ref);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gitmanip
