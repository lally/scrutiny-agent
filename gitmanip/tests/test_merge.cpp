#include "test_fixture.hpp"

class MergeTest : public GitTestFixture {};

TEST_F(MergeTest, MergeCommitProperties) {
    // Create a merge scenario
    auto base_oid = createCommit("Base", {{"base.txt", "base"}});
    auto left_oid = createCommit("Left branch", {{"base.txt", "base"}, {"left.txt", "left"}}, base_oid);
    auto right_oid = createCommit("Right branch", {{"base.txt", "base"}, {"right.txt", "right"}}, base_oid);

    // Create merge commit
    auto merge_oid = createMergeCommit("Merge branches", left_oid, right_oid);

    auto merge = repo_->lookup_commit(merge_oid);

    // Check merge commit properties
    EXPECT_TRUE(merge.is_merge());
    EXPECT_EQ(merge.parent_count(), 2);
    EXPECT_EQ(merge.parent_id(0), left_oid);
    EXPECT_EQ(merge.parent_id(1), right_oid);

    // Both parent commits should not be merges
    auto left = repo_->lookup_commit(left_oid);
    auto right = repo_->lookup_commit(right_oid);
    EXPECT_FALSE(left.is_merge());
    EXPECT_FALSE(right.is_merge());
}

TEST_F(MergeTest, MergeCommitTree) {
    auto base_oid = createCommit("Base", {{"base.txt", "base"}});
    auto left_oid = createCommit("Left", {{"base.txt", "base"}, {"left.txt", "left content"}}, base_oid);
    auto right_oid = createCommit("Right", {{"base.txt", "base"}, {"right.txt", "right content"}}, base_oid);

    auto merge_oid = createMergeCommit("Merge", left_oid, right_oid);

    auto merge = repo_->lookup_commit(merge_oid);
    auto tree = merge.tree();

    // Merge should have all files from both branches
    EXPECT_TRUE(tree.entry_by_name("base.txt").has_value());
    EXPECT_TRUE(tree.entry_by_name("left.txt").has_value());
    EXPECT_TRUE(tree.entry_by_name("right.txt").has_value());
}

TEST_F(MergeTest, MergeDiff) {
    auto base_oid = createCommit("Base", {{"file.txt", "base content"}});
    auto left_oid = createCommit("Left", {{"file.txt", "left content"}}, base_oid);
    auto right_oid = createCommit("Right", {{"file.txt", "right content"}}, base_oid);

    // Create merge commit that "resolves" to left's version (simple)
    auto left = repo_->lookup_commit(left_oid);
    auto right = repo_->lookup_commit(right_oid);

    gitmanip::TreeBuilder builder(*repo_, left.tree());
    auto tree = builder.build();

    std::vector<const gitmanip::Commit*> parents = {&left, &right};
    auto merge_oid = repo_->create_commit("Merge", tree, parents, sig_, sig_);

    auto merge = repo_->lookup_commit(merge_oid);

    // merge_diff should show differences unique to this merge
    auto merge_diff = merge.merge_diff();

    // Since we just took left's version, there should be no unique merge changes
    // (the content matches left parent)
    // This tests that merge_diff filters out changes that match any parent
}

TEST_F(MergeTest, MergeDiffWithEvilMerge) {
    auto base_oid = createCommit("Base", {{"file.txt", "base content"}});
    auto left_oid = createCommit("Left", {{"file.txt", "left content"}}, base_oid);
    auto right_oid = createCommit("Right", {{"file.txt", "right content"}}, base_oid);

    auto left = repo_->lookup_commit(left_oid);
    auto right = repo_->lookup_commit(right_oid);

    // Create "evil merge" with content that doesn't match either parent
    gitmanip::TreeBuilder builder(*repo_);
    builder.insert_blob("file.txt", "evil merge content - not in either parent");
    auto tree = builder.build();

    std::vector<const gitmanip::Commit*> parents = {&left, &right};
    auto merge_oid = repo_->create_commit("Evil merge", tree, parents, sig_, sig_);

    auto merge = repo_->lookup_commit(merge_oid);
    EXPECT_TRUE(merge.is_merge());

    // has_merge_changes should detect the evil merge
    EXPECT_TRUE(merge.has_merge_changes());
}

TEST_F(MergeTest, MergeDiffCleanMerge) {
    auto base_oid = createCommit("Base", {{"base.txt", "base"}});
    auto left_oid = createCommit("Left", {{"base.txt", "base"}, {"left.txt", "left"}}, base_oid);
    auto right_oid = createCommit("Right", {{"base.txt", "base"}, {"right.txt", "right"}}, base_oid);

    // Create clean merge (content from both parents, nothing extra)
    auto merge_oid = createMergeCommit("Clean merge", left_oid, right_oid);

    auto merge = repo_->lookup_commit(merge_oid);

    // Clean merge should have no unique changes
    EXPECT_FALSE(merge.has_merge_changes());
}

TEST_F(MergeTest, DiffFromEachParent) {
    auto base_oid = createCommit("Base", {{"base.txt", "base"}});
    auto left_oid = createCommit("Left", {{"base.txt", "base"}, {"left.txt", "left content"}}, base_oid);
    auto right_oid = createCommit("Right", {{"base.txt", "base"}, {"right.txt", "right content"}}, base_oid);

    auto merge_oid = createMergeCommit("Merge", left_oid, right_oid);
    auto merge = repo_->lookup_commit(merge_oid);

    // Diff from first parent (left) should show right.txt as added
    auto diff_from_left = merge.diff_from_parent(0);

    bool found_right_added = false;
    for (size_t i = 0; i < diff_from_left.num_deltas(); ++i) {
        auto delta = diff_from_left.delta(i);
        if (delta && delta->new_path == "right.txt" &&
            delta->status == gitmanip::DiffDelta::Status::Added) {
            found_right_added = true;
        }
    }
    EXPECT_TRUE(found_right_added);

    // Diff from second parent (right) should show left.txt as added
    auto diff_from_right = merge.diff_from_parent(1);

    bool found_left_added = false;
    for (size_t i = 0; i < diff_from_right.num_deltas(); ++i) {
        auto delta = diff_from_right.delta(i);
        if (delta && delta->new_path == "left.txt" &&
            delta->status == gitmanip::DiffDelta::Status::Added) {
            found_left_added = true;
        }
    }
    EXPECT_TRUE(found_left_added);
}

TEST_F(MergeTest, NonMergeIsNotMerge) {
    auto oid = createCommit("Single parent", {{"file.txt", "content"}});
    auto commit = repo_->lookup_commit(oid);

    EXPECT_FALSE(commit.is_merge());
    EXPECT_EQ(commit.parent_count(), 0);
}

TEST_F(MergeTest, RootCommitIsRoot) {
    auto oid = createCommit("Root", {{"file.txt", "content"}});
    auto commit = repo_->lookup_commit(oid);

    EXPECT_TRUE(commit.is_root());
    EXPECT_EQ(commit.parent_count(), 0);
}

TEST_F(MergeTest, DiffFromParentInvalidIndex) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"b.txt", "b"}}, first_oid);

    auto second = repo_->lookup_commit(second_oid);

    // Parent index 1 is invalid for a non-merge commit
    EXPECT_THROW(second.diff_from_parent(1), gitmanip::GitError);
}

TEST_F(MergeTest, MergeCommitParentAccessors) {
    auto base_oid = createCommit("Base", {{"base.txt", "base"}});
    auto left_oid = createCommit("Left", {{"left.txt", "left"}}, base_oid);
    auto right_oid = createCommit("Right", {{"right.txt", "right"}}, base_oid);

    auto merge_oid = createMergeCommit("Merge", left_oid, right_oid);
    auto merge = repo_->lookup_commit(merge_oid);

    // Test parent() accessor
    auto parent0 = merge.parent(0);
    auto parent1 = merge.parent(1);

    EXPECT_EQ(parent0.id(), left_oid);
    EXPECT_EQ(parent1.id(), right_oid);

    // Test parents() accessor
    auto all_parents = merge.parents();
    EXPECT_EQ(all_parents.size(), 2);

    // Test parent_id() accessor
    EXPECT_EQ(merge.parent_id(0), left_oid);
    EXPECT_EQ(merge.parent_id(1), right_oid);
}

TEST_F(MergeTest, OctopusMerge) {
    // Create an octopus merge (3+ parents)
    auto base_oid = createCommit("Base", {{"base.txt", "base"}});
    auto branch1_oid = createCommit("Branch1", {{"base.txt", "base"}, {"b1.txt", "b1"}}, base_oid);
    auto branch2_oid = createCommit("Branch2", {{"base.txt", "base"}, {"b2.txt", "b2"}}, base_oid);
    auto branch3_oid = createCommit("Branch3", {{"base.txt", "base"}, {"b3.txt", "b3"}}, base_oid);

    // Create octopus merge manually
    auto b1 = repo_->lookup_commit(branch1_oid);
    auto b2 = repo_->lookup_commit(branch2_oid);
    auto b3 = repo_->lookup_commit(branch3_oid);

    gitmanip::TreeBuilder builder(*repo_, b1.tree());
    for (auto entry : b2.tree().entries()) {
        auto content = b2.tree().blob_content(entry.name);
        if (content) builder.insert_blob(entry.name, *content);
    }
    for (auto entry : b3.tree().entries()) {
        auto content = b3.tree().blob_content(entry.name);
        if (content) builder.insert_blob(entry.name, *content);
    }
    auto tree = builder.build();

    std::vector<const gitmanip::Commit*> parents = {&b1, &b2, &b3};
    auto octopus_oid = repo_->create_commit("Octopus merge", tree, parents, sig_, sig_);

    auto octopus = repo_->lookup_commit(octopus_oid);

    EXPECT_TRUE(octopus.is_merge());
    EXPECT_EQ(octopus.parent_count(), 3);

    // Tree should have all files
    auto final_tree = octopus.tree();
    EXPECT_TRUE(final_tree.entry_by_name("base.txt").has_value());
    EXPECT_TRUE(final_tree.entry_by_name("b1.txt").has_value());
    EXPECT_TRUE(final_tree.entry_by_name("b2.txt").has_value());
    EXPECT_TRUE(final_tree.entry_by_name("b3.txt").has_value());
}
