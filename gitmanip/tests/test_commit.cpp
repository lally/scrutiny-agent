#include <gitmanip/gitmanip.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class CommitTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "gitmanip_commit_test";
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

TEST_F(CommitTest, CommitMessageParsing) {
    auto oid = createCommit("First line\n\nBody paragraph 1\n\nBody paragraph 2",
                            {{"file.txt", "content"}});

    auto commit = repo_->lookup_commit(oid);
    EXPECT_EQ(commit.summary(), "First line");
    EXPECT_TRUE(commit.body().find("Body paragraph") != std::string::npos);
}

TEST_F(CommitTest, CommitTree) {
    auto oid = createCommit("Test commit",
                            {{"file1.txt", "content1"},
                             {"file2.txt", "content2"}});

    auto commit = repo_->lookup_commit(oid);
    auto tree = commit.tree();

    EXPECT_EQ(tree.entry_count(), 2);

    auto entry1 = tree.entry_by_name("file1.txt");
    ASSERT_TRUE(entry1.has_value());
    EXPECT_EQ(entry1->name, "file1.txt");

    auto content = tree.blob_content_string("file1.txt");
    ASSERT_TRUE(content.has_value());
    EXPECT_EQ(*content, "content1");
}

TEST_F(CommitTest, CommitParents) {
    auto first_oid = createCommit("First commit", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second commit", {{"b.txt", "b"}}, first_oid);
    auto third_oid = createCommit("Third commit", {{"c.txt", "c"}}, second_oid);

    auto third = repo_->lookup_commit(third_oid);
    EXPECT_EQ(third.parent_count(), 1);

    auto second = third.parent(0);
    EXPECT_EQ(second.id(), second_oid);

    auto first = second.parent(0);
    EXPECT_EQ(first.id(), first_oid);
    EXPECT_EQ(first.parent_count(), 0);
}

TEST_F(CommitTest, CommitWalker) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"b.txt", "b"}}, first_oid);
    auto third_oid = createCommit("Third", {{"c.txt", "c"}}, second_oid);

    // Update HEAD
    repo_->update_ref("refs/heads/main", third_oid);
    repo_->set_head("refs/heads/main");

    auto walker = repo_->walk_commits();
    walker.push_head();
    walker.sort(gitmanip::CommitWalker::SortOrder::Topological);

    auto commits = walker.walk();
    EXPECT_EQ(commits.size(), 3);
    EXPECT_EQ(commits[0].id(), third_oid);
    EXPECT_EQ(commits[1].id(), second_oid);
    EXPECT_EQ(commits[2].id(), first_oid);
}

TEST_F(CommitTest, DiffFromParent) {
    auto first_oid = createCommit("First", {{"file.txt", "line1\nline2\n"}});
    auto second_oid = createCommit("Second", {{"file.txt", "line1\nline2\nline3\n"}}, first_oid);

    auto second = repo_->lookup_commit(second_oid);
    auto diff = second.diff_from_parent();

    EXPECT_EQ(diff.num_deltas(), 1);

    auto delta = diff.delta(0);
    ASSERT_TRUE(delta.has_value());
    EXPECT_EQ(delta->status, gitmanip::DiffDelta::Status::Modified);
    EXPECT_EQ(delta->new_path, "file.txt");

    auto stats = diff.stats();
    EXPECT_EQ(stats.insertions, 1);
    EXPECT_EQ(stats.deletions, 0);
}

TEST_F(CommitTest, IsAncestorOf) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"b.txt", "b"}}, first_oid);
    auto third_oid = createCommit("Third", {{"c.txt", "c"}}, second_oid);

    auto first = repo_->lookup_commit(first_oid);
    auto third = repo_->lookup_commit(third_oid);

    EXPECT_TRUE(first.is_ancestor_of(third));
    EXPECT_FALSE(third.is_ancestor_of(first));
}
