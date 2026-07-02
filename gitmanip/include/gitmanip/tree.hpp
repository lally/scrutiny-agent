#pragma once

#include "types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gitmanip {

class Repository;

class Tree {
public:
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;
    Tree(Tree&&) noexcept = default;
    Tree& operator=(Tree&&) noexcept = default;
    ~Tree() = default;

    [[nodiscard]] Oid id() const;
    [[nodiscard]] size_t entry_count() const;
    [[nodiscard]] std::optional<TreeEntry> entry_by_name(std::string_view name) const;
    [[nodiscard]] std::optional<TreeEntry> entry_by_index(size_t index) const;
    [[nodiscard]] std::optional<TreeEntry> entry_by_path(std::string_view path) const;

    // Get all entries
    [[nodiscard]] std::vector<TreeEntry> entries() const;

    // Get blob content by path
    [[nodiscard]] std::optional<std::vector<uint8_t>> blob_content(std::string_view path) const;
    [[nodiscard]] std::optional<std::string> blob_content_string(std::string_view path) const;

    // Access to underlying repository
    [[nodiscard]] Repository& repository() const { return *repo_; }

    // For libgit2 interop
    [[nodiscard]] git_tree* raw() const { return tree_.get(); }

private:
    friend class Repository;
    friend class Commit;
    friend class TreeBuilder;
    Tree(Repository* repo, detail::GitPtr<git_tree> tree);

    Repository* repo_;
    detail::GitPtr<git_tree> tree_;
};

// Builder for creating/modifying trees
class TreeBuilder {
public:
    explicit TreeBuilder(Repository& repo);
    TreeBuilder(Repository& repo, const Tree& base);
    ~TreeBuilder();

    TreeBuilder(const TreeBuilder&) = delete;
    TreeBuilder& operator=(const TreeBuilder&) = delete;
    TreeBuilder(TreeBuilder&&) noexcept;
    TreeBuilder& operator=(TreeBuilder&&) noexcept;

    // Add or update an entry
    TreeBuilder& insert(std::string_view name, const Oid& oid,
                        uint32_t filemode);

    // Add a blob with content
    TreeBuilder& insert_blob(std::string_view path, std::span<const uint8_t> content);
    TreeBuilder& insert_blob(std::string_view path, std::string_view content);

    // Remove an entry
    TreeBuilder& remove(std::string_view name);

    // Clear all entries
    TreeBuilder& clear();

    // Build the tree
    [[nodiscard]] Tree build();
    [[nodiscard]] Oid build_oid();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gitmanip
