#include "gitmanip/tree.hpp"
#include "gitmanip/error.hpp"
#include "gitmanip/repository.hpp"

#include <git2.h>
#include <fmt/format.h>

#include <map>

namespace gitmanip {

// Tree implementation
Tree::Tree(Repository* repo, detail::GitPtr<git_tree> tree)
    : repo_(repo), tree_(std::move(tree)) {}

Oid Tree::id() const {
    return Oid(git_tree_id(tree_.get()));
}

size_t Tree::entry_count() const {
    return git_tree_entrycount(tree_.get());
}

namespace {

TreeEntry convert_entry(const git_tree_entry* entry) {
    TreeEntry result;
    result.name = git_tree_entry_name(entry);
    result.oid = Oid(git_tree_entry_id(entry));
    result.filemode = git_tree_entry_filemode(entry);

    switch (git_tree_entry_type(entry)) {
        case GIT_OBJECT_BLOB:
            result.type = TreeEntry::Type::Blob;
            break;
        case GIT_OBJECT_TREE:
            result.type = TreeEntry::Type::Tree;
            break;
        case GIT_OBJECT_COMMIT:
            result.type = TreeEntry::Type::Commit;
            break;
        default:
            result.type = TreeEntry::Type::Blob;
            break;
    }

    return result;
}

}  // namespace

std::optional<TreeEntry> Tree::entry_by_name(std::string_view name) const {
    const git_tree_entry* entry = git_tree_entry_byname(tree_.get(), std::string(name).c_str());
    if (!entry) return std::nullopt;
    return convert_entry(entry);
}

std::optional<TreeEntry> Tree::entry_by_index(size_t index) const {
    const git_tree_entry* entry = git_tree_entry_byindex(tree_.get(), index);
    if (!entry) return std::nullopt;
    return convert_entry(entry);
}

std::optional<TreeEntry> Tree::entry_by_path(std::string_view path) const {
    git_tree_entry* entry = nullptr;
    int error = git_tree_entry_bypath(&entry, tree_.get(), std::string(path).c_str());
    if (error == GIT_ENOTFOUND) {
        return std::nullopt;
    }
    detail::check_libgit2_error(error, fmt::format("looking up path {} in tree", path));

    TreeEntry result = convert_entry(entry);
    git_tree_entry_free(entry);
    return result;
}

std::vector<TreeEntry> Tree::entries() const {
    std::vector<TreeEntry> result;
    size_t count = entry_count();
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const git_tree_entry* entry = git_tree_entry_byindex(tree_.get(), i);
        if (entry) {
            result.push_back(convert_entry(entry));
        }
    }
    return result;
}

std::optional<std::vector<uint8_t>> Tree::blob_content(std::string_view path) const {
    auto entry = entry_by_path(path);
    if (!entry || entry->type != TreeEntry::Type::Blob) {
        return std::nullopt;
    }
    return repo_->blob_content(entry->oid);
}

std::optional<std::string> Tree::blob_content_string(std::string_view path) const {
    auto content = blob_content(path);
    if (!content) return std::nullopt;
    return std::string(content->begin(), content->end());
}

// TreeBuilder implementation
struct TreeBuilder::Impl {
    Repository* repo;
    git_treebuilder* builder = nullptr;

    // For nested path support
    struct PendingEntry {
        Oid oid;
        uint32_t filemode;
    };
    std::map<std::string, PendingEntry> pending_blobs;

    ~Impl() {
        if (builder) {
            git_treebuilder_free(builder);
        }
    }

    // Build a tree recursively from pending entries
    Oid build_tree_recursive(const std::string& prefix);
};

Oid TreeBuilder::Impl::build_tree_recursive(const std::string& prefix) {
    git_treebuilder* tb = nullptr;
    int error = git_treebuilder_new(&tb, repo->raw(), nullptr);
    detail::check_libgit2_error(error, "creating tree builder");

    std::map<std::string, std::map<std::string, PendingEntry>> subtrees;

    for (const auto& [path, entry] : pending_blobs) {
        if (!prefix.empty() && !path.starts_with(prefix)) {
            continue;
        }

        std::string relative_path = prefix.empty() ? path : path.substr(prefix.size());

        size_t slash_pos = relative_path.find('/');
        if (slash_pos == std::string::npos) {
            // Direct entry in this tree
            error = git_treebuilder_insert(nullptr, tb, relative_path.c_str(),
                                           entry.oid.raw(),
                                           static_cast<git_filemode_t>(entry.filemode));
            if (error < 0) {
                git_treebuilder_free(tb);
                detail::check_libgit2_error(error, fmt::format("inserting {} into tree", path));
            }
        } else {
            // Entry in a subtree
            std::string dir = relative_path.substr(0, slash_pos);
            std::string subpath = prefix + dir + "/";
            subtrees[dir][path] = entry;
        }
    }

    // Build subtrees recursively
    for (const auto& [dirname, entries] : subtrees) {
        std::string subprefix = prefix + dirname + "/";
        Oid subtree_oid = build_tree_recursive(subprefix);

        error = git_treebuilder_insert(nullptr, tb, dirname.c_str(),
                                       subtree_oid.raw(), GIT_FILEMODE_TREE);
        if (error < 0) {
            git_treebuilder_free(tb);
            detail::check_libgit2_error(error, fmt::format("inserting subtree {} into tree", dirname));
        }
    }

    git_oid tree_oid;
    error = git_treebuilder_write(&tree_oid, tb);
    git_treebuilder_free(tb);
    detail::check_libgit2_error(error, "writing tree");

    return Oid(&tree_oid);
}

TreeBuilder::TreeBuilder(Repository& repo)
    : impl_(std::make_unique<Impl>()) {
    impl_->repo = &repo;

    int error = git_treebuilder_new(&impl_->builder, repo.raw(), nullptr);
    detail::check_libgit2_error(error, "creating tree builder");
}

TreeBuilder::TreeBuilder(Repository& repo, const Tree& base)
    : impl_(std::make_unique<Impl>()) {
    impl_->repo = &repo;

    int error = git_treebuilder_new(&impl_->builder, repo.raw(), base.raw());
    detail::check_libgit2_error(error, "creating tree builder from base");
}

TreeBuilder::~TreeBuilder() = default;
TreeBuilder::TreeBuilder(TreeBuilder&&) noexcept = default;
TreeBuilder& TreeBuilder::operator=(TreeBuilder&&) noexcept = default;

TreeBuilder& TreeBuilder::insert(std::string_view name, const Oid& oid, uint32_t filemode) {
    int error = git_treebuilder_insert(nullptr, impl_->builder,
                                       std::string(name).c_str(),
                                       oid.raw(),
                                       static_cast<git_filemode_t>(filemode));
    detail::check_libgit2_error(error, fmt::format("inserting {} into tree", name));
    return *this;
}

TreeBuilder& TreeBuilder::insert_blob(std::string_view path, std::span<const uint8_t> content) {
    Oid blob_oid = impl_->repo->create_blob(content);

    // Check if path contains directories
    if (path.find('/') != std::string_view::npos) {
        impl_->pending_blobs[std::string(path)] = {blob_oid, GIT_FILEMODE_BLOB};
    } else {
        insert(path, blob_oid, GIT_FILEMODE_BLOB);
    }
    return *this;
}

TreeBuilder& TreeBuilder::insert_blob(std::string_view path, std::string_view content) {
    return insert_blob(path, std::span(reinterpret_cast<const uint8_t*>(content.data()),
                                       content.size()));
}

TreeBuilder& TreeBuilder::remove(std::string_view name) {
    int error = git_treebuilder_remove(impl_->builder, std::string(name).c_str());
    // Ignore ENOTFOUND - removing non-existent entry is fine
    if (error != GIT_ENOTFOUND) {
        detail::check_libgit2_error(error, fmt::format("removing {} from tree", name));
    }
    return *this;
}

TreeBuilder& TreeBuilder::clear() {
    int error = git_treebuilder_clear(impl_->builder);
    detail::check_libgit2_error(error, "clearing tree builder");
    impl_->pending_blobs.clear();
    return *this;
}

Tree TreeBuilder::build() {
    return impl_->repo->lookup_tree(build_oid());
}

Oid TreeBuilder::build_oid() {
    // If we have pending nested blobs, build the tree recursively
    if (!impl_->pending_blobs.empty()) {
        // First, get existing entries from the builder
        size_t entry_count = git_treebuilder_entrycount(impl_->builder);
        for (size_t i = 0; i < entry_count; ++i) {
            // Note: libgit2 doesn't provide iteration over treebuilder entries
            // So we need to handle this differently
        }

        return impl_->build_tree_recursive("");
    }

    git_oid oid;
    int error = git_treebuilder_write(&oid, impl_->builder);
    detail::check_libgit2_error(error, "writing tree");
    return Oid(&oid);
}

}  // namespace gitmanip
