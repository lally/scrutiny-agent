#include "test_fixture.hpp"

class DiffTest : public GitTestFixture {};

TEST_F(DiffTest, DiffTreeToTree) {
    auto first_oid = createCommit("First", {{"file.txt", "line1\nline2\n"}});
    auto second_oid = createCommit("Second", {{"file.txt", "line1\nline2\nline3\n"}}, first_oid);

    auto first = repo_->lookup_commit(first_oid);
    auto second = repo_->lookup_commit(second_oid);

    auto diff = repo_->diff_tree_to_tree(first.tree(), second.tree());

    EXPECT_EQ(diff.num_deltas(), 1);
    auto stats = diff.stats();
    EXPECT_EQ(stats.insertions, 1);
    EXPECT_EQ(stats.deletions, 0);
}

TEST_F(DiffTest, DiffAddedFile) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"a.txt", "a"}, {"b.txt", "b"}}, first_oid);

    auto second = repo_->lookup_commit(second_oid);
    auto diff = second.diff_from_parent();

    EXPECT_EQ(diff.num_deltas(), 1);

    auto delta = diff.delta(0);
    ASSERT_TRUE(delta.has_value());
    EXPECT_EQ(delta->status, gitmanip::DiffDelta::Status::Added);
    EXPECT_EQ(delta->new_path, "b.txt");
}

TEST_F(DiffTest, DiffDeletedFile) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}, {"b.txt", "b"}});

    // Create second commit with only a.txt (effectively deleting b.txt)
    gitmanip::TreeBuilder builder(*repo_);
    builder.insert_blob("a.txt", "a");
    auto tree = builder.build();

    auto first = repo_->lookup_commit(first_oid);
    std::vector<const gitmanip::Commit*> parents = {&first};
    auto second_oid = repo_->create_commit("Second", tree, parents, sig_, sig_);

    auto second = repo_->lookup_commit(second_oid);
    auto diff = second.diff_from_parent();

    EXPECT_EQ(diff.num_deltas(), 1);

    auto delta = diff.delta(0);
    ASSERT_TRUE(delta.has_value());
    EXPECT_EQ(delta->status, gitmanip::DiffDelta::Status::Deleted);
    EXPECT_EQ(delta->old_path, "b.txt");
}

TEST_F(DiffTest, DiffModifiedFile) {
    auto first_oid = createCommit("First", {{"file.txt", "original content"}});
    auto second_oid = createCommit("Second", {{"file.txt", "modified content"}}, first_oid);

    auto second = repo_->lookup_commit(second_oid);
    auto diff = second.diff_from_parent();

    EXPECT_EQ(diff.num_deltas(), 1);

    auto delta = diff.delta(0);
    ASSERT_TRUE(delta.has_value());
    EXPECT_EQ(delta->status, gitmanip::DiffDelta::Status::Modified);
}

TEST_F(DiffTest, DiffRenamedFile) {
    auto first_oid = createCommit("First", {{"old_name.txt", "content"}});

    // Create tree with renamed file
    gitmanip::TreeBuilder builder(*repo_);
    builder.insert_blob("new_name.txt", "content");
    auto tree = builder.build();

    auto first = repo_->lookup_commit(first_oid);
    std::vector<const gitmanip::Commit*> parents = {&first};
    auto second_oid = repo_->create_commit("Second", tree, parents, sig_, sig_);

    auto second = repo_->lookup_commit(second_oid);

    // Use find_similar to enable rename detection
    auto diff = repo_->diff_tree_to_tree(first.tree(), second.tree());
    diff.find_similar();

    EXPECT_GE(diff.num_deltas(), 1);

    // With rename detection, should see Renamed status
    bool found_rename = false;
    for (size_t i = 0; i < diff.num_deltas(); ++i) {
        auto delta = diff.delta(i);
        if (delta && delta->status == gitmanip::DiffDelta::Status::Renamed) {
            found_rename = true;
            EXPECT_EQ(delta->old_path, "old_name.txt");
            EXPECT_EQ(delta->new_path, "new_name.txt");
        }
    }
    EXPECT_TRUE(found_rename);
}

TEST_F(DiffTest, DiffStats) {
    auto first_oid = createCommit("First", {{"file.txt", "line1\nline2\nline3\n"}});
    auto second_oid = createCommit("Second", {
        {"file.txt", "line1\nmodified\nline3\nnew line\n"}
    }, first_oid);

    auto second = repo_->lookup_commit(second_oid);
    auto diff = second.diff_from_parent();

    auto stats = diff.stats();
    EXPECT_EQ(stats.files_changed, 1);
    EXPECT_GT(stats.insertions, 0);
    EXPECT_GT(stats.deletions, 0);
}

TEST_F(DiffTest, DiffHunks) {
    auto first_oid = createCommit("First", {{"file.txt", "line1\nline2\nline3\n"}});
    auto second_oid = createCommit("Second", {{"file.txt", "line1\nmodified\nline3\n"}}, first_oid);

    auto second = repo_->lookup_commit(second_oid);
    auto diff = second.diff_from_parent();

    // Get hunks for the first delta
    auto hunks = diff.hunks(0);
    EXPECT_GE(hunks.size(), 1);

    if (!hunks.empty()) {
        auto& hunk = hunks[0];
        EXPECT_GT(hunk.old_start, 0);
        EXPECT_GT(hunk.new_start, 0);
    }
}

TEST_F(DiffTest, DiffLines) {
    auto first_oid = createCommit("First", {{"file.txt", "line1\nline2\n"}});
    auto second_oid = createCommit("Second", {{"file.txt", "line1\nmodified\n"}}, first_oid);

    auto second = repo_->lookup_commit(second_oid);
    auto diff = second.diff_from_parent();

    auto lines = diff.lines(0, 0);
    EXPECT_GE(lines.size(), 1);

    // Should have deletion and addition lines
    bool has_addition = false;
    bool has_deletion = false;
    for (const auto& line : lines) {
        if (line.origin == gitmanip::DiffLine::Origin::Addition) has_addition = true;
        if (line.origin == gitmanip::DiffLine::Origin::Deletion) has_deletion = true;
    }
    EXPECT_TRUE(has_addition);
    EXPECT_TRUE(has_deletion);
}

TEST_F(DiffTest, DiffPatch) {
    auto first_oid = createCommit("First", {{"file.txt", "line1\n"}});
    auto second_oid = createCommit("Second", {{"file.txt", "line1\nline2\n"}}, first_oid);

    auto second = repo_->lookup_commit(second_oid);
    auto diff = second.diff_from_parent();

    auto patch = diff.patch(0);
    EXPECT_FALSE(patch.empty());
    EXPECT_TRUE(patch.find("+line2") != std::string::npos);
}

TEST_F(DiffTest, DiffFullPatch) {
    auto first_oid = createCommit("First", {{"a.txt", "a\n"}, {"b.txt", "b\n"}});
    auto second_oid = createCommit("Second", {{"a.txt", "a modified\n"}, {"b.txt", "b modified\n"}}, first_oid);

    auto second = repo_->lookup_commit(second_oid);
    auto diff = second.diff_from_parent();

    auto full_patch = diff.full_patch();
    EXPECT_TRUE(full_patch.find("a.txt") != std::string::npos);
    EXPECT_TRUE(full_patch.find("b.txt") != std::string::npos);
}

TEST_F(DiffTest, DiffRootCommit) {
    auto first_oid = createCommit("First", {{"file.txt", "content"}});

    auto first = repo_->lookup_commit(first_oid);
    EXPECT_TRUE(first.is_root());

    auto diff = first.diff_from_parent();

    // Root commit diff should show all files as added
    EXPECT_EQ(diff.num_deltas(), 1);
    auto delta = diff.delta(0);
    ASSERT_TRUE(delta.has_value());
    EXPECT_EQ(delta->status, gitmanip::DiffDelta::Status::Added);
}

TEST_F(DiffTest, DiffFromSpecificParent) {
    // Create a merge scenario
    auto base_oid = createCommit("Base", {{"base.txt", "base"}});
    auto left_oid = createCommit("Left", {{"base.txt", "base"}, {"left.txt", "left"}}, base_oid);
    auto right_oid = createCommit("Right", {{"base.txt", "base"}, {"right.txt", "right"}}, base_oid);

    // Create merge commit
    auto merge_oid = createMergeCommit("Merge", left_oid, right_oid);

    auto merge = repo_->lookup_commit(merge_oid);
    EXPECT_TRUE(merge.is_merge());
    EXPECT_EQ(merge.parent_count(), 2);

    // Diff from first parent (left) should show right.txt as added
    auto diff0 = merge.diff_from_parent(0);
    bool has_right = false;
    for (size_t i = 0; i < diff0.num_deltas(); ++i) {
        auto delta = diff0.delta(i);
        if (delta && delta->new_path == "right.txt") {
            has_right = true;
        }
    }
    EXPECT_TRUE(has_right);

    // Diff from second parent (right) should show left.txt as added
    auto diff1 = merge.diff_from_parent(1);
    bool has_left = false;
    for (size_t i = 0; i < diff1.num_deltas(); ++i) {
        auto delta = diff1.delta(i);
        if (delta && delta->new_path == "left.txt") {
            has_left = true;
        }
    }
    EXPECT_TRUE(has_left);
}
