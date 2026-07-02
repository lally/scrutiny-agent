#include <gitmanip/gitmanip.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class RepositoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a temporary directory for test repos
        test_dir_ = fs::temp_directory_path() / "gitmanip_test";
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        // Clean up
        fs::remove_all(test_dir_);
    }

    fs::path test_dir_;
};

TEST_F(RepositoryTest, InitBareRepository) {
    auto repo_path = test_dir_ / "bare.git";
    auto repo = gitmanip::Repository::init(repo_path, true);

    EXPECT_TRUE(repo.is_bare());
    EXPECT_TRUE(repo.is_empty());
    EXPECT_TRUE(fs::exists(repo_path / "HEAD"));
}

TEST_F(RepositoryTest, InitNonBareRepository) {
    auto repo_path = test_dir_ / "normal";
    auto repo = gitmanip::Repository::init(repo_path, false);

    EXPECT_FALSE(repo.is_bare());
    EXPECT_TRUE(repo.is_empty());
    EXPECT_TRUE(fs::exists(repo_path / ".git"));
}

TEST_F(RepositoryTest, OpenExistingRepository) {
    auto repo_path = test_dir_ / "existing";
    {
        auto repo = gitmanip::Repository::init(repo_path, false);
    }

    auto repo = gitmanip::Repository::open(repo_path);
    EXPECT_FALSE(repo.is_bare());
}

TEST_F(RepositoryTest, DiscoverRepository) {
    auto repo_path = test_dir_ / "discover_test";
    auto subdir = repo_path / "a" / "b" / "c";
    fs::create_directories(subdir);

    {
        auto repo = gitmanip::Repository::init(repo_path, false);
    }

    auto repo = gitmanip::Repository::discover(subdir);
    EXPECT_EQ(fs::canonical(repo.workdir()), fs::canonical(repo_path));
}

TEST_F(RepositoryTest, CreateAndLookupCommit) {
    auto repo_path = test_dir_ / "commit_test";
    auto repo = gitmanip::Repository::init(repo_path, false);

    // Create a file
    std::ofstream(repo_path / "test.txt") << "Hello, World!";

    // Add to index
    repo.add_to_index("test.txt");

    // Write tree
    auto tree = repo.write_index_as_tree();

    // Create commit
    auto sig = gitmanip::Signature::now("Test User", "test@example.com");
    auto oid = repo.create_commit("Initial commit", tree, {}, sig, sig);

    // Lookup commit
    auto commit = repo.lookup_commit(oid);
    EXPECT_EQ(commit.message(), "Initial commit");
    EXPECT_EQ(commit.author().name, "Test User");
    EXPECT_EQ(commit.parent_count(), 0);
}

TEST_F(RepositoryTest, BranchOperations) {
    auto repo_path = test_dir_ / "branch_test";
    auto repo = gitmanip::Repository::init(repo_path, false);

    // Create initial commit
    std::ofstream(repo_path / "test.txt") << "content";
    repo.add_to_index("test.txt");
    auto tree = repo.write_index_as_tree();
    auto sig = gitmanip::Signature::now("Test", "test@test.com");
    auto oid = repo.create_commit("Initial", tree, {}, sig, sig);

    // Update HEAD to point to the commit
    repo.update_ref("refs/heads/main", oid);
    repo.set_head("refs/heads/main");

    auto commit = repo.lookup_commit(oid);

    // Create a branch
    repo.create_branch("feature", commit);

    auto branches = repo.branch_names();
    EXPECT_EQ(branches.size(), 2);

    // Delete branch
    repo.delete_branch("feature");
    branches = repo.branch_names();
    EXPECT_EQ(branches.size(), 1);
}

TEST_F(RepositoryTest, BlobOperations) {
    auto repo_path = test_dir_ / "blob_test";
    auto repo = gitmanip::Repository::init(repo_path, false);

    std::string content = "Hello, World!";
    auto oid = repo.create_blob(content);

    auto retrieved = repo.blob_content_string(oid);
    EXPECT_EQ(retrieved, content);
}
