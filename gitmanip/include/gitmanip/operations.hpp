#pragma once

#include "rebase_plan.hpp"
#include "tree.hpp"
#include "types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace gitmanip {

class Repository;
class Commit;
class RebasePlan;
class Signer;

// Result of a rebase operation
struct RebaseResult {
    bool success = false;
    Oid new_head;
    std::vector<std::pair<Oid, Oid>> commit_mapping;  // old -> new
    std::vector<std::string> warnings;

    // In case of conflict
    std::optional<size_t> conflicting_action_index;
    std::optional<std::string> conflict_message;
};

// Callback for monitoring rebase progress
struct RebaseProgress {
    size_t current_action = 0;
    size_t total_actions = 0;
    const RebaseAction* action = nullptr;
    std::optional<Oid> new_oid;  // Set after action completes
};

using RebaseProgressCallback = std::function<bool(const RebaseProgress&)>;

// Main operations executor
class Operations {
public:
    explicit Operations(Repository& repo);
    ~Operations();

    Operations(const Operations&) = delete;
    Operations& operator=(const Operations&) = delete;
    Operations(Operations&&) noexcept;
    Operations& operator=(Operations&&) noexcept;

    // Execute a rebase plan
    [[nodiscard]] RebaseResult execute(const RebasePlan& plan,
                                       RebaseProgressCallback progress = nullptr);

    // Convenience: reword a single commit
    [[nodiscard]] Oid reword_commit(const Commit& commit, std::string_view new_message);

    // Convenience: squash commits together
    [[nodiscard]] Oid squash_commits(std::span<const Commit* const> commits,
                                     std::string_view combined_message);

    // Convenience: drop a commit (rebase its children onto its parent)
    [[nodiscard]] RebaseResult drop_commit(const Commit& commit);

    // Convenience: split a commit into multiple
    [[nodiscard]] std::vector<Oid> split_commit(
        const Commit& commit,
        std::function<std::vector<std::pair<std::string, Tree>>(const Commit&)> splitter);

    // Amend the current HEAD commit
    [[nodiscard]] Oid amend_head(std::optional<std::string_view> new_message = std::nullopt,
                                 std::optional<Tree> new_tree = std::nullopt,
                                 std::optional<Signature> new_author = std::nullopt);

    // Cherry-pick a commit onto HEAD
    [[nodiscard]] Oid cherry_pick(const Commit& commit,
                                  std::optional<std::string_view> new_message = std::nullopt);

    // Revert a commit
    [[nodiscard]] Oid revert(const Commit& commit,
                             std::optional<std::string_view> message = std::nullopt);

    // Options
    Operations& set_update_refs(bool update);  // Update branch refs during rebase
    Operations& set_force(bool force);         // Allow operations on dirty workdir
    Operations& set_dry_run(bool dry_run);     // Don't actually modify anything

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Builder for creating modified commits
class CommitBuilder {
public:
    CommitBuilder(Repository& repo, const Commit& base);
    ~CommitBuilder();

    CommitBuilder(const CommitBuilder&) = delete;
    CommitBuilder& operator=(const CommitBuilder&) = delete;
    CommitBuilder(CommitBuilder&&) noexcept;
    CommitBuilder& operator=(CommitBuilder&&) noexcept;

    // Modify message
    CommitBuilder& set_message(std::string_view message);

    // Modify author
    CommitBuilder& set_author(const Signature& author);

    // Modify committer
    CommitBuilder& set_committer(const Signature& committer);

    // Modify tree
    CommitBuilder& set_tree(const Tree& tree);

    // Modify tree using builder function
    CommitBuilder& modify_tree(std::function<void(TreeBuilder&)> modifier);

    // Add/remove file changes
    CommitBuilder& add_file(std::string_view path, std::span<const uint8_t> content);
    CommitBuilder& add_file(std::string_view path, std::string_view content);
    CommitBuilder& remove_file(std::string_view path);
    CommitBuilder& modify_file(std::string_view path,
                               std::function<std::string(std::string_view)> modifier);

    // Set parents
    CommitBuilder& set_parents(std::span<const Commit* const> parents);
    CommitBuilder& set_parent(const Commit& parent);

    // Signing
    /// @brief Set a signer to create a signed commit.
    ///
    /// When a signer is set, build() will create a signed commit using
    /// the signer's key (GPG or SSH).
    ///
    /// @param signer The signer to use.
    /// @return *this for chaining.
    ///
    /// @code{.cpp}
    /// auto config = gitmanip::SigningConfig::from_repo(repo);
    /// auto signer = gitmanip::Signer::create(config);
    ///
    /// gitmanip::CommitBuilder builder(repo, base_commit);
    /// builder.set_message("Signed commit")
    ///        .set_signer(signer.get())
    ///        .build();
    /// @endcode
    CommitBuilder& set_signer(Signer* signer);

    // Build the new commit
    [[nodiscard]] Oid build();
    [[nodiscard]] Commit build_commit();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gitmanip
