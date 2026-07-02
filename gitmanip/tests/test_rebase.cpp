#include <gitmanip/gitmanip.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class RebaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "gitmanip_rebase_test";
        fs::create_directories(test_dir_);

        repo_ = std::make_unique<gitmanip::Repository>(
            gitmanip::Repository::init(test_dir_, false));

        sig_ = gitmanip::Signature::now("Test User", "test@example.com");
    }

    void TearDown() override {
        repo_.reset();
        fs::remove_all(test_dir_);
    }

    gitmanip::Oid createCommit(const std::string& message,
                                const std::vector<std::pair<std::string, std::string>>& files,
                                std::optional<gitmanip::Oid> parent = std::nullopt) {
        gitmanip::TreeBuilder builder(*repo_);

        for (const auto& [name, content] : files) {
            builder.insert_blob(name, content);
        }

        auto tree = builder.build();

        std::vector<const gitmanip::Commit*> parents;
        std::optional<gitmanip::Commit> parent_commit;
        if (parent) {
            parent_commit = repo_->lookup_commit(*parent);
            parents.push_back(&*parent_commit);
        }

        return repo_->create_commit(message, tree, parents, sig_, sig_);
    }

    fs::path test_dir_;
    std::unique_ptr<gitmanip::Repository> repo_;
    gitmanip::Signature sig_;
};

TEST_F(RebaseTest, RebaseActionCreation) {
    auto oid = createCommit("Test commit", {{"file.txt", "content"}});
    auto commit = repo_->lookup_commit(oid);

    auto pick = gitmanip::RebaseAction::pick(commit);
    EXPECT_EQ(pick.type, gitmanip::RebaseAction::Type::Pick);
    EXPECT_EQ(pick.original_oid, oid);
    EXPECT_EQ(pick.label(), "pick");

    auto reword = gitmanip::RebaseAction::reword(commit, "New message");
    EXPECT_EQ(reword.type, gitmanip::RebaseAction::Type::Reword);
    EXPECT_TRUE(reword.new_message.has_value());
    EXPECT_EQ(*reword.new_message, "New message");

    auto drop = gitmanip::RebaseAction::drop(commit);
    EXPECT_EQ(drop.type, gitmanip::RebaseAction::Type::Drop);
    EXPECT_EQ(drop.label(), "drop");
}

TEST_F(RebaseTest, RebasePlanConstruction) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"b.txt", "b"}}, first_oid);
    auto third_oid = createCommit("Third", {{"c.txt", "c"}}, second_oid);

    // Update refs
    repo_->update_ref("refs/heads/main", first_oid);
    repo_->update_ref("refs/heads/feature", third_oid);

    auto plan = gitmanip::RebasePlan::from_range(*repo_, "refs/heads/main", "refs/heads/feature");

    EXPECT_EQ(plan.size(), 2);  // Second and Third commits
    EXPECT_EQ(plan.onto(), first_oid);
    EXPECT_TRUE(plan.is_valid());
}

TEST_F(RebaseTest, RebasePlanManipulation) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"b.txt", "b"}}, first_oid);
    auto third_oid = createCommit("Third", {{"c.txt", "c"}}, second_oid);

    auto first = repo_->lookup_commit(first_oid);
    auto second = repo_->lookup_commit(second_oid);
    auto third = repo_->lookup_commit(third_oid);

    gitmanip::RebasePlan plan(*repo_, first_oid);
    plan.add(gitmanip::RebaseAction::pick(second));
    plan.add(gitmanip::RebaseAction::pick(third));

    EXPECT_EQ(plan.size(), 2);

    // Reorder
    plan.move(1, 0);
    EXPECT_EQ(plan.action(0).original_oid, third_oid);
    EXPECT_EQ(plan.action(1).original_oid, second_oid);

    // Replace with drop
    plan.replace(0, gitmanip::RebaseAction::drop(third));
    EXPECT_EQ(plan.action(0).type, gitmanip::RebaseAction::Type::Drop);
}

TEST_F(RebaseTest, RebasePlanToString) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"b.txt", "b"}}, first_oid);

    auto second = repo_->lookup_commit(second_oid);

    gitmanip::RebasePlan plan(*repo_, first_oid);
    plan.add(gitmanip::RebaseAction::pick(second));
    plan.add(gitmanip::RebaseAction::reword(second, "New message"));
    plan.add(gitmanip::RebaseAction::drop(second));

    std::string plan_str = plan.to_string();

    EXPECT_TRUE(plan_str.find("pick") != std::string::npos);
    EXPECT_TRUE(plan_str.find("reword") != std::string::npos);
    EXPECT_TRUE(plan_str.find("drop") != std::string::npos);
}

TEST_F(RebaseTest, RebasePlanValidation) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"b.txt", "b"}}, first_oid);

    auto first = repo_->lookup_commit(first_oid);
    auto second = repo_->lookup_commit(second_oid);

    gitmanip::RebasePlan plan(*repo_, first_oid);

    // Squash as first action is invalid
    plan.add(gitmanip::RebaseAction::squash(second));

    EXPECT_FALSE(plan.is_valid());
    auto errors = plan.validation_errors();
    EXPECT_FALSE(errors.empty());
}

TEST_F(RebaseTest, ExecuteSimpleRebase) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"a.txt", "a"}, {"b.txt", "b"}}, first_oid);
    auto third_oid = createCommit("Third", {{"a.txt", "a"}, {"b.txt", "b"}, {"c.txt", "c"}}, second_oid);

    auto second = repo_->lookup_commit(second_oid);
    auto third = repo_->lookup_commit(third_oid);

    gitmanip::RebasePlan plan(*repo_, first_oid);
    plan.add(gitmanip::RebaseAction::pick(second));
    plan.add(gitmanip::RebaseAction::pick(third));

    gitmanip::Operations ops(*repo_);
    auto result = ops.execute(plan);

    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.commit_mapping.size(), 2);

    // Verify the new commits exist
    auto new_head = repo_->lookup_commit(result.new_head);
    EXPECT_EQ(new_head.summary(), "Third");

    auto new_second = new_head.parent(0);
    EXPECT_EQ(new_second.summary(), "Second");
}

TEST_F(RebaseTest, ExecuteRewordRebase) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"a.txt", "a"}, {"b.txt", "b"}}, first_oid);

    auto second = repo_->lookup_commit(second_oid);

    gitmanip::RebasePlan plan(*repo_, first_oid);
    plan.add(gitmanip::RebaseAction::reword(second, "Reworded message"));

    gitmanip::Operations ops(*repo_);
    auto result = ops.execute(plan);

    EXPECT_TRUE(result.success);

    auto new_head = repo_->lookup_commit(result.new_head);
    EXPECT_EQ(new_head.message(), "Reworded message");
}

TEST_F(RebaseTest, ExecuteSquashRebase) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"a.txt", "a"}, {"b.txt", "b"}}, first_oid);
    auto third_oid = createCommit("Third", {{"a.txt", "a"}, {"b.txt", "b"}, {"c.txt", "c"}}, second_oid);

    auto second = repo_->lookup_commit(second_oid);
    auto third = repo_->lookup_commit(third_oid);

    gitmanip::RebasePlan plan(*repo_, first_oid);
    plan.add(gitmanip::RebaseAction::pick(second));
    plan.add(gitmanip::RebaseAction::fixup(third));

    gitmanip::Operations ops(*repo_);
    auto result = ops.execute(plan);

    EXPECT_TRUE(result.success);

    auto new_head = repo_->lookup_commit(result.new_head);
    EXPECT_EQ(new_head.summary(), "Second");  // Message from first pick
    EXPECT_EQ(new_head.parent_count(), 1);

    // Should have both b.txt and c.txt
    auto tree = new_head.tree();
    EXPECT_TRUE(tree.entry_by_name("b.txt").has_value());
    EXPECT_TRUE(tree.entry_by_name("c.txt").has_value());
}

TEST_F(RebaseTest, ExecuteDropRebase) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"a.txt", "a"}, {"b.txt", "b"}}, first_oid);
    auto third_oid = createCommit("Third", {{"a.txt", "a"}, {"b.txt", "b"}, {"c.txt", "c"}}, second_oid);

    auto second = repo_->lookup_commit(second_oid);
    auto third = repo_->lookup_commit(third_oid);

    gitmanip::RebasePlan plan(*repo_, first_oid);
    plan.add(gitmanip::RebaseAction::drop(second));
    plan.add(gitmanip::RebaseAction::pick(third));

    gitmanip::Operations ops(*repo_);
    auto result = ops.execute(plan);

    EXPECT_TRUE(result.success);

    // Only one commit should be created (third on top of first)
    auto new_head = repo_->lookup_commit(result.new_head);
    EXPECT_EQ(new_head.parent(0).id(), first_oid);
}

TEST_F(RebaseTest, ChangeModifications) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"a.txt", "a"}, {"b.txt", "b"}, {"c.txt", "c"}}, first_oid);

    auto second = repo_->lookup_commit(second_oid);

    gitmanip::RebasePlan plan(*repo_, first_oid);
    plan.add(gitmanip::RebaseAction::pick(second));
    plan.modify_changes(0, gitmanip::ChangeModification::drop_file("c.txt"));

    gitmanip::Operations ops(*repo_);
    auto result = ops.execute(plan);

    EXPECT_TRUE(result.success);

    auto new_head = repo_->lookup_commit(result.new_head);
    auto tree = new_head.tree();

    EXPECT_TRUE(tree.entry_by_name("a.txt").has_value());
    EXPECT_TRUE(tree.entry_by_name("b.txt").has_value());
    EXPECT_FALSE(tree.entry_by_name("c.txt").has_value());
}

TEST_F(RebaseTest, CommitBuilder) {
    auto first_oid = createCommit("First", {{"a.txt", "original"}});
    auto first = repo_->lookup_commit(first_oid);

    gitmanip::CommitBuilder builder(*repo_, first);
    builder.set_message("Modified message")
           .modify_file("a.txt", [](std::string_view content) {
               return std::string(content) + "\nappended";
           })
           .add_file("new.txt", "new content");

    auto new_oid = builder.build();
    auto new_commit = repo_->lookup_commit(new_oid);

    EXPECT_EQ(new_commit.message(), "Modified message");

    auto tree = new_commit.tree();
    auto a_content = tree.blob_content_string("a.txt");
    EXPECT_TRUE(a_content.has_value());
    EXPECT_TRUE(a_content->find("appended") != std::string::npos);

    EXPECT_TRUE(tree.entry_by_name("new.txt").has_value());
}
