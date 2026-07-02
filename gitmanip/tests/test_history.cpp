#include "test_fixture.hpp"
#include <set>

class FileHistoryTest : public GitTestFixture {};

TEST_F(FileHistoryTest, TraceSimpleHistory) {
    auto first_oid = createCommit("Add file", {{"file.txt", "v1\n"}});
    auto second_oid = createCommit("Modify file", {{"file.txt", "v2\n"}}, first_oid);
    auto third_oid = createCommit("Modify again", {{"file.txt", "v3\n"}}, second_oid);
    setupMain(third_oid);

    auto history = gitmanip::FileHistory::trace(*repo_, "file.txt");
    auto entries = history.entries();

    // Should have 3 entries (one per commit that touched the file)
    EXPECT_EQ(entries.size(), 3);

    // Entries contain all three commits - order depends on walking order
    std::set<gitmanip::Oid> expected_oids = {first_oid, second_oid, third_oid};
    std::set<gitmanip::Oid> actual_oids;
    for (const auto& e : entries) {
        actual_oids.insert(e.commit_id);
    }
    EXPECT_EQ(actual_oids, expected_oids);
}

TEST_F(FileHistoryTest, FindIntroduction) {
    auto first_oid = createCommit("Base", {{"other.txt", "other"}});
    auto second_oid = createCommit("Add file", {{"other.txt", "other"}, {"new.txt", "content"}}, first_oid);
    auto third_oid = createCommit("Modify", {{"other.txt", "other"}, {"new.txt", "modified"}}, second_oid);
    setupMain(third_oid);

    auto intro = gitmanip::FileHistory::find_introduction(*repo_, "new.txt");
    ASSERT_TRUE(intro.has_value());
    EXPECT_EQ(*intro, second_oid);
}

TEST_F(FileHistoryTest, FindIntroductionRootCommit) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    auto intro = gitmanip::FileHistory::find_introduction(*repo_, "file.txt");
    ASSERT_TRUE(intro.has_value());
    EXPECT_EQ(*intro, first_oid);
}

TEST_F(FileHistoryTest, FindDeletion) {
    auto first_oid = createCommit("Add file", {{"file.txt", "content"}});

    // Create commit that removes the file
    gitmanip::TreeBuilder builder(*repo_);
    builder.insert_blob("other.txt", "other");
    auto tree = builder.build();
    auto first = repo_->lookup_commit(first_oid);
    std::vector<const gitmanip::Commit*> parents = {&first};
    auto second_oid = repo_->create_commit("Delete file", tree, parents, sig_, sig_);

    setupMain(second_oid);

    auto deletion = gitmanip::FileHistory::find_deletion(*repo_, "file.txt");
    ASSERT_TRUE(deletion.has_value());
    EXPECT_EQ(*deletion, second_oid);
}

TEST_F(FileHistoryTest, FindDeletionNotDeleted) {
    auto first_oid = createCommit("Add file", {{"file.txt", "content"}});
    setupMain(first_oid);

    auto deletion = gitmanip::FileHistory::find_deletion(*repo_, "file.txt");
    EXPECT_FALSE(deletion.has_value());
}

TEST_F(FileHistoryTest, HistoryEntryChangeTypes) {
    auto first_oid = createCommit("Add", {{"file.txt", "v1\n"}});
    auto second_oid = createCommit("Modify", {{"file.txt", "v2\n"}}, first_oid);
    setupMain(second_oid);

    auto history = gitmanip::FileHistory::trace(*repo_, "file.txt");
    auto entries = history.entries();

    ASSERT_EQ(entries.size(), 2);

    // First entry (most recent) should be Modified
    EXPECT_EQ(entries[0].change_type, gitmanip::FileHistoryEntry::ChangeType::Modified);

    // Second entry should be Added
    EXPECT_EQ(entries[1].change_type, gitmanip::FileHistoryEntry::ChangeType::Added);
}

TEST_F(FileHistoryTest, HistoryWithMaxCommits) {
    // Create a long history
    auto oids = createLinearHistory(10);
    setupMain(oids.back());

    gitmanip::FileHistoryOptions opts;
    opts.max_commits = 3;

    // Note: Each commit only adds one new file, so we need to trace a file that exists throughout
    auto first_oid = createCommit("Base", {{"tracked.txt", "v0"}});
    auto second_oid = createCommit("M1", {{"tracked.txt", "v1"}}, first_oid);
    auto third_oid = createCommit("M2", {{"tracked.txt", "v2"}}, second_oid);
    auto fourth_oid = createCommit("M3", {{"tracked.txt", "v3"}}, third_oid);
    auto fifth_oid = createCommit("M4", {{"tracked.txt", "v4"}}, fourth_oid);
    setupMain(fifth_oid);

    auto history = gitmanip::FileHistory::trace(*repo_, "tracked.txt", opts);
    auto entries = history.entries();

    EXPECT_LE(entries.size(), 3);
}

class CommitSearchTest : public GitTestFixture {};

TEST_F(CommitSearchTest, SearchByMessage) {
    auto first_oid = createCommit("Fix bug in parser", {{"a.txt", "a"}});
    auto second_oid = createCommit("Add new feature", {{"b.txt", "b"}}, first_oid);
    auto third_oid = createCommit("Fix another bug", {{"c.txt", "c"}}, second_oid);
    setupMain(third_oid);

    auto commits = gitmanip::CommitSearch::by_message(*repo_, "bug");

    EXPECT_EQ(commits.size(), 2);
    // Should find both "Fix bug" commits
    bool found_first = false;
    bool found_third = false;
    for (const auto& c : commits) {
        if (c.id() == first_oid) found_first = true;
        if (c.id() == third_oid) found_third = true;
    }
    EXPECT_TRUE(found_first);
    EXPECT_TRUE(found_third);
}

TEST_F(CommitSearchTest, SearchByMessageCaseInsensitive) {
    auto first_oid = createCommit("FIX BUG", {{"a.txt", "a"}});
    auto second_oid = createCommit("fix bug", {{"b.txt", "b"}}, first_oid);
    setupMain(second_oid);

    gitmanip::CommitSearchOptions opts;
    opts.case_insensitive = true;

    auto commits = gitmanip::CommitSearch::by_message(*repo_, "Fix Bug", opts);

    EXPECT_EQ(commits.size(), 2);
}

TEST_F(CommitSearchTest, SearchByMessageRegex) {
    auto first_oid = createCommit("JIRA-123: Fix bug", {{"a.txt", "a"}});
    auto second_oid = createCommit("JIRA-456: Add feature", {{"b.txt", "b"}}, first_oid);
    auto third_oid = createCommit("Random commit", {{"c.txt", "c"}}, second_oid);
    setupMain(third_oid);

    auto commits = gitmanip::CommitSearch::by_message_regex(*repo_, "JIRA-\\d+");

    EXPECT_EQ(commits.size(), 2);
}

TEST_F(CommitSearchTest, SearchTouchingFile) {
    auto first_oid = createCommit("Add file", {{"target.txt", "v1"}});
    auto second_oid = createCommit("Add other", {{"target.txt", "v1"}, {"other.txt", "other"}}, first_oid);
    auto third_oid = createCommit("Modify target", {{"target.txt", "v2"}, {"other.txt", "other"}}, second_oid);
    setupMain(third_oid);

    auto commits = gitmanip::CommitSearch::touching_file(*repo_, "target.txt");

    // Should find first (added) and third (modified) commits
    EXPECT_GE(commits.size(), 2);

    bool found_first = false;
    bool found_third = false;
    for (const auto& c : commits) {
        if (c.id() == first_oid) found_first = true;
        if (c.id() == third_oid) found_third = true;
    }
    EXPECT_TRUE(found_first);
    EXPECT_TRUE(found_third);
}

TEST_F(CommitSearchTest, SearchByContent) {
    auto first_oid = createCommit("Add function", {{"code.cpp", "void deprecated_function() {}"}});
    auto second_oid = createCommit("Add another", {{"code.cpp", "void deprecated_function() {}\nvoid other() {}"}}, first_oid);
    setupMain(second_oid);

    auto commits = gitmanip::CommitSearch::by_content(*repo_, "deprecated_function");

    // Should find the commit that introduced this text
    EXPECT_GE(commits.size(), 1);
}

TEST_F(CommitSearchTest, SearchWithMaxResults) {
    auto oids = createLinearHistory(10);
    setupMain(oids.back());

    gitmanip::CommitSearchOptions opts;
    opts.max_results = 5;

    auto commits = gitmanip::CommitSearch::by_message(*repo_, "Commit", opts);

    EXPECT_LE(commits.size(), 5);
}

TEST_F(CommitSearchTest, SearchMergeBase) {
    // Create divergent branches
    auto base_oid = createCommit("Base", {{"base.txt", "base"}});
    auto left_oid = createCommit("Left", {{"base.txt", "base"}, {"left.txt", "left"}}, base_oid);
    auto right_oid = createCommit("Right", {{"base.txt", "base"}, {"right.txt", "right"}}, base_oid);

    auto merge_base = gitmanip::CommitSearch::merge_base(*repo_, left_oid, right_oid);
    ASSERT_TRUE(merge_base.has_value());
    EXPECT_EQ(*merge_base, base_oid);
}

TEST_F(CommitSearchTest, SearchIsAncestor) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"b.txt", "b"}}, first_oid);
    auto third_oid = createCommit("Third", {{"c.txt", "c"}}, second_oid);

    EXPECT_TRUE(gitmanip::CommitSearch::is_ancestor(*repo_, first_oid, third_oid));
    EXPECT_TRUE(gitmanip::CommitSearch::is_ancestor(*repo_, second_oid, third_oid));
    EXPECT_FALSE(gitmanip::CommitSearch::is_ancestor(*repo_, third_oid, first_oid));
}
