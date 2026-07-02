#pragma once

#include <gitmanip/gitmanip.hpp>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

// Base test fixture that creates a temporary git repository with helper methods
class GitTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // Create unique temp directory for this test
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dist(10000, 99999);
        test_dir_ = fs::temp_directory_path() / ("gitmanip_test_" + std::to_string(dist(gen)));
        fs::create_directories(test_dir_);

        repo_ = std::make_unique<gitmanip::Repository>(
            gitmanip::Repository::init(test_dir_, false));

        sig_ = gitmanip::Signature::now("Test User", "test@example.com");
    }

    void TearDown() override {
        repo_.reset();
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    // Create a commit with the given message and file contents
    // If parent is provided, the commit will have that as its parent
    gitmanip::Oid createCommit(
        const std::string& message,
        const std::vector<std::pair<std::string, std::string>>& files,
        std::optional<gitmanip::Oid> parent = std::nullopt) {

        gitmanip::TreeBuilder builder(*repo_);

        // If we have a parent, start from its tree
        if (parent) {
            auto parent_commit = repo_->lookup_commit(*parent);
            builder = gitmanip::TreeBuilder(*repo_, parent_commit.tree());
        }

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

    // Create a merge commit with two parents
    gitmanip::Oid createMergeCommit(
        const std::string& message,
        const gitmanip::Oid& parent1,
        const gitmanip::Oid& parent2,
        const std::vector<std::pair<std::string, std::string>>& additional_files = {}) {

        auto p1 = repo_->lookup_commit(parent1);
        auto p2 = repo_->lookup_commit(parent2);

        // Start with parent1's tree
        gitmanip::TreeBuilder builder(*repo_, p1.tree());

        // Merge in files from parent2's tree (simplified: just copies them)
        for (auto entry : p2.tree().entries()) {
            auto content = p2.tree().blob_content(entry.name);
            if (content) {
                builder.insert_blob(entry.name, *content);
            }
        }

        // Add any additional files
        for (const auto& [name, content] : additional_files) {
            builder.insert_blob(name, content);
        }

        auto tree = builder.build();

        std::vector<const gitmanip::Commit*> parents = {&p1, &p2};
        return repo_->create_commit(message, tree, parents, sig_, sig_);
    }

    // Create a linear history of commits
    std::vector<gitmanip::Oid> createLinearHistory(size_t count, const std::string& prefix = "Commit") {
        std::vector<gitmanip::Oid> oids;
        std::optional<gitmanip::Oid> parent;

        for (size_t i = 0; i < count; ++i) {
            std::string msg = prefix + " " + std::to_string(i + 1);
            std::string filename = "file" + std::to_string(i + 1) + ".txt";
            std::string content = "Content " + std::to_string(i + 1);

            auto oid = createCommit(msg, {{filename, content}}, parent);
            oids.push_back(oid);
            parent = oid;
        }

        return oids;
    }

    // Setup refs/heads/main pointing to an OID
    void setupMain(const gitmanip::Oid& oid) {
        repo_->update_ref("refs/heads/main", oid);
        repo_->set_head("refs/heads/main");
    }

    // Create a branch pointing to an OID
    void createBranch(const std::string& name, const gitmanip::Oid& oid) {
        repo_->update_ref("refs/heads/" + name, oid);
    }

    // Write a file to the working directory
    void writeFile(const std::string& path, const std::string& content) {
        fs::path full_path = test_dir_ / path;
        fs::create_directories(full_path.parent_path());
        std::ofstream ofs(full_path);
        ofs << content;
    }

    // Read a file from working directory
    std::string readFile(const std::string& path) {
        std::ifstream ifs(test_dir_ / path);
        std::stringstream ss;
        ss << ifs.rdbuf();
        return ss.str();
    }

    fs::path test_dir_;
    std::unique_ptr<gitmanip::Repository> repo_;
    gitmanip::Signature sig_;
};
