#include "test_fixture.hpp"

class RemoteTest : public GitTestFixture {};

TEST_F(RemoteTest, AddRemote) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    gitmanip::Remote::add(*repo_, "origin", "https://github.com/user/repo.git");

    EXPECT_TRUE(gitmanip::Remote::exists(*repo_, "origin"));
}

TEST_F(RemoteTest, ListRemotes) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    gitmanip::Remote::add(*repo_, "origin", "https://github.com/user/repo.git");
    gitmanip::Remote::add(*repo_, "upstream", "https://github.com/upstream/repo.git");

    auto remotes = gitmanip::Remote::list(*repo_);
    EXPECT_EQ(remotes.size(), 2);

    auto names = gitmanip::Remote::names(*repo_);
    EXPECT_EQ(names.size(), 2);
    EXPECT_TRUE(std::find(names.begin(), names.end(), "origin") != names.end());
    EXPECT_TRUE(std::find(names.begin(), names.end(), "upstream") != names.end());
}

TEST_F(RemoteTest, GetRemote) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    gitmanip::Remote::add(*repo_, "origin", "https://github.com/user/repo.git");

    auto remote = gitmanip::Remote::get(*repo_, "origin");
    ASSERT_TRUE(remote.has_value());
    EXPECT_EQ(remote->name, "origin");
    EXPECT_EQ(remote->url, "https://github.com/user/repo.git");
}

TEST_F(RemoteTest, RemoveRemote) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    gitmanip::Remote::add(*repo_, "origin", "https://github.com/user/repo.git");
    EXPECT_TRUE(gitmanip::Remote::exists(*repo_, "origin"));

    gitmanip::Remote::remove(*repo_, "origin");
    EXPECT_FALSE(gitmanip::Remote::exists(*repo_, "origin"));
}

TEST_F(RemoteTest, RenameRemote) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    gitmanip::Remote::add(*repo_, "origin", "https://github.com/user/repo.git");
    gitmanip::Remote::rename(*repo_, "origin", "upstream");

    EXPECT_FALSE(gitmanip::Remote::exists(*repo_, "origin"));
    EXPECT_TRUE(gitmanip::Remote::exists(*repo_, "upstream"));
}

TEST_F(RemoteTest, SetUrl) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    gitmanip::Remote::add(*repo_, "origin", "https://github.com/user/repo.git");
    gitmanip::Remote::set_url(*repo_, "origin", "https://github.com/user/new-repo.git");

    auto remote = gitmanip::Remote::get(*repo_, "origin");
    ASSERT_TRUE(remote.has_value());
    EXPECT_EQ(remote->url, "https://github.com/user/new-repo.git");
}

class BranchTest : public GitTestFixture {};

TEST_F(BranchTest, ListBranches) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    auto commit = repo_->lookup_commit(first_oid);
    repo_->create_branch("feature1", commit);
    repo_->create_branch("feature2", commit);

    auto branches = gitmanip::Branch::list(*repo_);
    EXPECT_EQ(branches.size(), 3); // main + feature1 + feature2
}

TEST_F(BranchTest, GetBranch) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    auto branch = gitmanip::Branch::get(*repo_, "main");
    ASSERT_TRUE(branch.has_value());
    EXPECT_EQ(branch->name, "main");
    EXPECT_EQ(branch->target, first_oid);
    EXPECT_FALSE(branch->is_remote);
}

TEST_F(BranchTest, CurrentBranch) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    auto current = gitmanip::Branch::current(*repo_);
    ASSERT_TRUE(current.has_value());
    EXPECT_EQ(current->name, "main");
    EXPECT_TRUE(current->is_head);
}

TEST_F(BranchTest, CurrentBranchDetached) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    repo_->set_head_detached(first_oid);

    auto current = gitmanip::Branch::current(*repo_);
    EXPECT_FALSE(current.has_value());
}

TEST_F(BranchTest, BranchExists) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    EXPECT_TRUE(gitmanip::Branch::exists(*repo_, "main"));
    EXPECT_FALSE(gitmanip::Branch::exists(*repo_, "nonexistent"));
}

TEST_F(BranchTest, BranchIsHead) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    auto commit = repo_->lookup_commit(first_oid);
    repo_->create_branch("feature", commit);

    auto main_branch = gitmanip::Branch::get(*repo_, "main");
    ASSERT_TRUE(main_branch.has_value());
    EXPECT_TRUE(main_branch->is_head);

    auto feature_branch = gitmanip::Branch::get(*repo_, "feature");
    ASSERT_TRUE(feature_branch.has_value());
    EXPECT_FALSE(feature_branch->is_head);
}

class TagTest : public GitTestFixture {};

TEST_F(TagTest, CreateLightweightTag) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    auto commit = repo_->lookup_commit(first_oid);
    gitmanip::Tag::create_lightweight(*repo_, "v1.0.0", commit);

    EXPECT_TRUE(gitmanip::Tag::exists(*repo_, "v1.0.0"));

    auto tag_info = gitmanip::Tag::get(*repo_, "v1.0.0");
    ASSERT_TRUE(tag_info.has_value());
    EXPECT_EQ(tag_info->name, "v1.0.0");
    EXPECT_FALSE(tag_info->is_annotated);
    EXPECT_EQ(tag_info->commit_target, first_oid);
}

TEST_F(TagTest, CreateAnnotatedTag) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    auto commit = repo_->lookup_commit(first_oid);
    gitmanip::Tag::create_annotated(*repo_, "v1.0.0", commit, "Release 1.0.0");

    EXPECT_TRUE(gitmanip::Tag::exists(*repo_, "v1.0.0"));

    auto tag_info = gitmanip::Tag::get(*repo_, "v1.0.0");
    ASSERT_TRUE(tag_info.has_value());
    EXPECT_EQ(tag_info->name, "v1.0.0");
    EXPECT_TRUE(tag_info->is_annotated);
    EXPECT_TRUE(tag_info->message.has_value());
    // The message may or may not have a trailing newline depending on libgit2 version
    EXPECT_TRUE(tag_info->message->find("Release 1.0.0") != std::string::npos);
    EXPECT_EQ(tag_info->commit_target, first_oid);
}

TEST_F(TagTest, CreateAnnotatedTagWithTagger) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    auto commit = repo_->lookup_commit(first_oid);
    auto tagger = gitmanip::Signature::now("Tagger Name", "tagger@example.com");
    gitmanip::Tag::create_annotated(*repo_, "v1.0.0", commit, "Release", tagger);

    auto tag_info = gitmanip::Tag::get(*repo_, "v1.0.0");
    ASSERT_TRUE(tag_info.has_value());
    EXPECT_TRUE(tag_info->tagger.has_value());
    EXPECT_EQ(tag_info->tagger->name, "Tagger Name");
    EXPECT_EQ(tag_info->tagger->email, "tagger@example.com");
}

TEST_F(TagTest, ListTags) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    auto second_oid = createCommit("Second", {{"file.txt", "v2"}}, first_oid);
    setupMain(second_oid);

    auto first_commit = repo_->lookup_commit(first_oid);
    auto second_commit = repo_->lookup_commit(second_oid);

    gitmanip::Tag::create_lightweight(*repo_, "v1.0.0", first_commit);
    gitmanip::Tag::create_annotated(*repo_, "v2.0.0", second_commit, "Release 2");

    auto tags = gitmanip::Tag::list(*repo_);
    EXPECT_EQ(tags.size(), 2);

    auto names = gitmanip::Tag::names(*repo_);
    EXPECT_EQ(names.size(), 2);
    EXPECT_TRUE(std::find(names.begin(), names.end(), "v1.0.0") != names.end());
    EXPECT_TRUE(std::find(names.begin(), names.end(), "v2.0.0") != names.end());
}

TEST_F(TagTest, ListTagsWithPattern) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    auto commit = repo_->lookup_commit(first_oid);

    gitmanip::Tag::create_lightweight(*repo_, "v1.0.0", commit);
    gitmanip::Tag::create_lightweight(*repo_, "v1.1.0", commit);
    gitmanip::Tag::create_lightweight(*repo_, "v2.0.0", commit);
    gitmanip::Tag::create_lightweight(*repo_, "release-1", commit);

    auto v1_tags = gitmanip::Tag::names(*repo_, "v1.*");
    EXPECT_EQ(v1_tags.size(), 2);

    auto release_tags = gitmanip::Tag::names(*repo_, "release-*");
    EXPECT_EQ(release_tags.size(), 1);
}

TEST_F(TagTest, RemoveTag) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    auto commit = repo_->lookup_commit(first_oid);
    gitmanip::Tag::create_lightweight(*repo_, "v1.0.0", commit);

    EXPECT_TRUE(gitmanip::Tag::exists(*repo_, "v1.0.0"));

    gitmanip::Tag::remove(*repo_, "v1.0.0");

    EXPECT_FALSE(gitmanip::Tag::exists(*repo_, "v1.0.0"));
}

TEST_F(TagTest, TagsPointingTo) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"b.txt", "b"}}, first_oid);
    setupMain(second_oid);

    auto first_commit = repo_->lookup_commit(first_oid);
    auto second_commit = repo_->lookup_commit(second_oid);

    gitmanip::Tag::create_lightweight(*repo_, "v1.0", first_commit);
    gitmanip::Tag::create_annotated(*repo_, "v1.0-annotated", first_commit, "v1");
    gitmanip::Tag::create_lightweight(*repo_, "v2.0", second_commit);

    auto tags_at_first = gitmanip::Tag::pointing_to(*repo_, first_oid);
    EXPECT_EQ(tags_at_first.size(), 2);

    auto tags_at_second = gitmanip::Tag::pointing_to(*repo_, second_oid);
    EXPECT_EQ(tags_at_second.size(), 1);
    EXPECT_EQ(tags_at_second[0], "v2.0");
}

TEST_F(TagTest, ForceCreateTag) {
    auto first_oid = createCommit("First", {{"a.txt", "a"}});
    auto second_oid = createCommit("Second", {{"b.txt", "b"}}, first_oid);
    setupMain(second_oid);

    auto first_commit = repo_->lookup_commit(first_oid);
    auto second_commit = repo_->lookup_commit(second_oid);

    gitmanip::Tag::create_lightweight(*repo_, "v1.0", first_commit);

    // Without force, should throw
    EXPECT_THROW(
        gitmanip::Tag::create_lightweight(*repo_, "v1.0", second_commit, false),
        gitmanip::GitError
    );

    // With force, should succeed
    EXPECT_NO_THROW(
        gitmanip::Tag::create_lightweight(*repo_, "v1.0", second_commit, true)
    );

    // Tag should now point to second commit
    auto tag_info = gitmanip::Tag::get(*repo_, "v1.0");
    ASSERT_TRUE(tag_info.has_value());
    EXPECT_EQ(tag_info->commit_target, second_oid);
}

TEST_F(TagTest, GetNonexistentTag) {
    auto first_oid = createCommit("Initial", {{"file.txt", "content"}});
    setupMain(first_oid);

    auto tag_info = gitmanip::Tag::get(*repo_, "nonexistent");
    EXPECT_FALSE(tag_info.has_value());
}
