#include "test_fixture.hpp"

class TreeTest : public GitTestFixture {};

TEST_F(TreeTest, TreeEntryCount) {
    auto oid = createCommit("Initial", {
        {"file1.txt", "content1"},
        {"file2.txt", "content2"},
        {"file3.txt", "content3"}
    });

    auto commit = repo_->lookup_commit(oid);
    auto tree = commit.tree();

    EXPECT_EQ(tree.entry_count(), 3);
}

TEST_F(TreeTest, TreeEntryByName) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});

    auto commit = repo_->lookup_commit(oid);
    auto tree = commit.tree();

    auto entry = tree.entry_by_name("file.txt");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->name, "file.txt");
    EXPECT_EQ(entry->type, gitmanip::TreeEntry::Type::Blob);
}

TEST_F(TreeTest, TreeEntryByNameNotFound) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});

    auto commit = repo_->lookup_commit(oid);
    auto tree = commit.tree();

    auto entry = tree.entry_by_name("nonexistent.txt");
    EXPECT_FALSE(entry.has_value());
}

TEST_F(TreeTest, TreeEntryByIndex) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});

    auto commit = repo_->lookup_commit(oid);
    auto tree = commit.tree();

    auto entry = tree.entry_by_index(0);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->name, "file.txt");
}

TEST_F(TreeTest, TreeEntryByPath) {
    // Need to create a tree with nested directories
    gitmanip::TreeBuilder builder(*repo_);
    builder.insert_blob("src/main.cpp", "int main() {}");
    builder.insert_blob("src/lib/util.cpp", "void util() {}");
    builder.insert_blob("README.md", "# Readme");
    auto tree = builder.build();

    auto oid = repo_->create_commit("Initial", tree, {}, sig_, sig_);
    auto commit = repo_->lookup_commit(oid);
    auto final_tree = commit.tree();

    auto entry = final_tree.entry_by_path("src/main.cpp");
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->name, "main.cpp");

    auto nested_entry = final_tree.entry_by_path("src/lib/util.cpp");
    ASSERT_TRUE(nested_entry.has_value());
    EXPECT_EQ(nested_entry->name, "util.cpp");
}

TEST_F(TreeTest, TreeIterateEntries) {
    auto oid = createCommit("Initial", {
        {"a.txt", "a"},
        {"b.txt", "b"},
        {"c.txt", "c"}
    });

    auto commit = repo_->lookup_commit(oid);
    auto tree = commit.tree();

    std::vector<std::string> names;
    for (auto entry : tree.entries()) {
        names.push_back(entry.name);
    }

    EXPECT_EQ(names.size(), 3);
    EXPECT_TRUE(std::find(names.begin(), names.end(), "a.txt") != names.end());
    EXPECT_TRUE(std::find(names.begin(), names.end(), "b.txt") != names.end());
    EXPECT_TRUE(std::find(names.begin(), names.end(), "c.txt") != names.end());
}

TEST_F(TreeTest, TreeBlobContent) {
    auto oid = createCommit("Initial", {{"file.txt", "Hello, World!"}});

    auto commit = repo_->lookup_commit(oid);
    auto tree = commit.tree();

    auto content = tree.blob_content("file.txt");
    ASSERT_TRUE(content.has_value());
    EXPECT_EQ(content->size(), 13);

    auto content_str = tree.blob_content_string("file.txt");
    ASSERT_TRUE(content_str.has_value());
    EXPECT_EQ(*content_str, "Hello, World!");
}

TEST_F(TreeTest, TreeBlobContentNotFound) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});

    auto commit = repo_->lookup_commit(oid);
    auto tree = commit.tree();

    auto content = tree.blob_content("nonexistent.txt");
    EXPECT_FALSE(content.has_value());
}

TEST_F(TreeTest, TreeBuilderEmpty) {
    gitmanip::TreeBuilder builder(*repo_);
    builder.insert_blob("file.txt", "content");

    auto tree = builder.build();
    EXPECT_EQ(tree.entry_count(), 1);
}

TEST_F(TreeTest, TreeBuilderFromExisting) {
    auto oid = createCommit("Initial", {{"a.txt", "a"}, {"b.txt", "b"}});

    auto commit = repo_->lookup_commit(oid);
    gitmanip::TreeBuilder builder(*repo_, commit.tree());

    // Add a new file
    builder.insert_blob("c.txt", "c");

    auto tree = builder.build();
    EXPECT_EQ(tree.entry_count(), 3);
}

TEST_F(TreeTest, TreeBuilderRemove) {
    auto oid = createCommit("Initial", {{"a.txt", "a"}, {"b.txt", "b"}});

    auto commit = repo_->lookup_commit(oid);
    gitmanip::TreeBuilder builder(*repo_, commit.tree());

    builder.remove("b.txt");

    auto tree = builder.build();
    EXPECT_EQ(tree.entry_count(), 1);
    EXPECT_TRUE(tree.entry_by_name("a.txt").has_value());
    EXPECT_FALSE(tree.entry_by_name("b.txt").has_value());
}

TEST_F(TreeTest, TreeBuilderModify) {
    auto oid = createCommit("Initial", {{"file.txt", "original"}});

    auto commit = repo_->lookup_commit(oid);
    gitmanip::TreeBuilder builder(*repo_, commit.tree());

    builder.insert_blob("file.txt", "modified");

    auto tree = builder.build();
    auto content = tree.blob_content_string("file.txt");
    ASSERT_TRUE(content.has_value());
    EXPECT_EQ(*content, "modified");
}

TEST_F(TreeTest, TreeBuilderNestedPaths) {
    gitmanip::TreeBuilder builder(*repo_);
    builder.insert_blob("src/main.cpp", "main");
    builder.insert_blob("src/lib/util.cpp", "util");
    builder.insert_blob("include/header.hpp", "header");

    auto tree = builder.build();

    // Tree should have nested structure
    auto src_entry = tree.entry_by_name("src");
    ASSERT_TRUE(src_entry.has_value());
    EXPECT_EQ(src_entry->type, gitmanip::TreeEntry::Type::Tree);

    // Can access nested files by path
    auto main_content = tree.blob_content_string("src/main.cpp");
    ASSERT_TRUE(main_content.has_value());
    EXPECT_EQ(*main_content, "main");
}

TEST_F(TreeTest, TreeBuilderBinaryContent) {
    std::vector<uint8_t> binary_data = {0x00, 0x01, 0x02, 0xFF, 0xFE};

    gitmanip::TreeBuilder builder(*repo_);
    builder.insert_blob("binary.bin", binary_data);

    auto tree = builder.build();

    auto content = tree.blob_content("binary.bin");
    ASSERT_TRUE(content.has_value());
    EXPECT_EQ(*content, binary_data);
}

TEST_F(TreeTest, TreeId) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});

    auto commit = repo_->lookup_commit(oid);
    auto tree = commit.tree();

    // Tree should have a valid OID
    EXPECT_FALSE(tree.id().is_zero());
    EXPECT_EQ(tree.id().to_string().size(), 40);
}

TEST_F(TreeTest, EmptyTree) {
    auto empty_tree = repo_->empty_tree();

    EXPECT_EQ(empty_tree.entry_count(), 0);
    EXPECT_FALSE(empty_tree.id().is_zero());
}

TEST_F(TreeTest, TreeEntryFilemode) {
    gitmanip::TreeBuilder builder(*repo_);
    builder.insert_blob("file.txt", "content");

    auto tree = builder.build();
    auto entry = tree.entry_by_name("file.txt");

    ASSERT_TRUE(entry.has_value());
    // Default mode should be 0100644 (regular file)
    EXPECT_EQ(entry->filemode, 0100644);
}

TEST_F(TreeTest, LookupTree) {
    auto oid = createCommit("Initial", {{"file.txt", "content"}});

    auto commit = repo_->lookup_commit(oid);
    auto tree = commit.tree();

    // Look up tree by OID
    auto same_tree = repo_->lookup_tree(tree.id());

    EXPECT_EQ(same_tree.id(), tree.id());
    EXPECT_EQ(same_tree.entry_count(), tree.entry_count());
}
