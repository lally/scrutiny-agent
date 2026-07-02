#pragma once

/// @file repository.hpp
/// @brief Repository access and management.
///
/// This header provides the Repository class, which is the main entry point
/// for working with Git repositories. It handles opening, creating, and
/// querying repositories, as well as managing references and creating commits.
///
/// @section repo_example Basic Usage
/// @code{.cpp}
/// // Open an existing repository
/// auto repo = gitmanip::Repository::open("/path/to/repo");
///
/// // Or discover from a subdirectory
/// auto repo2 = gitmanip::Repository::discover("/path/to/repo/src/subdir");
///
/// // Or initialize a new one
/// auto repo3 = gitmanip::Repository::init("/path/to/new/repo");
///
/// // Query basic info
/// std::cout << "Path: " << repo.path() << "\n";
/// std::cout << "Is bare: " << repo.is_bare() << "\n";
/// std::cout << "HEAD: " << repo.head_oid().short_id() << "\n";
/// @endcode
///
/// @section repo_commits Working with Commits
/// @code{.cpp}
/// // Lookup a commit by OID, ref, or revision
/// auto commit1 = repo.lookup_commit(some_oid);
/// auto commit2 = repo.lookup_commit("HEAD");
/// auto commit3 = repo.lookup_commit("feature-branch~3");
///
/// // Get commits between two refs
/// auto commits = repo.commits_between("main", "feature");
///
/// // Walk all commits
/// for (auto commit : repo.walk_commits().push_head().walk()) {
///     std::cout << commit.summary() << "\n";
/// }
/// @endcode
///
/// @section repo_refs Working with References
/// @code{.cpp}
/// // List branches
/// for (const auto& name : repo.branch_names()) {
///     std::cout << name << "\n";
/// }
///
/// // Create a branch
/// auto commit = repo.lookup_commit("HEAD");
/// repo.create_branch("new-feature", commit);
///
/// // Update a reference
/// repo.update_ref("refs/heads/main", new_oid, "commit message");
/// @endcode

#include "commit.hpp"
#include "diff.hpp"
#include "tree.hpp"
#include "types.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gitmanip {

/// @brief Represents a Git repository.
///
/// Repository is the main class for interacting with a Git repository.
/// It provides methods for:
/// - Opening and creating repositories
/// - Looking up and creating commits
/// - Managing branches and references
/// - Creating diffs between trees
/// - Working with the index (staging area)
///
/// @note Repository objects are NOT thread-safe. Use separate instances
///       for concurrent access, or provide external synchronization.
///
/// @note Objects returned from Repository (Commit, Tree, Diff) maintain
///       a reference to the repository and must not outlive it.
class Repository {
public:
    /// @brief Open an existing repository at the given path.
    ///
    /// The path can point to either a bare repository or the working
    /// directory of a non-bare repository.
    ///
    /// @param path Path to the repository (or working directory).
    /// @return The opened repository.
    /// @throws RepositoryError if the path is not a valid repository.
    ///
    /// @code{.cpp}
    /// auto repo = gitmanip::Repository::open("/home/user/myproject");
    /// @endcode
    static Repository open(const std::filesystem::path& path);

    /// @brief Discover a repository by walking up from a path.
    ///
    /// Starting from the given path, walks up the directory tree until
    /// a Git repository is found. This is useful when you have a path
    /// to a file inside a repository but don't know the repository root.
    ///
    /// @param path Starting path for discovery.
    /// @return The discovered repository.
    /// @throws RepositoryError if no repository is found.
    ///
    /// @code{.cpp}
    /// // Works even from deep inside the repo
    /// auto repo = gitmanip::Repository::discover("/repo/src/lib/utils/helper.cpp");
    /// @endcode
    static Repository discover(const std::filesystem::path& path);

    /// @brief Initialize a new repository.
    ///
    /// Creates a new Git repository at the specified path.
    ///
    /// @param path Path where the repository should be created.
    /// @param bare If true, create a bare repository (no working directory).
    /// @return The newly created repository.
    /// @throws RepositoryError if initialization fails.
    ///
    /// @code{.cpp}
    /// auto repo = gitmanip::Repository::init("/path/to/new/repo");
    /// auto bare = gitmanip::Repository::init("/path/to/repo.git", true);
    /// @endcode
    static Repository init(const std::filesystem::path& path, bool bare = false);

    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;
    Repository(Repository&&) noexcept;
    Repository& operator=(Repository&&) noexcept;
    ~Repository();

    // =========================================================================
    // Repository Information
    // =========================================================================

    /// @brief Get the path to the .git directory.
    ///
    /// For bare repositories, this is the repository path itself.
    /// For non-bare repositories, this is the .git subdirectory.
    [[nodiscard]] std::filesystem::path path() const;

    /// @brief Get the working directory path.
    ///
    /// @return Working directory path, or empty path for bare repositories.
    [[nodiscard]] std::filesystem::path workdir() const;

    /// @brief Check if this is a bare repository.
    [[nodiscard]] bool is_bare() const;

    /// @brief Check if the repository is empty (no commits).
    [[nodiscard]] bool is_empty() const;

    /// @brief Check if HEAD is detached (not pointing to a branch).
    [[nodiscard]] bool is_head_detached() const;

    // =========================================================================
    // Reference Operations
    // =========================================================================

    /// @brief Resolve a reference name to an OID.
    ///
    /// @param refname Reference name (e.g., "HEAD", "refs/heads/main").
    /// @return The OID the reference points to.
    /// @throws GitError if the reference doesn't exist.
    [[nodiscard]] Oid resolve_ref(std::string_view refname) const;

    /// @brief Try to resolve a reference, returning nullopt if not found.
    ///
    /// @param refname Reference name.
    /// @return The OID, or nullopt if the reference doesn't exist.
    [[nodiscard]] std::optional<Oid> try_resolve_ref(std::string_view refname) const;

    /// @brief Get the OID that HEAD points to.
    [[nodiscard]] Oid head_oid() const;

    /// @brief Get the full reference name of HEAD.
    ///
    /// @return Reference name (e.g., "refs/heads/main"), or the OID
    ///         if HEAD is detached.
    [[nodiscard]] std::string head_name() const;

    /// @brief Get all branch names.
    ///
    /// @param local Include local branches.
    /// @param remote Include remote-tracking branches.
    /// @return Vector of branch names (without "refs/heads/" prefix).
    [[nodiscard]] std::vector<std::string> branch_names(bool local = true, bool remote = false) const;

    /// @brief Create a new branch.
    ///
    /// @param name Branch name (without "refs/heads/" prefix).
    /// @param target Commit the branch should point to.
    /// @param force If true, overwrite existing branch.
    /// @throws GitError if branch exists and force is false.
    void create_branch(std::string_view name, const Commit& target, bool force = false);

    /// @brief Delete a branch.
    ///
    /// @param name Branch name (without "refs/heads/" prefix).
    /// @throws GitError if the branch doesn't exist or is checked out.
    void delete_branch(std::string_view name);

    /// @brief Set HEAD to point to a reference.
    ///
    /// @param refname Full reference name (e.g., "refs/heads/main").
    void set_head(std::string_view refname);

    /// @brief Detach HEAD to point directly to a commit.
    ///
    /// @param oid Commit OID to point to.
    void set_head_detached(const Oid& oid);

    /// @brief Update a reference to point to a new OID.
    ///
    /// @param refname Full reference name.
    /// @param oid New target OID.
    /// @param message Optional reflog message.
    void update_ref(std::string_view refname, const Oid& oid,
                    std::optional<std::string_view> message = std::nullopt);

    // =========================================================================
    // Commit Operations
    // =========================================================================

    /// @brief Look up a commit by its OID.
    ///
    /// @param oid The commit's object ID.
    /// @return The commit.
    /// @throws GitError if the commit doesn't exist.
    [[nodiscard]] Commit lookup_commit(const Oid& oid) const;

    /// @brief Try to look up a commit, returning nullopt if not found.
    [[nodiscard]] std::optional<Commit> try_lookup_commit(const Oid& oid) const;

    /// @brief Look up a commit by revision string.
    ///
    /// Accepts any revision format that Git understands:
    /// - Full or abbreviated SHA
    /// - Reference names ("HEAD", "main", "refs/heads/feature")
    /// - Revision operators ("HEAD~3", "main^2", "HEAD@{yesterday}")
    ///
    /// @param rev Revision string.
    /// @return The commit.
    /// @throws GitError if the revision is invalid or doesn't resolve to a commit.
    ///
    /// @code{.cpp}
    /// auto head = repo.lookup_commit("HEAD");
    /// auto parent = repo.lookup_commit("HEAD~1");
    /// auto tag = repo.lookup_commit("v1.0.0");
    /// @endcode
    [[nodiscard]] Commit lookup_commit(std::string_view rev) const;

    /// @brief Create a new commit.
    ///
    /// Creates a commit with the given message, tree, and parents.
    /// The commit is not attached to any reference; use update_ref()
    /// to update a branch to point to it.
    ///
    /// @param message Commit message.
    /// @param tree Tree object for the commit.
    /// @param parents Parent commits (empty for root commit).
    /// @param author Author signature.
    /// @param committer Committer signature.
    /// @return OID of the new commit.
    ///
    /// @code{.cpp}
    /// auto sig = gitmanip::Signature::now("Name", "email@example.com");
    /// auto oid = repo.create_commit("My commit", tree, {&parent}, sig, sig);
    /// repo.update_ref("refs/heads/main", oid);
    /// @endcode
    [[nodiscard]] Oid create_commit(std::string_view message,
                                    const Tree& tree,
                                    std::span<const Commit* const> parents,
                                    const Signature& author,
                                    const Signature& committer);

    /// @brief Create a commit using the default signature from git config.
    [[nodiscard]] Oid create_commit(std::string_view message,
                                    const Tree& tree,
                                    std::span<const Commit* const> parents);

    // =========================================================================
    // Tree Operations
    // =========================================================================

    /// @brief Look up a tree by its OID.
    [[nodiscard]] Tree lookup_tree(const Oid& oid) const;

    /// @brief Get the empty tree (used as parent for root commits).
    [[nodiscard]] Tree empty_tree() const;

    // =========================================================================
    // Diff Operations
    // =========================================================================

    /// @brief Create a diff between two trees.
    ///
    /// @param old_tree The "old" tree (base for comparison).
    /// @param new_tree The "new" tree.
    /// @param options Diff options.
    /// @return Diff object containing the changes.
    [[nodiscard]] Diff diff_tree_to_tree(const Tree& old_tree, const Tree& new_tree,
                                         const DiffOptions& options = {}) const;

    /// @brief Create a diff between a tree and the index.
    [[nodiscard]] Diff diff_tree_to_index(const Tree& tree,
                                          const DiffOptions& options = {}) const;

    /// @brief Create a diff between the index and working directory.
    [[nodiscard]] Diff diff_index_to_workdir(const DiffOptions& options = {}) const;

    // =========================================================================
    // Blob Operations
    // =========================================================================

    /// @brief Create a blob from raw data.
    ///
    /// @param data Binary content for the blob.
    /// @return OID of the new blob.
    [[nodiscard]] Oid create_blob(std::span<const uint8_t> data);

    /// @brief Create a blob from string content.
    [[nodiscard]] Oid create_blob(std::string_view content);

    /// @brief Get blob content as binary data.
    [[nodiscard]] std::vector<uint8_t> blob_content(const Oid& oid) const;

    /// @brief Get blob content as a string.
    [[nodiscard]] std::string blob_content_string(const Oid& oid) const;

    // =========================================================================
    // Index (Staging Area) Operations
    // =========================================================================

    /// @brief Add a file to the index.
    ///
    /// @param path Path relative to repository root.
    void add_to_index(const std::filesystem::path& path);

    /// @brief Add all modified/new files to the index.
    void add_all_to_index();

    /// @brief Reset the index to match HEAD.
    void reset_index();

    /// @brief Write the current index as a tree object.
    ///
    /// This is typically used before creating a commit.
    [[nodiscard]] Tree write_index_as_tree();

    // =========================================================================
    // Checkout Operations
    // =========================================================================

    /// @brief Check out a tree to the working directory.
    ///
    /// @param tree Tree to check out.
    /// @param force If true, overwrite local changes.
    void checkout_tree(const Tree& tree, bool force = false);

    /// @brief Check out HEAD to the working directory.
    void checkout_head(bool force = false);

    // =========================================================================
    // Status Operations
    // =========================================================================

    /// @brief Check if the working directory is clean.
    ///
    /// @return true if there are no modified, staged, or untracked files.
    [[nodiscard]] bool is_workdir_clean() const;

    /// @brief Check if there are staged changes.
    [[nodiscard]] bool has_staged_changes() const;

    // =========================================================================
    // Signature Operations
    // =========================================================================

    /// @brief Get the default signature from git config.
    ///
    /// Reads user.name and user.email from the repository's git config.
    [[nodiscard]] Signature default_signature() const;

    // =========================================================================
    // Commit Walking
    // =========================================================================

    /// @brief Create a commit walker for iterating over history.
    ///
    /// @code{.cpp}
    /// auto walker = repo.walk_commits();
    /// walker.push_head();
    /// walker.sort(gitmanip::CommitWalker::SortOrder::Time);
    /// for (auto commit : walker.walk()) {
    ///     std::cout << commit.summary() << "\n";
    /// }
    /// @endcode
    [[nodiscard]] CommitWalker walk_commits();

    /// @brief Get commits between two references.
    ///
    /// Returns commits reachable from to_ref but not from from_ref,
    /// in topological order (oldest first).
    ///
    /// @param from_ref Base reference (exclusive).
    /// @param to_ref Target reference (inclusive).
    /// @return Vector of commits.
    ///
    /// @code{.cpp}
    /// // Get all commits on feature branch not in main
    /// auto commits = repo.commits_between("main", "feature");
    /// @endcode
    [[nodiscard]] std::vector<Commit> commits_between(std::string_view from_ref,
                                                      std::string_view to_ref);

    /// @brief Access the underlying libgit2 repository pointer.
    ///
    /// @warning This is for advanced use only. The returned pointer
    ///          is owned by this Repository object.
    [[nodiscard]] git_repository* raw() const { return repo_.get(); }

private:
    explicit Repository(detail::GitPtr<git_repository> repo);

    detail::GitPtr<git_repository> repo_;
};

/// @brief RAII wrapper for libgit2 library initialization.
///
/// This class is used internally and ensures libgit2 is properly
/// initialized before use and shut down when no longer needed.
/// You typically don't need to use this directly.
class LibGit2Init {
public:
    LibGit2Init();
    ~LibGit2Init();
    LibGit2Init(const LibGit2Init&) = delete;
    LibGit2Init& operator=(const LibGit2Init&) = delete;

private:
    static int ref_count_;
};

}  // namespace gitmanip
