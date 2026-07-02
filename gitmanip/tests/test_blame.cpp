#include "test_fixture.hpp"

class BlameTest : public GitTestFixture {};

TEST_F(BlameTest, BasicBlame) {
    auto first_oid = createCommit("First", {{"file.txt", "line1\nline2\nline3\n"}});
    setupMain(first_oid);

    auto blame = gitmanip::Blame::file(*repo_, "file.txt");

    // Should have hunks covering all lines
    auto hunks = blame.hunks();
    EXPECT_GE(hunks.size(), 1);

    // All lines should be attributed to first commit
    size_t total_lines = 0;
    for (const auto& hunk : hunks) {
        EXPECT_EQ(hunk.final_commit_id, first_oid);
        total_lines += hunk.lines_in_hunk;
    }
    EXPECT_EQ(total_lines, 3);
}

TEST_F(BlameTest, BlameMultipleCommits) {
    auto first_oid = createCommit("First", {{"file.txt", "line1\nline2\n"}});
    auto second_oid = createCommit("Second", {{"file.txt", "line1\nline2\nline3\n"}}, first_oid);
    setupMain(second_oid);

    auto blame = gitmanip::Blame::file(*repo_, "file.txt");
    auto hunks = blame.hunks();

    // Should have at least 2 hunks (or 1 if they got merged)
    EXPECT_GE(hunks.size(), 1);

    // Line 3 should be from second commit
    auto line3_hunk = blame.line(3);
    ASSERT_TRUE(line3_hunk.has_value());
    EXPECT_EQ(line3_hunk->final_commit_id, second_oid);

    // Lines 1-2 should be from first commit
    auto line1_hunk = blame.line(1);
    ASSERT_TRUE(line1_hunk.has_value());
    EXPECT_EQ(line1_hunk->final_commit_id, first_oid);
}

TEST_F(BlameTest, BlameModifiedLine) {
    auto first_oid = createCommit("First", {{"file.txt", "line1\noriginal\nline3\n"}});
    auto second_oid = createCommit("Second", {{"file.txt", "line1\nmodified\nline3\n"}}, first_oid);
    setupMain(second_oid);

    auto blame = gitmanip::Blame::file(*repo_, "file.txt");

    // Line 2 should be from second commit (it was modified)
    auto line2_hunk = blame.line(2);
    ASSERT_TRUE(line2_hunk.has_value());
    EXPECT_EQ(line2_hunk->final_commit_id, second_oid);

    // Lines 1 and 3 should be from first commit
    auto line1_hunk = blame.line(1);
    ASSERT_TRUE(line1_hunk.has_value());
    EXPECT_EQ(line1_hunk->final_commit_id, first_oid);
}

TEST_F(BlameTest, BlameAtCommit) {
    auto first_oid = createCommit("First", {{"file.txt", "line1\nline2\n"}});
    auto second_oid = createCommit("Second", {{"file.txt", "line1\nline2\nline3\n"}}, first_oid);
    setupMain(second_oid);

    // Blame at first commit - should only see first commit's lines
    auto blame = gitmanip::Blame::file_at(*repo_, "file.txt", first_oid);
    auto hunks = blame.hunks();

    // All lines at first commit should be from first commit
    for (const auto& hunk : hunks) {
        EXPECT_EQ(hunk.final_commit_id, first_oid);
    }
}

TEST_F(BlameTest, BlameLineRange) {
    auto first_oid = createCommit("First", {
        {"file.txt", "line1\nline2\nline3\nline4\nline5\n"}
    });
    setupMain(first_oid);

    gitmanip::BlameOptions opts;
    opts.min_line = 2;
    opts.max_line = 4;

    auto blame = gitmanip::Blame::file(*repo_, "file.txt", opts);
    auto hunks = blame.hunks();

    // Should only have lines 2-4
    size_t total_lines = 0;
    for (const auto& hunk : hunks) {
        total_lines += hunk.lines_in_hunk;
    }
    EXPECT_EQ(total_lines, 3);
}

TEST_F(BlameTest, BlameSignature) {
    auto first_oid = createCommit("First", {{"file.txt", "content\n"}});
    setupMain(first_oid);

    auto blame = gitmanip::Blame::file(*repo_, "file.txt");
    auto hunks = blame.hunks();

    ASSERT_GE(hunks.size(), 1);
    EXPECT_EQ(hunks[0].final_signature.name, "Test User");
    EXPECT_EQ(hunks[0].final_signature.email, "test@example.com");
}

TEST_F(BlameTest, BlameLineCommit) {
    auto first_oid = createCommit("First", {{"file.txt", "line1\n"}});
    auto second_oid = createCommit("Second", {{"file.txt", "line1\nline2\n"}}, first_oid);
    setupMain(second_oid);

    auto blame = gitmanip::Blame::file(*repo_, "file.txt");

    auto line1_commit = blame.line_commit(1);
    ASSERT_TRUE(line1_commit.has_value());
    EXPECT_EQ(*line1_commit, first_oid);

    auto line2_commit = blame.line_commit(2);
    ASSERT_TRUE(line2_commit.has_value());
    EXPECT_EQ(*line2_commit, second_oid);
}

TEST_F(BlameTest, BlameHunkEndLine) {
    auto first_oid = createCommit("First", {{"file.txt", "line1\nline2\nline3\n"}});
    setupMain(first_oid);

    auto blame = gitmanip::Blame::file(*repo_, "file.txt");
    auto hunks = blame.hunks();

    ASSERT_GE(hunks.size(), 1);
    auto& hunk = hunks[0];

    // end_line() should be start_line + lines_in_hunk - 1
    EXPECT_EQ(hunk.end_line(), hunk.start_line + hunk.lines_in_hunk - 1);
}

TEST_F(BlameTest, BlameNonexistentFile) {
    auto first_oid = createCommit("First", {{"file.txt", "content"}});
    setupMain(first_oid);

    EXPECT_THROW(
        gitmanip::Blame::file(*repo_, "nonexistent.txt"),
        gitmanip::GitError
    );
}

TEST_F(BlameTest, BlameInvalidLine) {
    auto first_oid = createCommit("First", {{"file.txt", "line1\nline2\n"}});
    setupMain(first_oid);

    auto blame = gitmanip::Blame::file(*repo_, "file.txt");

    // Line 0 should be invalid (lines are 1-indexed)
    auto line0 = blame.line(0);
    EXPECT_FALSE(line0.has_value());

    // Line 100 should be invalid (file only has 2 lines)
    auto line100 = blame.line(100);
    EXPECT_FALSE(line100.has_value());
}
