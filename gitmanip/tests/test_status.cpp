#include "test_fixture.hpp"

class StatusTest : public GitTestFixture {};

TEST_F(StatusTest, EmptyRepository) {
    // Fresh repo with no commits should have no status entries
    auto entries = gitmanip::Status::list(*repo_);
    EXPECT_TRUE(entries.empty());
}

TEST_F(StatusTest, NoChanges) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid);

    // Checkout to sync working directory
    repo_->checkout_head(true);

    auto entries = gitmanip::Status::list(*repo_);
    EXPECT_TRUE(entries.empty());
}

TEST_F(StatusTest, UntrackedFile) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid);
    repo_->checkout_head(true);

    // Create an untracked file
    writeFile("untracked.txt", "new content");

    auto entries = gitmanip::Status::list(*repo_);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].path, "untracked.txt");
    EXPECT_FALSE(entries[0].index_status.has_value());
    ASSERT_TRUE(entries[0].workdir_status.has_value());
    EXPECT_EQ(*entries[0].workdir_status, gitmanip::FileState::New);
}

TEST_F(StatusTest, ModifiedFile) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid);
    repo_->checkout_head(true);

    // Modify the tracked file
    writeFile("file.txt", "modified content");

    auto entries = gitmanip::Status::list(*repo_);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].path, "file.txt");
    EXPECT_FALSE(entries[0].index_status.has_value());
    ASSERT_TRUE(entries[0].workdir_status.has_value());
    EXPECT_EQ(*entries[0].workdir_status, gitmanip::FileState::Modified);
}

TEST_F(StatusTest, StagedFile) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid);
    repo_->checkout_head(true);

    // Modify and stage
    writeFile("file.txt", "modified content");
    repo_->add_to_index("file.txt");

    auto entries = gitmanip::Status::list(*repo_);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].path, "file.txt");
    ASSERT_TRUE(entries[0].index_status.has_value());
    EXPECT_EQ(*entries[0].index_status, gitmanip::FileState::Modified);
    EXPECT_FALSE(entries[0].workdir_status.has_value());
}

TEST_F(StatusTest, PartiallyStaged) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid);
    repo_->checkout_head(true);

    // Modify and stage
    writeFile("file.txt", "modified content");
    repo_->add_to_index("file.txt");

    // Modify again without staging
    writeFile("file.txt", "modified again");

    auto entries = gitmanip::Status::list(*repo_);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].path, "file.txt");
    ASSERT_TRUE(entries[0].index_status.has_value());
    EXPECT_EQ(*entries[0].index_status, gitmanip::FileState::Modified);
    ASSERT_TRUE(entries[0].workdir_status.has_value());
    EXPECT_EQ(*entries[0].workdir_status, gitmanip::FileState::Modified);
}

TEST_F(StatusTest, NewStagedFile) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid);
    repo_->checkout_head(true);

    // Create and stage a new file
    writeFile("new.txt", "new content");
    repo_->add_to_index("new.txt");

    auto entries = gitmanip::Status::list(*repo_);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].path, "new.txt");
    ASSERT_TRUE(entries[0].index_status.has_value());
    EXPECT_EQ(*entries[0].index_status, gitmanip::FileState::New);
}

TEST_F(StatusTest, DeletedFile) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid);
    repo_->checkout_head(true);

    // Delete the file from working directory
    fs::remove(test_dir_ / "file.txt");

    auto entries = gitmanip::Status::list(*repo_);
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].path, "file.txt");
    ASSERT_TRUE(entries[0].workdir_status.has_value());
    EXPECT_EQ(*entries[0].workdir_status, gitmanip::FileState::Deleted);
}

TEST_F(StatusTest, HasChanges) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid);
    repo_->checkout_head(true);

    EXPECT_FALSE(gitmanip::Status::has_changes(*repo_));

    writeFile("file.txt", "modified content");
    EXPECT_TRUE(gitmanip::Status::has_changes(*repo_));
}

TEST_F(StatusTest, HasStaged) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid);
    repo_->checkout_head(true);

    EXPECT_FALSE(gitmanip::Status::has_staged(*repo_));

    writeFile("file.txt", "modified content");
    EXPECT_FALSE(gitmanip::Status::has_staged(*repo_));

    repo_->add_to_index("file.txt");
    EXPECT_TRUE(gitmanip::Status::has_staged(*repo_));
}

TEST_F(StatusTest, HasUnstaged) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid);
    repo_->checkout_head(true);

    EXPECT_FALSE(gitmanip::Status::has_unstaged(*repo_));

    writeFile("file.txt", "modified content");
    EXPECT_TRUE(gitmanip::Status::has_unstaged(*repo_));

    repo_->add_to_index("file.txt");
    EXPECT_FALSE(gitmanip::Status::has_unstaged(*repo_));
}

TEST_F(StatusTest, HasUntracked) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid);
    repo_->checkout_head(true);

    EXPECT_FALSE(gitmanip::Status::has_untracked(*repo_));

    writeFile("untracked.txt", "new content");
    EXPECT_TRUE(gitmanip::Status::has_untracked(*repo_));
}

TEST_F(StatusTest, ExcludeUntracked) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid);
    repo_->checkout_head(true);

    writeFile("untracked.txt", "new content");

    gitmanip::StatusOptions opts;
    opts.include_untracked = false;

    auto entries = gitmanip::Status::list(*repo_, opts);
    EXPECT_TRUE(entries.empty());
}

TEST_F(StatusTest, MultipleFiles) {
    auto oid = createCommit("Initial", {
        {"file1.txt", "content1"},
        {"file2.txt", "content2"},
        {"file3.txt", "content3"}
    });
    setupMain(oid);
    repo_->checkout_head(true);

    // Modify file1 and stage
    writeFile("file1.txt", "modified1");
    repo_->add_to_index("file1.txt");

    // Modify file2 without staging
    writeFile("file2.txt", "modified2");

    // Create untracked file
    writeFile("file4.txt", "new content");

    auto entries = gitmanip::Status::list(*repo_);
    EXPECT_EQ(entries.size(), 3);

    // Find each entry
    auto find_entry = [&entries](const std::string& path) -> const gitmanip::StatusEntry* {
        for (const auto& e : entries) {
            if (e.path == path) return &e;
        }
        return nullptr;
    };

    auto* e1 = find_entry("file1.txt");
    ASSERT_NE(e1, nullptr);
    EXPECT_TRUE(e1->index_status.has_value());
    EXPECT_FALSE(e1->workdir_status.has_value());

    auto* e2 = find_entry("file2.txt");
    ASSERT_NE(e2, nullptr);
    EXPECT_FALSE(e2->index_status.has_value());
    EXPECT_TRUE(e2->workdir_status.has_value());

    auto* e4 = find_entry("file4.txt");
    ASSERT_NE(e4, nullptr);
    EXPECT_FALSE(e4->index_status.has_value());
    EXPECT_TRUE(e4->workdir_status.has_value());
    EXPECT_EQ(*e4->workdir_status, gitmanip::FileState::New);
}

class SyncStatusTest : public GitTestFixture {
protected:
    void SetUp() override {
        GitTestFixture::SetUp();

        // Create a remote repository
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dist(10000, 99999);
        remote_dir_ = fs::temp_directory_path() / ("gitmanip_remote_" + std::to_string(dist(gen)));
        fs::create_directories(remote_dir_);

        remote_repo_ = std::make_unique<gitmanip::Repository>(
            gitmanip::Repository::init(remote_dir_, true));
    }

    void TearDown() override {
        remote_repo_.reset();
        GitTestFixture::TearDown();
        std::error_code ec;
        fs::remove_all(remote_dir_, ec);
    }

    // Setup a remote and tracking branch
    void setupRemoteTracking(const gitmanip::Oid& oid) {
        // Add remote
        gitmanip::Remote::add(*repo_, "origin", remote_dir_.string());

        // Create a remote-tracking branch by updating refs/remotes/origin/main
        repo_->update_ref("refs/remotes/origin/main", oid);

        // Set upstream for main
        gitmanip::Branch::set_upstream(*repo_, "main", "origin/main");
    }

    fs::path remote_dir_;
    std::unique_ptr<gitmanip::Repository> remote_repo_;
};

TEST_F(SyncStatusTest, InSync) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid);

    setupRemoteTracking(oid);

    auto sync = gitmanip::Status::sync(*repo_);
    ASSERT_TRUE(sync.has_value());
    EXPECT_EQ(sync->upstream, "origin/main");
    EXPECT_EQ(sync->ahead_count(), 0);
    EXPECT_EQ(sync->behind_count(), 0);
    EXPECT_TRUE(sync->is_synced());
}

TEST_F(SyncStatusTest, LocalAhead) {
    auto oid1 = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid1);

    setupRemoteTracking(oid1);

    // Create more commits locally
    auto oid2 = createCommit("Second", {{"file2.txt", "content"}}, oid1);
    auto oid3 = createCommit("Third", {{"file3.txt", "content"}}, oid2);
    repo_->update_ref("refs/heads/main", oid3);

    auto sync = gitmanip::Status::sync(*repo_);
    ASSERT_TRUE(sync.has_value());
    EXPECT_EQ(sync->ahead_count(), 2);
    EXPECT_EQ(sync->behind_count(), 0);
    EXPECT_FALSE(sync->is_synced());

    // Check the actual commits
    EXPECT_EQ(sync->ahead.size(), 2);
    // The commits should be in topological order (newest first)
    EXPECT_TRUE(sync->ahead[0] == oid3 || sync->ahead[0] == oid2);
}

TEST_F(SyncStatusTest, LocalBehind) {
    auto oid1 = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid1);

    // Create commits that will be "on the remote"
    auto oid2 = createCommit("Second", {{"file2.txt", "content"}}, oid1);
    auto oid3 = createCommit("Third", {{"file3.txt", "content"}}, oid2);

    // Set up remote tracking to point to oid3 (simulating remote has more commits)
    gitmanip::Remote::add(*repo_, "origin", remote_dir_.string());
    repo_->update_ref("refs/remotes/origin/main", oid3);
    gitmanip::Branch::set_upstream(*repo_, "main", "origin/main");

    // Local main stays at oid1
    // (already set by setupMain)

    auto sync = gitmanip::Status::sync(*repo_);
    ASSERT_TRUE(sync.has_value());
    EXPECT_EQ(sync->ahead_count(), 0);
    EXPECT_EQ(sync->behind_count(), 2);
    EXPECT_FALSE(sync->is_synced());

    // Check the actual commits
    EXPECT_EQ(sync->behind.size(), 2);
}

TEST_F(SyncStatusTest, Diverged) {
    auto oid1 = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid1);

    // Create local commits
    auto local2 = createCommit("Local 2", {{"local.txt", "content"}}, oid1);
    repo_->update_ref("refs/heads/main", local2);

    // Create "remote" commits (branching from oid1)
    auto remote2 = createCommit("Remote 2", {{"remote.txt", "content"}}, oid1);

    // Set up remote tracking
    gitmanip::Remote::add(*repo_, "origin", remote_dir_.string());
    repo_->update_ref("refs/remotes/origin/main", remote2);
    gitmanip::Branch::set_upstream(*repo_, "main", "origin/main");

    auto sync = gitmanip::Status::sync(*repo_);
    ASSERT_TRUE(sync.has_value());
    EXPECT_EQ(sync->ahead_count(), 1);
    EXPECT_EQ(sync->behind_count(), 1);
    EXPECT_FALSE(sync->is_synced());
}

TEST_F(SyncStatusTest, NoUpstream) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid);

    // Don't set up any remote/upstream

    auto sync = gitmanip::Status::sync(*repo_);
    EXPECT_FALSE(sync.has_value());
}

TEST_F(SyncStatusTest, DetachedHead) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});
    repo_->set_head_detached(oid);

    auto sync = gitmanip::Status::sync(*repo_);
    EXPECT_FALSE(sync.has_value());
}

TEST_F(SyncStatusTest, SyncByBranchName) {
    auto oid1 = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(oid1);

    auto oid2 = createCommit("Second", {{"file2.txt", "content"}}, oid1);
    auto commit = repo_->lookup_commit(oid2);
    repo_->create_branch("feature", commit);

    // Set up remote tracking for feature branch
    gitmanip::Remote::add(*repo_, "origin", remote_dir_.string());
    repo_->update_ref("refs/remotes/origin/feature", oid1);
    gitmanip::Branch::set_upstream(*repo_, "feature", "origin/feature");

    auto sync = gitmanip::Status::sync(*repo_, "feature");
    ASSERT_TRUE(sync.has_value());
    EXPECT_EQ(sync->upstream, "origin/feature");
    EXPECT_EQ(sync->ahead_count(), 1);
    EXPECT_EQ(sync->behind_count(), 0);
}

TEST_F(SyncStatusTest, FileStateToString) {
    EXPECT_STREQ(gitmanip::to_string(gitmanip::FileState::New), "new");
    EXPECT_STREQ(gitmanip::to_string(gitmanip::FileState::Modified), "modified");
    EXPECT_STREQ(gitmanip::to_string(gitmanip::FileState::Deleted), "deleted");
    EXPECT_STREQ(gitmanip::to_string(gitmanip::FileState::Renamed), "renamed");
    EXPECT_STREQ(gitmanip::to_string(gitmanip::FileState::Copied), "copied");
    EXPECT_STREQ(gitmanip::to_string(gitmanip::FileState::TypeChange), "typechange");
    EXPECT_STREQ(gitmanip::to_string(gitmanip::FileState::Ignored), "ignored");
    EXPECT_STREQ(gitmanip::to_string(gitmanip::FileState::Conflicted), "conflicted");
}
