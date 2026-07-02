#pragma once

/// @file refs.hpp
/// @brief Git references: remotes, branches, and tags.
///
/// This header provides types and functions for working with Git references:
/// - Remotes: Named remote repositories (origin, upstream, etc.)
/// - Branches: Local and remote-tracking branches
/// - Tags: Lightweight and annotated tags
///
/// @section refs_example Example Usage
/// @code{.cpp}
/// auto repo = gitmanip::Repository::open("/path/to/repo");
///
/// // List remotes
/// for (const auto& remote : gitmanip::Remote::list(repo)) {
///     std::cout << remote.name << ": " << remote.url << "\n";
/// }
///
/// // List branches with details
/// for (const auto& branch : gitmanip::Branch::list(repo)) {
///     std::cout << branch.name << " -> " << branch.target.short_id();
///     if (branch.is_head) std::cout << " (HEAD)";
///     std::cout << "\n";
/// }
///
/// // List tags
/// for (const auto& tag : gitmanip::Tag::list(repo)) {
///     std::cout << tag.name << " -> " << tag.target.short_id();
///     if (tag.is_annotated) {
///         std::cout << " [" << tag.message << "]";
///     }
///     std::cout << "\n";
/// }
/// @endcode

#include "types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace gitmanip {

class Repository;
class Commit;
class Signer;

/// @brief Information about a Git remote.
struct RemoteInfo {
    /// @brief Name of the remote (e.g., "origin", "upstream").
    std::string name;

    /// @brief Fetch URL for the remote.
    std::string url;

    /// @brief Push URL (may differ from fetch URL).
    std::string push_url;

    /// @brief Refspecs for fetching.
    std::vector<std::string> fetch_refspecs;

    /// @brief Refspecs for pushing.
    std::vector<std::string> push_refspecs;
};

/// @brief Operations on Git remotes.
///
/// Remotes are named references to other repositories, typically used for
/// fetching and pushing changes.
class Remote {
public:
    /// @brief List all remotes in the repository.
    ///
    /// @param repo The repository.
    /// @return Vector of remote information.
    ///
    /// @code{.cpp}
    /// for (const auto& remote : gitmanip::Remote::list(repo)) {
    ///     std::cout << remote.name << ": " << remote.url << "\n";
    /// }
    /// @endcode
    [[nodiscard]] static std::vector<RemoteInfo> list(Repository& repo);

    /// @brief Get information about a specific remote.
    ///
    /// @param repo The repository.
    /// @param name Remote name.
    /// @return Remote information, or nullopt if not found.
    [[nodiscard]] static std::optional<RemoteInfo> get(Repository& repo, std::string_view name);

    /// @brief Get just the remote names.
    ///
    /// @param repo The repository.
    /// @return Vector of remote names.
    [[nodiscard]] static std::vector<std::string> names(Repository& repo);

    /// @brief Check if a remote exists.
    ///
    /// @param repo The repository.
    /// @param name Remote name.
    /// @return true if the remote exists.
    [[nodiscard]] static bool exists(Repository& repo, std::string_view name);

    /// @brief Add a new remote.
    ///
    /// @param repo The repository.
    /// @param name Name for the remote.
    /// @param url URL of the remote repository.
    /// @throws GitError if the remote already exists.
    static void add(Repository& repo, std::string_view name, std::string_view url);

    /// @brief Remove a remote.
    ///
    /// @param repo The repository.
    /// @param name Remote name to remove.
    /// @throws GitError if the remote doesn't exist.
    static void remove(Repository& repo, std::string_view name);

    /// @brief Rename a remote.
    ///
    /// @param repo The repository.
    /// @param old_name Current name.
    /// @param new_name New name.
    /// @throws GitError if the remote doesn't exist or new name is taken.
    static void rename(Repository& repo, std::string_view old_name, std::string_view new_name);

    /// @brief Set the URL for a remote.
    ///
    /// @param repo The repository.
    /// @param name Remote name.
    /// @param url New URL.
    static void set_url(Repository& repo, std::string_view name, std::string_view url);

    /// @brief Set the push URL for a remote.
    ///
    /// @param repo The repository.
    /// @param name Remote name.
    /// @param url New push URL.
    static void set_push_url(Repository& repo, std::string_view name, std::string_view url);
};

/// @brief Information about a Git branch.
struct BranchInfo {
    /// @brief Branch name (without refs/heads/ or refs/remotes/ prefix).
    std::string name;

    /// @brief Full reference name (e.g., "refs/heads/main").
    std::string refname;

    /// @brief OID of the commit this branch points to.
    Oid target;

    /// @brief Whether this is a remote-tracking branch.
    bool is_remote = false;

    /// @brief Whether this is the current HEAD branch.
    bool is_head = false;

    /// @brief Upstream branch name (if configured).
    std::optional<std::string> upstream;

    /// @brief Remote name for remote-tracking branches.
    std::optional<std::string> remote_name;
};

/// @brief Operations on Git branches.
///
/// Branches are references that point to commits and typically move forward
/// as new commits are made.
class Branch {
public:
    /// @brief List all branches.
    ///
    /// @param repo The repository.
    /// @param local Include local branches (default: true).
    /// @param remote Include remote-tracking branches (default: false).
    /// @return Vector of branch information.
    ///
    /// @code{.cpp}
    /// // List all branches (local and remote)
    /// for (const auto& branch : gitmanip::Branch::list(repo, true, true)) {
    ///     std::cout << (branch.is_remote ? "remote: " : "local: ")
    ///               << branch.name << "\n";
    /// }
    /// @endcode
    [[nodiscard]] static std::vector<BranchInfo> list(Repository& repo,
                                                      bool local = true,
                                                      bool remote = false);

    /// @brief Get information about a specific branch.
    ///
    /// @param repo The repository.
    /// @param name Branch name.
    /// @param is_remote Whether to look for a remote-tracking branch.
    /// @return Branch information, or nullopt if not found.
    [[nodiscard]] static std::optional<BranchInfo> get(Repository& repo,
                                                       std::string_view name,
                                                       bool is_remote = false);

    /// @brief Get the current branch (what HEAD points to).
    ///
    /// @param repo The repository.
    /// @return Current branch info, or nullopt if HEAD is detached.
    [[nodiscard]] static std::optional<BranchInfo> current(Repository& repo);

    /// @brief Check if a branch exists.
    ///
    /// @param repo The repository.
    /// @param name Branch name.
    /// @param is_remote Check remote-tracking branches instead of local.
    /// @return true if the branch exists.
    [[nodiscard]] static bool exists(Repository& repo, std::string_view name,
                                     bool is_remote = false);

    /// @brief Get the upstream branch for a local branch.
    ///
    /// @param repo The repository.
    /// @param name Local branch name.
    /// @return Upstream branch info, or nullopt if not configured.
    [[nodiscard]] static std::optional<BranchInfo> upstream(Repository& repo,
                                                            std::string_view name);

    /// @brief Set the upstream branch for a local branch.
    ///
    /// @param repo The repository.
    /// @param branch_name Local branch name.
    /// @param upstream_name Upstream branch name (e.g., "origin/main").
    static void set_upstream(Repository& repo, std::string_view branch_name,
                             std::string_view upstream_name);

    /// @brief Unset the upstream branch.
    ///
    /// @param repo The repository.
    /// @param branch_name Local branch name.
    static void unset_upstream(Repository& repo, std::string_view branch_name);
};

/// @brief Information about a Git tag.
struct TagInfo {
    /// @brief Tag name (without refs/tags/ prefix).
    std::string name;

    /// @brief Full reference name (e.g., "refs/tags/v1.0.0").
    std::string refname;

    /// @brief OID of the tagged object (commit for lightweight, tag object for annotated).
    Oid target;

    /// @brief OID of the commit this tag ultimately points to.
    /// For lightweight tags, same as target. For annotated tags, the tagged commit.
    Oid commit_target;

    /// @brief Whether this is an annotated tag (vs lightweight).
    bool is_annotated = false;

    /// @brief Whether this tag is signed (only for annotated tags).
    bool is_signed = false;

    /// @brief Tag message (only for annotated tags).
    std::optional<std::string> message;

    /// @brief Tagger signature (only for annotated tags).
    std::optional<Signature> tagger;

    /// @brief Cryptographic signature (only for signed tags).
    std::optional<std::string> gpg_signature;
};

/// @brief Operations on Git tags.
///
/// Tags are references that point to specific commits (or other objects).
/// They come in two forms:
/// - Lightweight tags: Simple references to commits
/// - Annotated tags: Full objects with message, tagger, and optional signature
class Tag {
public:
    /// @brief List all tags.
    ///
    /// @param repo The repository.
    /// @param pattern Optional glob pattern to filter tags (e.g., "v1.*").
    /// @return Vector of tag information.
    ///
    /// @code{.cpp}
    /// // List all tags
    /// for (const auto& tag : gitmanip::Tag::list(repo)) {
    ///     std::cout << tag.name;
    ///     if (tag.is_annotated) {
    ///         std::cout << " (annotated): " << *tag.message;
    ///     }
    ///     std::cout << "\n";
    /// }
    ///
    /// // List only v1.x tags
    /// for (const auto& tag : gitmanip::Tag::list(repo, "v1.*")) {
    ///     std::cout << tag.name << "\n";
    /// }
    /// @endcode
    [[nodiscard]] static std::vector<TagInfo> list(Repository& repo,
                                                   std::optional<std::string_view> pattern = std::nullopt);

    /// @brief Get just the tag names.
    ///
    /// @param repo The repository.
    /// @param pattern Optional glob pattern to filter.
    /// @return Vector of tag names.
    [[nodiscard]] static std::vector<std::string> names(Repository& repo,
                                                        std::optional<std::string_view> pattern = std::nullopt);

    /// @brief Get information about a specific tag.
    ///
    /// @param repo The repository.
    /// @param name Tag name.
    /// @return Tag information, or nullopt if not found.
    [[nodiscard]] static std::optional<TagInfo> get(Repository& repo, std::string_view name);

    /// @brief Check if a tag exists.
    ///
    /// @param repo The repository.
    /// @param name Tag name.
    /// @return true if the tag exists.
    [[nodiscard]] static bool exists(Repository& repo, std::string_view name);

    /// @brief Create a lightweight tag.
    ///
    /// @param repo The repository.
    /// @param name Tag name.
    /// @param target Commit to tag.
    /// @param force Overwrite existing tag.
    /// @throws GitError if tag exists and force is false.
    ///
    /// @code{.cpp}
    /// auto commit = repo.lookup_commit("HEAD");
    /// gitmanip::Tag::create_lightweight(repo, "v1.0.0", commit);
    /// @endcode
    static void create_lightweight(Repository& repo, std::string_view name,
                                   const Commit& target, bool force = false);

    /// @brief Create an annotated tag.
    ///
    /// @param repo The repository.
    /// @param name Tag name.
    /// @param target Commit to tag.
    /// @param message Tag message.
    /// @param tagger Tagger signature (uses default if not provided).
    /// @param force Overwrite existing tag.
    /// @throws GitError if tag exists and force is false.
    ///
    /// @code{.cpp}
    /// auto commit = repo.lookup_commit("HEAD");
    /// gitmanip::Tag::create_annotated(repo, "v1.0.0", commit, "Release 1.0.0");
    /// @endcode
    static void create_annotated(Repository& repo, std::string_view name,
                                 const Commit& target, std::string_view message,
                                 std::optional<Signature> tagger = std::nullopt,
                                 bool force = false);

    /// @brief Create a signed annotated tag.
    ///
    /// @param repo The repository.
    /// @param name Tag name.
    /// @param target Commit to tag.
    /// @param message Tag message.
    /// @param signer Signer to use for signing.
    /// @param tagger Tagger signature (uses default if not provided).
    /// @param force Overwrite existing tag.
    /// @throws GitError if tag exists and force is false, or signing fails.
    ///
    /// @code{.cpp}
    /// auto config = gitmanip::SigningConfig::from_repo(repo);
    /// auto signer = gitmanip::Signer::create(config);
    /// auto commit = repo.lookup_commit("HEAD");
    /// gitmanip::Tag::create_signed(repo, "v1.0.0", commit, "Release 1.0.0", *signer);
    /// @endcode
    static void create_signed(Repository& repo, std::string_view name,
                              const Commit& target, std::string_view message,
                              Signer& signer,
                              std::optional<Signature> tagger = std::nullopt,
                              bool force = false);

    /// @brief Delete a tag.
    ///
    /// @param repo The repository.
    /// @param name Tag name.
    /// @throws GitError if the tag doesn't exist.
    static void remove(Repository& repo, std::string_view name);

    /// @brief Verify a signed tag.
    ///
    /// @param repo The repository.
    /// @param name Tag name.
    /// @param signer Signer to use for verification.
    /// @return true if the tag has a valid signature.
    /// @throws GitError if the tag doesn't exist or is not signed.
    [[nodiscard]] static bool verify(Repository& repo, std::string_view name, Signer& signer);

    /// @brief Get tags that point to a specific commit.
    ///
    /// @param repo The repository.
    /// @param commit_oid OID of the commit.
    /// @return Vector of tag names pointing to this commit.
    [[nodiscard]] static std::vector<std::string> pointing_to(Repository& repo, const Oid& commit_oid);
};

}  // namespace gitmanip
