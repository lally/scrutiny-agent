#include "gitmanip/repository.hpp"
#include "gitmanip/error.hpp"

#include <git2.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <cstring>

namespace gitmanip {

// Deleters
namespace detail {

void GitDeleter<git_repository>::operator()(git_repository* ptr) const {
    if (ptr) git_repository_free(ptr);
}

void GitDeleter<git_commit>::operator()(git_commit* ptr) const {
    if (ptr) git_commit_free(ptr);
}

void GitDeleter<git_tree>::operator()(git_tree* ptr) const {
    if (ptr) git_tree_free(ptr);
}

void GitDeleter<git_index>::operator()(git_index* ptr) const {
    if (ptr) git_index_free(ptr);
}

void GitDeleter<git_reference>::operator()(git_reference* ptr) const {
    if (ptr) git_reference_free(ptr);
}

void GitDeleter<git_signature>::operator()(git_signature* ptr) const {
    if (ptr) git_signature_free(ptr);
}

void GitDeleter<git_diff>::operator()(git_diff* ptr) const {
    if (ptr) git_diff_free(ptr);
}

}  // namespace detail

// LibGit2Init
int LibGit2Init::ref_count_ = 0;

LibGit2Init::LibGit2Init() {
    if (ref_count_++ == 0) {
        git_libgit2_init();
    }
}

LibGit2Init::~LibGit2Init() {
    if (--ref_count_ == 0) {
        git_libgit2_shutdown();
    }
}

// Oid implementation
Oid::Oid(const git_oid* oid) {
    if (oid) {
        std::memcpy(data_.data(), oid->id, Size);
    }
}

Oid::Oid(std::string_view hex) {
    if (hex.size() < HexSize) {
        throw GitError(ErrorCode::InvalidOid,
                       fmt::format("OID hex string too short: {}", hex));
    }

    git_oid oid;
    int error = git_oid_fromstr(&oid, std::string(hex).c_str());
    detail::check_libgit2_error(error, "parsing OID");
    std::memcpy(data_.data(), oid.id, Size);
}

std::string Oid::to_string() const {
    char buf[HexSize + 1];
    git_oid oid;
    std::memcpy(oid.id, data_.data(), Size);
    git_oid_tostr(buf, sizeof(buf), &oid);
    return std::string(buf);
}

std::string Oid::short_id(size_t length) const {
    return to_string().substr(0, length);
}

bool Oid::is_zero() const {
    for (auto b : data_) {
        if (b != 0) return false;
    }
    return true;
}

const git_oid* Oid::raw() const {
    if (!raw_cache_valid_) {
        std::memcpy(raw_cache_.data(), data_.data(), Size);
        raw_cache_valid_ = true;
    }
    return reinterpret_cast<const git_oid*>(raw_cache_.data());
}

// Signature implementation
std::string Signature::format() const {
    int hours = offset_minutes / 60;
    int mins = std::abs(offset_minutes) % 60;
    return fmt::format("{} <{}> {:+03d}{:02d}", name, email, hours, mins);
}

Signature Signature::now(std::string_view name, std::string_view email) {
    Signature sig;
    sig.name = std::string(name);
    sig.email = std::string(email);
    sig.time = std::chrono::system_clock::now();
    // Get local timezone offset
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    sig.offset_minutes = static_cast<int>(tm.tm_gmtoff / 60);
    return sig;
}

// Repository implementation
Repository::Repository(detail::GitPtr<git_repository> repo)
    : repo_(std::move(repo)) {}

Repository::Repository(Repository&&) noexcept = default;
Repository& Repository::operator=(Repository&&) noexcept = default;
Repository::~Repository() = default;

Repository Repository::open(const std::filesystem::path& path) {
    static LibGit2Init init;

    git_repository* repo = nullptr;
    int error = git_repository_open(&repo, path.c_str());
    detail::check_libgit2_error(error, fmt::format("opening repository at {}", path.string()));

    return Repository(detail::GitPtr<git_repository>(repo));
}

Repository Repository::discover(const std::filesystem::path& path) {
    static LibGit2Init init;

    git_buf buf = GIT_BUF_INIT;
    int error = git_repository_discover(&buf, path.c_str(), 0, nullptr);
    detail::check_libgit2_error(error, fmt::format("discovering repository from {}", path.string()));

    std::string repo_path(buf.ptr, buf.size);
    git_buf_dispose(&buf);

    return open(repo_path);
}

Repository Repository::init(const std::filesystem::path& path, bool bare) {
    static LibGit2Init init;

    git_repository* repo = nullptr;
    int error = git_repository_init(&repo, path.c_str(), bare ? 1 : 0);
    detail::check_libgit2_error(error, fmt::format("initializing repository at {}", path.string()));

    return Repository(detail::GitPtr<git_repository>(repo));
}

std::filesystem::path Repository::path() const {
    return git_repository_path(repo_.get());
}

std::filesystem::path Repository::workdir() const {
    const char* wd = git_repository_workdir(repo_.get());
    return wd ? wd : "";
}

bool Repository::is_bare() const {
    return git_repository_is_bare(repo_.get()) != 0;
}

bool Repository::is_empty() const {
    return git_repository_is_empty(repo_.get()) != 0;
}

bool Repository::is_head_detached() const {
    return git_repository_head_detached(repo_.get()) != 0;
}

Oid Repository::resolve_ref(std::string_view refname) const {
    git_oid oid;
    int error = git_reference_name_to_id(&oid, repo_.get(), std::string(refname).c_str());
    detail::check_libgit2_error(error, fmt::format("resolving reference {}", refname));
    return Oid(&oid);
}

std::optional<Oid> Repository::try_resolve_ref(std::string_view refname) const {
    git_oid oid;
    int error = git_reference_name_to_id(&oid, repo_.get(), std::string(refname).c_str());
    if (error == GIT_ENOTFOUND) {
        return std::nullopt;
    }
    detail::check_libgit2_error(error, fmt::format("resolving reference {}", refname));
    return Oid(&oid);
}

Oid Repository::head_oid() const {
    return resolve_ref("HEAD");
}

std::string Repository::head_name() const {
    git_reference* ref = nullptr;
    int error = git_repository_head(&ref, repo_.get());
    detail::check_libgit2_error(error, "getting HEAD reference");

    detail::GitPtr<git_reference> ref_ptr(ref);
    return git_reference_name(ref);
}

std::vector<std::string> Repository::branch_names(bool local, bool remote) const {
    std::vector<std::string> names;

    git_branch_iterator* iter = nullptr;
    git_branch_t list_flags = static_cast<git_branch_t>(
        (local ? GIT_BRANCH_LOCAL : 0) | (remote ? GIT_BRANCH_REMOTE : 0));

    int error = git_branch_iterator_new(&iter, repo_.get(), list_flags);
    detail::check_libgit2_error(error, "creating branch iterator");

    git_reference* ref = nullptr;
    git_branch_t branch_type;

    while (git_branch_next(&ref, &branch_type, iter) == 0) {
        const char* name = nullptr;
        git_branch_name(&name, ref);
        if (name) {
            names.emplace_back(name);
        }
        git_reference_free(ref);
    }

    git_branch_iterator_free(iter);
    return names;
}

void Repository::create_branch(std::string_view name, const Commit& target, bool force) {
    git_reference* ref = nullptr;
    int error = git_branch_create(&ref, repo_.get(), std::string(name).c_str(),
                                  target.raw(), force ? 1 : 0);
    detail::check_libgit2_error(error, fmt::format("creating branch {}", name));
    git_reference_free(ref);
}

void Repository::delete_branch(std::string_view name) {
    git_reference* ref = nullptr;
    int error = git_branch_lookup(&ref, repo_.get(), std::string(name).c_str(), GIT_BRANCH_LOCAL);
    detail::check_libgit2_error(error, fmt::format("looking up branch {}", name));

    detail::GitPtr<git_reference> ref_ptr(ref);
    error = git_branch_delete(ref);
    detail::check_libgit2_error(error, fmt::format("deleting branch {}", name));
}

void Repository::set_head(std::string_view refname) {
    int error = git_repository_set_head(repo_.get(), std::string(refname).c_str());
    detail::check_libgit2_error(error, fmt::format("setting HEAD to {}", refname));
}

void Repository::set_head_detached(const Oid& oid) {
    int error = git_repository_set_head_detached(repo_.get(), oid.raw());
    detail::check_libgit2_error(error, fmt::format("detaching HEAD to {}", oid.short_id()));
}

void Repository::update_ref(std::string_view refname, const Oid& oid,
                            std::optional<std::string_view> message) {
    git_reference* ref = nullptr;
    std::string msg = message ? std::string(*message) : "";

    int error = git_reference_create(&ref, repo_.get(), std::string(refname).c_str(),
                                     oid.raw(), 1, msg.empty() ? nullptr : msg.c_str());
    detail::check_libgit2_error(error, fmt::format("updating reference {}", refname));
    git_reference_free(ref);
}

Commit Repository::lookup_commit(const Oid& oid) const {
    git_commit* commit = nullptr;
    int error = git_commit_lookup(&commit, repo_.get(), oid.raw());
    detail::check_libgit2_error(error, fmt::format("looking up commit {}", oid.short_id()));

    return Commit(const_cast<Repository*>(this), detail::GitPtr<git_commit>(commit));
}

std::optional<Commit> Repository::try_lookup_commit(const Oid& oid) const {
    git_commit* commit = nullptr;
    int error = git_commit_lookup(&commit, repo_.get(), oid.raw());
    if (error == GIT_ENOTFOUND) {
        return std::nullopt;
    }
    detail::check_libgit2_error(error, fmt::format("looking up commit {}", oid.short_id()));
    return Commit(const_cast<Repository*>(this), detail::GitPtr<git_commit>(commit));
}

Commit Repository::lookup_commit(std::string_view rev) const {
    git_object* obj = nullptr;
    int error = git_revparse_single(&obj, repo_.get(), std::string(rev).c_str());
    detail::check_libgit2_error(error, fmt::format("parsing revision {}", rev));

    if (git_object_type(obj) != GIT_OBJECT_COMMIT) {
        git_object_free(obj);
        throw GitError(ErrorCode::InvalidArgument,
                       fmt::format("{} does not resolve to a commit", rev));
    }

    git_commit* commit = nullptr;
    error = git_commit_lookup(&commit, repo_.get(), git_object_id(obj));
    git_object_free(obj);
    detail::check_libgit2_error(error, "looking up commit");

    return Commit(const_cast<Repository*>(this), detail::GitPtr<git_commit>(commit));
}

Oid Repository::create_commit(std::string_view message,
                               const Tree& tree,
                               std::span<const Commit* const> parents,
                               const Signature& author,
                               const Signature& committer) {
    git_signature* git_author = nullptr;
    git_signature* git_committer = nullptr;

    auto author_time = std::chrono::system_clock::to_time_t(author.time);
    auto committer_time = std::chrono::system_clock::to_time_t(committer.time);

    int error = git_signature_new(&git_author, author.name.c_str(), author.email.c_str(),
                                  author_time, author.offset_minutes);
    detail::check_libgit2_error(error, "creating author signature");

    error = git_signature_new(&git_committer, committer.name.c_str(), committer.email.c_str(),
                              committer_time, committer.offset_minutes);
    if (error < 0) {
        git_signature_free(git_author);
        detail::check_libgit2_error(error, "creating committer signature");
    }

    std::vector<const git_commit*> parent_ptrs;
    parent_ptrs.reserve(parents.size());
    for (const auto* p : parents) {
        parent_ptrs.push_back(p->raw());
    }

    git_oid oid;
    error = git_commit_create(&oid, repo_.get(), nullptr,
                              git_author, git_committer,
                              nullptr, std::string(message).c_str(),
                              tree.raw(),
                              parent_ptrs.size(), parent_ptrs.data());

    git_signature_free(git_author);
    git_signature_free(git_committer);

    detail::check_libgit2_error(error, "creating commit");
    return Oid(&oid);
}

Oid Repository::create_commit(std::string_view message,
                               const Tree& tree,
                               std::span<const Commit* const> parents) {
    auto sig = default_signature();
    return create_commit(message, tree, parents, sig, sig);
}

Tree Repository::lookup_tree(const Oid& oid) const {
    git_tree* tree = nullptr;
    int error = git_tree_lookup(&tree, repo_.get(), oid.raw());
    detail::check_libgit2_error(error, fmt::format("looking up tree {}", oid.short_id()));

    return Tree(const_cast<Repository*>(this), detail::GitPtr<git_tree>(tree));
}

Tree Repository::empty_tree() const {
    git_oid oid;
    int error = git_oid_fromstr(&oid, "4b825dc642cb6eb9a060e54bf8d69288fbee4904");
    detail::check_libgit2_error(error, "parsing empty tree OID");

    git_tree* tree = nullptr;
    error = git_tree_lookup(&tree, repo_.get(), &oid);
    detail::check_libgit2_error(error, "looking up empty tree");

    return Tree(const_cast<Repository*>(this), detail::GitPtr<git_tree>(tree));
}

Diff Repository::diff_tree_to_tree(const Tree& old_tree, const Tree& new_tree,
                                    const DiffOptions& options) const {
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    opts.context_lines = options.context_lines;

    if (options.ignore_whitespace) {
        opts.flags |= GIT_DIFF_IGNORE_WHITESPACE;
    }
    if (options.ignore_whitespace_change) {
        opts.flags |= GIT_DIFF_IGNORE_WHITESPACE_CHANGE;
    }
    if (options.ignore_whitespace_eol) {
        opts.flags |= GIT_DIFF_IGNORE_WHITESPACE_EOL;
    }

    git_diff* diff = nullptr;
    int error = git_diff_tree_to_tree(&diff, repo_.get(),
                                      old_tree.raw(), new_tree.raw(), &opts);
    detail::check_libgit2_error(error, "creating tree-to-tree diff");

    return Diff(const_cast<Repository*>(this), detail::GitPtr<git_diff>(diff));
}

Diff Repository::diff_tree_to_index(const Tree& tree, const DiffOptions& options) const {
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    opts.context_lines = options.context_lines;

    git_index* index = nullptr;
    int error = git_repository_index(&index, repo_.get());
    detail::check_libgit2_error(error, "getting repository index");
    detail::GitPtr<git_index> index_ptr(index);

    git_diff* diff = nullptr;
    error = git_diff_tree_to_index(&diff, repo_.get(), tree.raw(), index, &opts);
    detail::check_libgit2_error(error, "creating tree-to-index diff");

    return Diff(const_cast<Repository*>(this), detail::GitPtr<git_diff>(diff));
}

Diff Repository::diff_index_to_workdir(const DiffOptions& options) const {
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    opts.context_lines = options.context_lines;

    if (options.include_untracked) {
        opts.flags |= GIT_DIFF_INCLUDE_UNTRACKED;
    }
    if (options.recurse_untracked_dirs) {
        opts.flags |= GIT_DIFF_RECURSE_UNTRACKED_DIRS;
    }

    git_index* index = nullptr;
    int error = git_repository_index(&index, repo_.get());
    detail::check_libgit2_error(error, "getting repository index");
    detail::GitPtr<git_index> index_ptr(index);

    git_diff* diff = nullptr;
    error = git_diff_index_to_workdir(&diff, repo_.get(), index, &opts);
    detail::check_libgit2_error(error, "creating index-to-workdir diff");

    return Diff(const_cast<Repository*>(this), detail::GitPtr<git_diff>(diff));
}

Oid Repository::create_blob(std::span<const uint8_t> data) {
    git_oid oid;
    int error = git_blob_create_from_buffer(&oid, repo_.get(), data.data(), data.size());
    detail::check_libgit2_error(error, "creating blob");
    return Oid(&oid);
}

Oid Repository::create_blob(std::string_view content) {
    return create_blob(std::span(reinterpret_cast<const uint8_t*>(content.data()),
                                 content.size()));
}

std::vector<uint8_t> Repository::blob_content(const Oid& oid) const {
    git_blob* blob = nullptr;
    int error = git_blob_lookup(&blob, repo_.get(), oid.raw());
    detail::check_libgit2_error(error, fmt::format("looking up blob {}", oid.short_id()));

    const void* data = git_blob_rawcontent(blob);
    size_t size = git_blob_rawsize(blob);

    std::vector<uint8_t> content(static_cast<const uint8_t*>(data),
                                 static_cast<const uint8_t*>(data) + size);
    git_blob_free(blob);
    return content;
}

std::string Repository::blob_content_string(const Oid& oid) const {
    auto content = blob_content(oid);
    return std::string(content.begin(), content.end());
}

void Repository::add_to_index(const std::filesystem::path& path) {
    git_index* index = nullptr;
    int error = git_repository_index(&index, repo_.get());
    detail::check_libgit2_error(error, "getting repository index");
    detail::GitPtr<git_index> index_ptr(index);

    error = git_index_add_bypath(index, path.c_str());
    detail::check_libgit2_error(error, fmt::format("adding {} to index", path.string()));

    error = git_index_write(index);
    detail::check_libgit2_error(error, "writing index");
}

void Repository::add_all_to_index() {
    git_index* index = nullptr;
    int error = git_repository_index(&index, repo_.get());
    detail::check_libgit2_error(error, "getting repository index");
    detail::GitPtr<git_index> index_ptr(index);

    const char* paths[] = {"."};
    git_strarray pathspec = {const_cast<char**>(paths), 1};

    error = git_index_add_all(index, &pathspec, 0, nullptr, nullptr);
    detail::check_libgit2_error(error, "adding all files to index");

    error = git_index_write(index);
    detail::check_libgit2_error(error, "writing index");
}

void Repository::reset_index() {
    git_object* head_obj = nullptr;
    int error = git_revparse_single(&head_obj, repo_.get(), "HEAD");

    if (error == GIT_ENOTFOUND) {
        // No HEAD yet, just clear the index
        git_index* index = nullptr;
        error = git_repository_index(&index, repo_.get());
        detail::check_libgit2_error(error, "getting repository index");
        detail::GitPtr<git_index> index_ptr(index);

        error = git_index_clear(index);
        detail::check_libgit2_error(error, "clearing index");

        error = git_index_write(index);
        detail::check_libgit2_error(error, "writing index");
        return;
    }

    detail::check_libgit2_error(error, "resolving HEAD");
    std::unique_ptr<git_object, decltype(&git_object_free)> head_ptr(head_obj, git_object_free);

    error = git_reset(repo_.get(), head_obj, GIT_RESET_MIXED, nullptr);
    detail::check_libgit2_error(error, "resetting index");
}

Tree Repository::write_index_as_tree() {
    git_index* index = nullptr;
    int error = git_repository_index(&index, repo_.get());
    detail::check_libgit2_error(error, "getting repository index");
    detail::GitPtr<git_index> index_ptr(index);

    git_oid tree_oid;
    error = git_index_write_tree(&tree_oid, index);
    detail::check_libgit2_error(error, "writing index as tree");

    return lookup_tree(Oid(&tree_oid));
}

void Repository::checkout_tree(const Tree& tree, bool force) {
    git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
    opts.checkout_strategy = force
        ? GIT_CHECKOUT_FORCE
        : GIT_CHECKOUT_SAFE;

    int error = git_checkout_tree(repo_.get(),
                                  reinterpret_cast<git_object*>(tree.raw()),
                                  &opts);
    detail::check_libgit2_error(error, "checking out tree");
}

void Repository::checkout_head(bool force) {
    git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
    opts.checkout_strategy = force
        ? GIT_CHECKOUT_FORCE
        : GIT_CHECKOUT_SAFE;

    int error = git_checkout_head(repo_.get(), &opts);
    detail::check_libgit2_error(error, "checking out HEAD");
}

bool Repository::is_workdir_clean() const {
    if (is_bare()) return true;

    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                 GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;

    git_status_list* status = nullptr;
    int error = git_status_list_new(&status, repo_.get(), &opts);
    detail::check_libgit2_error(error, "getting status list");

    size_t count = git_status_list_entrycount(status);
    git_status_list_free(status);

    return count == 0;
}

bool Repository::has_staged_changes() const {
    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.show = GIT_STATUS_SHOW_INDEX_ONLY;

    git_status_list* status = nullptr;
    int error = git_status_list_new(&status, repo_.get(), &opts);
    detail::check_libgit2_error(error, "getting status list");

    size_t count = git_status_list_entrycount(status);
    git_status_list_free(status);

    return count > 0;
}

Signature Repository::default_signature() const {
    git_signature* sig = nullptr;
    int error = git_signature_default(&sig, repo_.get());
    detail::check_libgit2_error(error, "getting default signature");
    detail::GitPtr<git_signature> sig_ptr(sig);

    Signature result;
    result.name = sig->name;
    result.email = sig->email;
    result.time = std::chrono::system_clock::from_time_t(sig->when.time);
    result.offset_minutes = sig->when.offset;
    return result;
}

CommitWalker Repository::walk_commits() {
    return CommitWalker(*this);
}

std::vector<Commit> Repository::commits_between(std::string_view from_ref,
                                                 std::string_view to_ref) {
    return CommitWalker::between(*this, from_ref, to_ref);
}

}  // namespace gitmanip
