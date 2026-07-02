#include "gitmanip/operations.hpp"
#include "gitmanip/commit.hpp"
#include "gitmanip/diff.hpp"
#include "gitmanip/error.hpp"
#include "gitmanip/rebase_plan.hpp"
#include "gitmanip/repository.hpp"
#include "gitmanip/signing.hpp"
#include "gitmanip/tree.hpp"

#include <git2.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <set>

namespace gitmanip {

// Operations implementation
struct Operations::Impl {
    Repository* repo;
    bool update_refs = true;
    bool force = false;
    bool dry_run = false;

    // Apply change modifications to a tree
    Tree apply_modifications(const Tree& base_tree,
                             const std::vector<ChangeModification>& mods);

    // Cherry-pick changes from one commit onto a new parent
    Tree cherry_pick_tree(const Commit& commit, const Commit& new_parent);
};

Tree Operations::Impl::apply_modifications(const Tree& base_tree,
                                            const std::vector<ChangeModification>& mods) {
    if (mods.empty()) {
        return repo->lookup_tree(base_tree.id());
    }

    TreeBuilder builder(*repo, base_tree);

    for (const auto& mod : mods) {
        switch (mod.type) {
            case ChangeModification::Type::KeepAll:
                // Nothing to do
                break;

            case ChangeModification::Type::DropFile:
                for (const auto& path : mod.paths) {
                    builder.remove(path);
                }
                break;

            case ChangeModification::Type::KeepOnlyFiles: {
                // Get all files in tree
                std::set<std::string> to_keep(mod.paths.begin(), mod.paths.end());
                std::vector<std::string> to_remove;

                for (auto entry : base_tree.entries()) {
                    if (to_keep.find(entry.name) == to_keep.end()) {
                        to_remove.push_back(entry.name);
                    }
                }

                for (const auto& name : to_remove) {
                    builder.remove(name);
                }
                break;
            }

            case ChangeModification::Type::InsertFile:
            case ChangeModification::Type::ReplaceContent:
                if (!mod.paths.empty() && mod.content) {
                    builder.insert_blob(mod.paths[0], *mod.content);
                }
                break;

            case ChangeModification::Type::ModifyHunk:
                // TODO: Implement hunk-level modification
                // This requires parsing the diff and reconstructing the file
                spdlog::warn("Hunk-level modification not yet implemented");
                break;
        }
    }

    return builder.build();
}

Tree Operations::Impl::cherry_pick_tree(const Commit& commit, const Commit& new_parent) {
    // Get the trees
    Tree commit_tree = commit.tree();
    Tree new_parent_tree = new_parent.tree();

    // Get the original parent tree (if exists)
    if (commit.parent_count() == 0) {
        // Root commit - just use the commit's tree
        return repo->lookup_tree(commit_tree.id());
    }

    Tree original_parent_tree = commit.parent(0).tree();

    // Three-way merge
    git_index* index = nullptr;
    int error = git_merge_trees(&index, repo->raw(),
                                original_parent_tree.raw(),
                                new_parent_tree.raw(),
                                commit_tree.raw(),
                                nullptr);
    detail::check_libgit2_error(error, "merging trees for cherry-pick");
    detail::GitPtr<git_index> index_ptr(index);

    // Check for conflicts
    if (git_index_has_conflicts(index)) {
        throw ConflictError("Cherry-pick resulted in conflicts");
    }

    // Write the merged index as a tree
    git_oid tree_oid;
    error = git_index_write_tree_to(&tree_oid, index, repo->raw());
    detail::check_libgit2_error(error, "writing merged tree");

    return repo->lookup_tree(Oid(&tree_oid));
}

Operations::Operations(Repository& repo)
    : impl_(std::make_unique<Impl>()) {
    impl_->repo = &repo;
}

Operations::~Operations() = default;
Operations::Operations(Operations&&) noexcept = default;
Operations& Operations::operator=(Operations&&) noexcept = default;

RebaseResult Operations::execute(const RebasePlan& plan, RebaseProgressCallback progress) {
    RebaseResult result;
    result.success = false;

    // Validate the plan
    auto errors = plan.validation_errors();
    if (!errors.empty()) {
        result.conflict_message = fmt::format("Plan validation failed: {}", errors[0]);
        return result;
    }

    if (plan.size() == 0) {
        result.success = true;
        result.new_head = plan.onto();
        return result;
    }

    try {
        Commit current_parent = impl_->repo->lookup_commit(plan.onto());
        std::string pending_message;
        std::optional<Signature> pending_author;
        Tree pending_tree = current_parent.tree();
        std::vector<std::pair<Oid, Oid>> pending_squash_mapping;

        for (size_t i = 0; i < plan.size(); ++i) {
            const auto& action = plan.action(i);

            if (progress) {
                RebaseProgress prog;
                prog.current_action = i;
                prog.total_actions = plan.size();
                prog.action = &action;

                if (!progress(prog)) {
                    result.conflict_message = "Rebase cancelled by user";
                    return result;
                }
            }

            if (action.type == RebaseAction::Type::Drop) {
                // Skip this commit entirely
                continue;
            }

            // Get the original commit
            Commit original = impl_->repo->lookup_commit(action.original_oid);

            // Determine the new tree
            Tree new_tree = impl_->cherry_pick_tree(original, current_parent);

            // Apply any change modifications
            const auto& mods = plan.get_modifications(i);
            if (!mods.empty()) {
                new_tree = impl_->apply_modifications(new_tree, mods);
            }

            // Determine message and author based on action type
            std::string message;
            Signature author = original.author();

            switch (action.type) {
                case RebaseAction::Type::Pick:
                    message = original.message();
                    break;

                case RebaseAction::Type::Reword:
                    if (action.new_message) {
                        message = *action.new_message;
                    } else {
                        message = original.message();
                    }
                    break;

                case RebaseAction::Type::Edit:
                    // In a real implementation, we'd pause here
                    // For now, just treat like pick
                    message = original.message();
                    result.warnings.push_back(
                        fmt::format("Edit action at {} treated as pick", action.original_oid.short_id()));
                    break;

                case RebaseAction::Type::Squash:
                    // Combine with pending
                    if (pending_message.empty()) {
                        pending_message = current_parent.message();
                        pending_author = current_parent.author();
                        pending_tree = current_parent.tree();
                    }

                    if (action.new_message) {
                        pending_message = *action.new_message;
                    } else {
                        // Combine messages
                        pending_message += "\n\n" + original.message();
                    }

                    // Merge trees
                    pending_tree = impl_->repo->lookup_tree(new_tree.id());
                    pending_squash_mapping.push_back({action.original_oid, Oid()});

                    // Check if next action is also squash/fixup
                    if (i + 1 < plan.size()) {
                        auto next_type = plan.action(i + 1).type;
                        if (next_type == RebaseAction::Type::Squash ||
                            next_type == RebaseAction::Type::Fixup) {
                            continue;  // Don't create commit yet
                        }
                    }

                    // Create the squashed commit
                    message = pending_message;
                    if (pending_author) {
                        author = *pending_author;
                    }
                    new_tree = std::move(pending_tree);
                    pending_message.clear();
                    pending_author.reset();
                    break;

                case RebaseAction::Type::Fixup:
                    // Like squash but discard message
                    if (pending_message.empty()) {
                        pending_message = current_parent.message();
                        pending_author = current_parent.author();
                        pending_tree = current_parent.tree();
                    }

                    pending_tree = impl_->repo->lookup_tree(new_tree.id());
                    pending_squash_mapping.push_back({action.original_oid, Oid()});

                    // Check if next action is also squash/fixup
                    if (i + 1 < plan.size()) {
                        auto next_type = plan.action(i + 1).type;
                        if (next_type == RebaseAction::Type::Squash ||
                            next_type == RebaseAction::Type::Fixup) {
                            continue;
                        }
                    }

                    message = pending_message;
                    if (pending_author) {
                        author = *pending_author;
                    }
                    new_tree = std::move(pending_tree);
                    pending_message.clear();
                    pending_author.reset();
                    break;

                case RebaseAction::Type::Drop:
                    // Already handled above
                    break;
            }

            if (impl_->dry_run) {
                spdlog::info("Would create commit: {} -> new parent {}",
                             action.original_oid.short_id(),
                             current_parent.id().short_id());
                continue;
            }

            // Create the new commit
            const Commit* parent_ptr = &current_parent;
            Oid new_oid = impl_->repo->create_commit(
                message,
                new_tree,
                std::span<const Commit* const>(&parent_ptr, 1),
                author,
                impl_->repo->default_signature());

            result.commit_mapping.push_back({action.original_oid, new_oid});

            // Update mapping for squashed commits
            for (auto& [old_oid, new_oid_ref] : pending_squash_mapping) {
                new_oid_ref = new_oid;
                result.commit_mapping.push_back({old_oid, new_oid});
            }
            pending_squash_mapping.clear();

            // Move to next parent
            current_parent = impl_->repo->lookup_commit(new_oid);

            if (progress) {
                RebaseProgress prog;
                prog.current_action = i;
                prog.total_actions = plan.size();
                prog.action = &action;
                prog.new_oid = new_oid;
                progress(prog);
            }
        }

        result.success = true;
        result.new_head = current_parent.id();

    } catch (const ConflictError& e) {
        result.conflict_message = e.message();
    } catch (const GitError& e) {
        result.conflict_message = e.message();
    }

    return result;
}

Oid Operations::reword_commit(const Commit& commit, std::string_view new_message) {
    // If this is HEAD, we can amend
    if (commit.id() == impl_->repo->head_oid()) {
        return amend_head(new_message);
    }

    // Otherwise, need to rebase
    if (commit.parent_count() == 0) {
        throw GitError(ErrorCode::InvalidOperation,
                       "Cannot reword root commit without full rebase");
    }

    // Create a plan with just this commit
    RebasePlan plan(*impl_->repo, commit.parent_id(0));
    plan.add(RebaseAction::reword(commit, std::string(new_message)));

    auto result = execute(plan);
    if (!result.success) {
        throw GitError(ErrorCode::InvalidOperation,
                       result.conflict_message.value_or("Reword failed"));
    }

    return result.new_head;
}

Oid Operations::squash_commits(std::span<const Commit* const> commits,
                                std::string_view combined_message) {
    if (commits.empty()) {
        throw GitError(ErrorCode::InvalidArgument, "No commits to squash");
    }

    if (commits.size() == 1) {
        // Just reword
        return reword_commit(*commits[0], combined_message);
    }

    // Find common ancestor
    const Commit& first = *commits[0];
    if (first.parent_count() == 0) {
        throw GitError(ErrorCode::InvalidOperation,
                       "Cannot squash commits including root commit");
    }

    Oid onto = first.parent_id(0);
    RebasePlan plan(*impl_->repo, onto);

    // First commit becomes pick (or reword if we have a message)
    if (combined_message.empty()) {
        plan.add(RebaseAction::pick(first));
    } else {
        plan.add(RebaseAction::reword(first, std::string(combined_message)));
    }

    // Rest become fixup
    for (size_t i = 1; i < commits.size(); ++i) {
        plan.add(RebaseAction::fixup(*commits[i]));
    }

    auto result = execute(plan);
    if (!result.success) {
        throw GitError(ErrorCode::InvalidOperation,
                       result.conflict_message.value_or("Squash failed"));
    }

    return result.new_head;
}

RebaseResult Operations::drop_commit(const Commit& commit) {
    if (commit.parent_count() == 0) {
        throw GitError(ErrorCode::InvalidOperation, "Cannot drop root commit");
    }

    Oid onto = commit.parent_id(0);
    RebasePlan plan(*impl_->repo, onto);
    plan.add(RebaseAction::drop(commit));

    return execute(plan);
}

std::vector<Oid> Operations::split_commit(
    const Commit& commit,
    std::function<std::vector<std::pair<std::string, Tree>>(const Commit&)> splitter) {

    auto splits = splitter(commit);
    if (splits.empty()) {
        throw GitError(ErrorCode::InvalidArgument, "Splitter returned no commits");
    }

    std::vector<Oid> result;

    Commit parent = commit.parent_count() > 0
        ? commit.parent(0)
        : impl_->repo->lookup_commit(impl_->repo->empty_tree().id());

    for (const auto& [message, tree] : splits) {
        const Commit* parent_ptr = &parent;
        Oid new_oid = impl_->repo->create_commit(
            message,
            tree,
            std::span<const Commit* const>(&parent_ptr, 1),
            commit.author(),
            impl_->repo->default_signature());

        result.push_back(new_oid);
        parent = impl_->repo->lookup_commit(new_oid);
    }

    return result;
}

Oid Operations::amend_head(std::optional<std::string_view> new_message,
                            std::optional<Tree> new_tree,
                            std::optional<Signature> new_author) {
    Commit head = impl_->repo->lookup_commit(impl_->repo->head_oid());

    std::string message = new_message ? std::string(*new_message) : head.message();
    Tree tree = new_tree ? std::move(*new_tree) : std::move(head.tree());
    Signature author = new_author ? *new_author : head.author();

    std::vector<const Commit*> parent_ptrs;
    auto parents = head.parents();
    parent_ptrs.reserve(parents.size());
    for (const auto& p : parents) {
        parent_ptrs.push_back(&p);
    }

    Oid new_oid = impl_->repo->create_commit(
        message,
        tree,
        std::span<const Commit* const>(parent_ptrs),
        author,
        impl_->repo->default_signature());

    if (impl_->update_refs) {
        std::string head_ref = impl_->repo->head_name();
        impl_->repo->update_ref(head_ref, new_oid, "amend commit");
    }

    return new_oid;
}

Oid Operations::cherry_pick(const Commit& commit,
                             std::optional<std::string_view> new_message) {
    Commit head = impl_->repo->lookup_commit(impl_->repo->head_oid());
    Tree new_tree = impl_->cherry_pick_tree(commit, head);

    std::string message = new_message ? std::string(*new_message) : commit.message();

    const Commit* parent_ptr = &head;
    return impl_->repo->create_commit(
        message,
        new_tree,
        std::span<const Commit* const>(&parent_ptr, 1),
        commit.author(),
        impl_->repo->default_signature());
}

Oid Operations::revert(const Commit& commit, std::optional<std::string_view> message) {
    Commit head = impl_->repo->lookup_commit(impl_->repo->head_oid());

    if (commit.parent_count() == 0) {
        throw GitError(ErrorCode::InvalidOperation, "Cannot revert root commit");
    }

    // Get the trees for three-way merge
    Tree commit_tree = commit.tree();
    Tree parent_tree = commit.parent(0).tree();
    Tree head_tree = head.tree();

    // Merge: parent is "ours" (what we want), commit is what we're reverting from
    git_index* index = nullptr;
    int error = git_merge_trees(&index, impl_->repo->raw(),
                                commit_tree.raw(),
                                head_tree.raw(),
                                parent_tree.raw(),
                                nullptr);
    detail::check_libgit2_error(error, "merging trees for revert");
    detail::GitPtr<git_index> index_ptr(index);

    if (git_index_has_conflicts(index)) {
        throw ConflictError("Revert resulted in conflicts");
    }

    git_oid tree_oid;
    error = git_index_write_tree_to(&tree_oid, index, impl_->repo->raw());
    detail::check_libgit2_error(error, "writing reverted tree");

    Tree new_tree = impl_->repo->lookup_tree(Oid(&tree_oid));

    std::string msg = message
        ? std::string(*message)
        : fmt::format("Revert \"{}\"\n\nThis reverts commit {}.",
                      commit.summary(), commit.id().to_string());

    const Commit* parent_ptr = &head;
    return impl_->repo->create_commit(
        msg,
        new_tree,
        std::span<const Commit* const>(&parent_ptr, 1));
}

Operations& Operations::set_update_refs(bool update) {
    impl_->update_refs = update;
    return *this;
}

Operations& Operations::set_force(bool force) {
    impl_->force = force;
    return *this;
}

Operations& Operations::set_dry_run(bool dry_run) {
    impl_->dry_run = dry_run;
    return *this;
}

// CommitBuilder implementation
struct CommitBuilder::Impl {
    Repository* repo = nullptr;
    std::unique_ptr<Commit> base;
    std::optional<std::string> message;
    std::optional<Signature> author;
    std::optional<Signature> committer;
    std::optional<Tree> tree;
    std::vector<const Commit*> parents;
    std::vector<std::function<void(TreeBuilder&)>> tree_modifiers;
    Signer* signer = nullptr;
};

CommitBuilder::CommitBuilder(Repository& repo, const Commit& base)
    : impl_(std::make_unique<Impl>()) {
    impl_->repo = &repo;
    impl_->base = std::make_unique<Commit>(repo.lookup_commit(base.id()));
}

CommitBuilder::~CommitBuilder() = default;
CommitBuilder::CommitBuilder(CommitBuilder&&) noexcept = default;
CommitBuilder& CommitBuilder::operator=(CommitBuilder&&) noexcept = default;

CommitBuilder& CommitBuilder::set_message(std::string_view message) {
    impl_->message = std::string(message);
    return *this;
}

CommitBuilder& CommitBuilder::set_author(const Signature& author) {
    impl_->author = author;
    return *this;
}

CommitBuilder& CommitBuilder::set_committer(const Signature& committer) {
    impl_->committer = committer;
    return *this;
}

CommitBuilder& CommitBuilder::set_tree(const Tree& tree) {
    impl_->tree = impl_->repo->lookup_tree(tree.id());
    return *this;
}

CommitBuilder& CommitBuilder::modify_tree(std::function<void(TreeBuilder&)> modifier) {
    impl_->tree_modifiers.push_back(std::move(modifier));
    return *this;
}

CommitBuilder& CommitBuilder::add_file(std::string_view path, std::span<const uint8_t> content) {
    return modify_tree([p = std::string(path), c = std::vector<uint8_t>(content.begin(), content.end())](TreeBuilder& tb) {
        tb.insert_blob(p, c);
    });
}

CommitBuilder& CommitBuilder::add_file(std::string_view path, std::string_view content) {
    return add_file(path, std::span(reinterpret_cast<const uint8_t*>(content.data()),
                                     content.size()));
}

CommitBuilder& CommitBuilder::remove_file(std::string_view path) {
    return modify_tree([p = std::string(path)](TreeBuilder& tb) {
        tb.remove(p);
    });
}

CommitBuilder& CommitBuilder::modify_file(std::string_view path,
                                           std::function<std::string(std::string_view)> modifier) {
    return modify_tree([this, p = std::string(path), mod = std::move(modifier)](TreeBuilder& tb) {
        auto content = impl_->base->tree().blob_content_string(p);
        if (content) {
            std::string new_content = mod(*content);
            tb.insert_blob(p, new_content);
        }
    });
}

CommitBuilder& CommitBuilder::set_parents(std::span<const Commit* const> parents) {
    impl_->parents.assign(parents.begin(), parents.end());
    return *this;
}

CommitBuilder& CommitBuilder::set_parent(const Commit& parent) {
    impl_->parents = {&parent};
    return *this;
}

CommitBuilder& CommitBuilder::set_signer(Signer* signer) {
    impl_->signer = signer;
    return *this;
}

Oid CommitBuilder::build() {
    // Determine tree
    Tree final_tree = impl_->tree
        ? std::move(*impl_->tree)
        : std::move(impl_->base->tree());

    // Apply tree modifiers
    if (!impl_->tree_modifiers.empty()) {
        TreeBuilder tb(*impl_->repo, final_tree);
        for (const auto& modifier : impl_->tree_modifiers) {
            modifier(tb);
        }
        final_tree = tb.build();
    }

    // Determine parents
    std::vector<const Commit*> parents;
    if (impl_->parents.empty()) {
        auto base_parents = impl_->base->parents();
        for (const auto& p : base_parents) {
            parents.push_back(&p);
        }
    } else {
        parents = impl_->parents;
    }

    Signature author = impl_->author.value_or(impl_->base->author());
    Signature committer = impl_->committer.value_or(impl_->repo->default_signature());
    std::string message = impl_->message.value_or(impl_->base->message());

    // If signer is set, create a signed commit
    if (impl_->signer) {
        return create_signed_commit(
            *impl_->repo,
            message,
            final_tree,
            std::span<const Commit* const>(parents),
            author,
            committer,
            *impl_->signer);
    }

    return impl_->repo->create_commit(
        message,
        final_tree,
        std::span<const Commit* const>(parents),
        author,
        committer);
}

Commit CommitBuilder::build_commit() {
    return impl_->repo->lookup_commit(build());
}

}  // namespace gitmanip
