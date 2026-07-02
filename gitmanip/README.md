# gitmanip

A modern C++23 library for programmatic Git repository manipulation built on libgit2.

## Features

- **Repository Access**: Open, create, and query Git repositories
- **Commit Operations**: Read commit data, walk history, compare commits
- **Diff Generation**: Create and analyze diffs between trees, commits, or working directory
- **Blame/Annotation**: Get line-by-line authorship information
- **File History**: Track file history through renames, find when files were introduced
- **Commit Search**: Search commits by message, author, content changes
- **Rebase Operations**: Programmatically rebase, squash, reword, drop, and reorder commits
- **Tree Manipulation**: Build and modify Git trees for custom commits
- **Commit/Tag Signing**: GPG and SSH signing with automatic config loading from git

## Requirements

- C++23 compiler (GCC 13+, Clang 17+, MSVC 2022+)
- CMake 3.25+
- Conan 2.0+ (package manager)

## Quick Start

### Building

gitmanip builds as part of the scrutiny-agent tree (the repository
root owns the Conan dependencies and toolchain):

```bash
# from the repository root
scripts/build-host.sh

# Run the gitmanip tests
ctest --test-dir build --output-on-failure -R '^[A-Za-z]+Test'
```

### Basic Usage

```cpp
#include <gitmanip/gitmanip.hpp>
#include <iostream>

int main() {
    // Open a repository
    auto repo = gitmanip::Repository::open("/path/to/repo");

    // Get HEAD commit info
    auto head = repo.lookup_commit("HEAD");
    std::cout << "HEAD: " << head.id().short_id() << "\n";
    std::cout << "Message: " << head.summary() << "\n";
    std::cout << "Author: " << head.author().name << "\n";

    return 0;
}
```

## API Reference

### Repository Operations

```cpp
// Opening repositories
auto repo = gitmanip::Repository::open("/path/to/repo");
auto repo = gitmanip::Repository::discover("/path/to/subdir");  // walks up to find .git
auto repo = gitmanip::Repository::init("/path/to/new/repo");
auto repo = gitmanip::Repository::init("/path/to/bare.git", true);  // bare repo

// Repository info
repo.path();           // Path to .git directory
repo.workdir();        // Working directory (empty for bare repos)
repo.is_bare();        // Is this a bare repository?
repo.is_empty();       // Does it have any commits?
repo.head_oid();       // OID of HEAD commit
repo.head_name();      // Reference name of HEAD (e.g., "refs/heads/main")

// Reference operations
repo.resolve_ref("refs/heads/main");           // Get OID for ref
repo.try_resolve_ref("refs/heads/maybe");      // Returns nullopt if not found
repo.update_ref("refs/heads/main", oid, "message");  // Update ref to new OID
repo.set_head("refs/heads/main");              // Point HEAD to ref
repo.set_head_detached(oid);                   // Detach HEAD to OID
```

### Remotes

```cpp
// List all remotes
for (const auto& remote : gitmanip::Remote::list(repo)) {
    std::cout << remote.name << ": " << remote.url << "\n";
    if (remote.push_url != remote.url) {
        std::cout << "  push: " << remote.push_url << "\n";
    }
}

// Get just names
auto names = gitmanip::Remote::names(repo);  // ["origin", "upstream"]

// Get specific remote
if (auto remote = gitmanip::Remote::get(repo, "origin")) {
    std::cout << "Origin URL: " << remote->url << "\n";
}

// Check existence
if (gitmanip::Remote::exists(repo, "origin")) { ... }

// Manage remotes
gitmanip::Remote::add(repo, "upstream", "https://github.com/org/repo.git");
gitmanip::Remote::remove(repo, "old-remote");
gitmanip::Remote::rename(repo, "origin", "primary");
gitmanip::Remote::set_url(repo, "origin", "git@github.com:user/repo.git");
gitmanip::Remote::set_push_url(repo, "origin", "git@github.com:user/repo.git");
```

### Branches

```cpp
// List branches with full details
for (const auto& branch : gitmanip::Branch::list(repo, true, true)) {
    std::cout << branch.name;
    if (branch.is_remote) std::cout << " (remote)";
    if (branch.is_head) std::cout << " *";
    std::cout << " -> " << branch.target.short_id();
    if (branch.upstream) {
        std::cout << " [" << *branch.upstream << "]";
    }
    std::cout << "\n";
}

// List local only, remote only, or both
auto local = gitmanip::Branch::list(repo, true, false);   // Local only
auto remote = gitmanip::Branch::list(repo, false, true);  // Remote only
auto all = gitmanip::Branch::list(repo, true, true);      // Both

// Get current branch
if (auto current = gitmanip::Branch::current(repo)) {
    std::cout << "On branch: " << current->name << "\n";
} else {
    std::cout << "HEAD is detached\n";
}

// Get specific branch
if (auto branch = gitmanip::Branch::get(repo, "feature")) {
    std::cout << branch->name << " at " << branch->target.short_id() << "\n";
}

// Remote-tracking branch
if (auto branch = gitmanip::Branch::get(repo, "origin/main", true)) {
    std::cout << "Remote branch: " << branch->name << "\n";
}

// Check existence
gitmanip::Branch::exists(repo, "main");              // Local branch
gitmanip::Branch::exists(repo, "origin/main", true); // Remote-tracking

// Upstream management
if (auto upstream = gitmanip::Branch::upstream(repo, "main")) {
    std::cout << "main tracks " << upstream->name << "\n";
}
gitmanip::Branch::set_upstream(repo, "feature", "origin/feature");
gitmanip::Branch::unset_upstream(repo, "feature");

// Create/delete branches (using Repository methods)
repo.create_branch("new-feature", commit);
repo.delete_branch("old-feature");
```

### Tags

```cpp
// List all tags
for (const auto& tag : gitmanip::Tag::list(repo)) {
    std::cout << tag.name << " -> " << tag.commit_target.short_id();
    if (tag.is_annotated) {
        std::cout << " (annotated)";
        if (tag.message) {
            std::cout << ": " << *tag.message;
        }
        if (tag.tagger) {
            std::cout << " by " << tag.tagger->name;
        }
    }
    std::cout << "\n";
}

// Filter tags by pattern
auto v1_tags = gitmanip::Tag::list(repo, "v1.*");    // v1.0, v1.1, v1.2.3
auto release_tags = gitmanip::Tag::list(repo, "*release*");

// Get just tag names
auto names = gitmanip::Tag::names(repo);             // All tags
auto names = gitmanip::Tag::names(repo, "v2.*");     // Filtered

// Get specific tag
if (auto tag = gitmanip::Tag::get(repo, "v1.0.0")) {
    std::cout << "Tag points to: " << tag->commit_target.short_id() << "\n";
    if (tag->is_annotated && tag->message) {
        std::cout << "Message: " << *tag->message << "\n";
    }
}

// Check existence
if (gitmanip::Tag::exists(repo, "v1.0.0")) { ... }

// Create tags
auto commit = repo.lookup_commit("HEAD");
gitmanip::Tag::create_lightweight(repo, "v1.0.0-rc1", commit);
gitmanip::Tag::create_annotated(repo, "v1.0.0", commit, "Release version 1.0.0");

// Create with custom tagger
auto tagger = gitmanip::Signature::now("Release Bot", "bot@example.com");
gitmanip::Tag::create_annotated(repo, "v1.0.0", commit, "Release 1.0.0", tagger);

// Force overwrite existing tag
gitmanip::Tag::create_lightweight(repo, "latest", commit, true);

// Delete tag
gitmanip::Tag::remove(repo, "old-tag");

// Find tags pointing to a commit
auto tags = gitmanip::Tag::pointing_to(repo, commit.id());
for (const auto& name : tags) {
    std::cout << "Tagged as: " << name << "\n";
}
```

### Commit Operations

```cpp
// Looking up commits
auto commit = repo.lookup_commit(oid);         // By OID
auto commit = repo.lookup_commit("HEAD");      // By revision string
auto commit = repo.lookup_commit("main~3");    // Parent syntax supported
auto commit = repo.lookup_commit("v1.0.0");    // Tags work too

// Commit properties
commit.id();              // Oid - full SHA
commit.id().short_id();   // Short SHA (default 7 chars)
commit.id().to_string();  // Full 40-char hex string

commit.message();         // Full commit message
commit.summary();         // First line only
commit.body();            // Everything after first line

commit.author();          // Signature {name, email, time, offset_minutes}
commit.committer();       // Signature for committer

commit.parent_count();    // Number of parents (0=root, 1=normal, 2+=merge)
commit.parent(0);         // Get first parent commit
commit.parent_id(0);      // Get first parent OID without loading commit
commit.parents();         // Vector of all parent commits

commit.is_merge();        // Has more than one parent?
commit.is_root();         // Has no parents?

commit.tree();            // Get the tree (file snapshot) for this commit

// Commit relationships
commit.is_ancestor_of(other);  // Is this commit an ancestor of other?
```

### Diff Operations on Commits

```cpp
// Get diff for a commit (vs its first parent)
auto diff = commit.diff_from_parent();

// Get diff vs a specific parent (useful for merge commits)
auto diff = commit.diff_from_parent(0);  // vs first parent (target branch)
auto diff = commit.diff_from_parent(1);  // vs second parent (merged branch)

// For merge commits: get only merge-unique changes
// This shows conflict resolutions and "evil merge" additions/deletions
if (commit.is_merge()) {
    auto merge_changes = commit.merge_diff();
    if (merge_changes.num_deltas() > 0) {
        std::cout << "Merge has unique changes:\n";
        std::cout << merge_changes.full_patch();
    }

    // Quick check without computing full diff
    if (commit.has_merge_changes()) {
        std::cout << "Warning: merge contains changes not from either parent\n";
    }
}

// Diff between any two commits
auto diff = commit1.diff_to(commit2);
```

### Walking Commit History

```cpp
// Basic walking
auto walker = repo.walk_commits();
walker.push_head();                              // Start from HEAD
walker.sort(gitmanip::CommitWalker::SortOrder::Time);  // Sort by time

for (auto commit : walker.walk()) {              // C++23 generator
    std::cout << commit.summary() << "\n";
}

// Or collect into vector
auto commits = walker.collect();

// Walking between refs (useful for rebasing)
auto commits = repo.commits_between("main", "feature");  // Commits on feature not in main

// Advanced walking
walker.push(oid);                    // Start from specific OID
walker.push_ref("refs/heads/dev");   // Start from ref
walker.hide(oid);                    // Exclude commit and ancestors
walker.hide_ref("refs/heads/main");  // Exclude ref and ancestors
walker.simplify_first_parent();      // Follow only first parents

// Sort orders
SortOrder::None          // No sorting
SortOrder::Topological   // Parents before children
SortOrder::Time          // Newest first
SortOrder::Reverse       // Oldest first (combine with others)
```

### Diff Operations

```cpp
// Get diff for a commit (vs its parent)
auto diff = commit.diff_from_parent();

// Diff between two commits
auto diff = commit1.diff_to(commit2);

// Diff between trees
auto diff = repo.diff_tree_to_tree(old_tree, new_tree);

// Diff options
gitmanip::DiffOptions opts;
opts.context_lines = 3;              // Lines of context
opts.ignore_whitespace = true;       // -w flag
opts.ignore_whitespace_change = true; // -b flag
opts.pathspec = {"src/*.cpp"};       // Filter by path

auto diff = repo.diff_tree_to_tree(old_tree, new_tree, opts);

// Analyzing diffs
diff.num_deltas();                   // Number of changed files

for (auto delta : diff.deltas()) {   // Iterate changed files
    delta.status;      // Added, Deleted, Modified, Renamed, etc.
    delta.old_path;    // Path in old tree
    delta.new_path;    // Path in new tree
    delta.old_oid;     // OID of old blob
    delta.new_oid;     // OID of new blob
    delta.similarity;  // For renames: 0-100 similarity
}

// Get detailed changes
auto hunks = diff.hunks(0);          // Hunks for first file
for (const auto& hunk : hunks) {
    hunk.old_start;   // Starting line in old file
    hunk.old_lines;   // Number of lines in old
    hunk.new_start;   // Starting line in new file
    hunk.new_lines;   // Number of lines in new
    hunk.header;      // @@ -10,5 +10,7 @@ header
}

auto lines = diff.lines(0, 0);       // Lines in first hunk of first file
for (const auto& line : lines) {
    line.origin;      // '+', '-', or ' '
    line.content;     // Line content
    line.old_lineno;  // Line number in old file (-1 if N/A)
    line.new_lineno;  // Line number in new file (-1 if N/A)
}

// Get patch text
std::string patch = diff.full_patch();     // Complete patch
std::string file_patch = diff.patch(0);    // Patch for one file

// Statistics
auto stats = diff.stats();
stats.files_changed;
stats.insertions;
stats.deletions;

// Find renames (modifies diff in-place)
diff.find_similar(50, 50);  // rename_threshold, copy_threshold
```

### Tree Operations

```cpp
// Get tree from commit
auto tree = commit.tree();

// Tree properties
tree.id();                           // OID of tree
tree.entry_count();                  // Number of entries

// Access entries
auto entry = tree.entry_by_name("file.txt");      // By name in this dir
auto entry = tree.entry_by_index(0);              // By index
auto entry = tree.entry_by_path("src/lib/util.cpp");  // By full path

if (entry) {
    entry->name;      // Entry name
    entry->oid;       // OID of blob/tree
    entry->type;      // Blob, Tree, or Commit (submodule)
    entry->filemode;  // Unix file mode
}

// Iterate entries
for (auto entry : tree.entries()) {
    std::cout << entry.name << "\n";
}

// Get file content directly
auto content = tree.blob_content("path/to/file.txt");        // vector<uint8_t>
auto text = tree.blob_content_string("path/to/file.txt");    // string

// Building new trees
gitmanip::TreeBuilder builder(repo);
builder.insert_blob("file.txt", "content");
builder.insert_blob("data.bin", binary_data);
builder.remove("old_file.txt");
auto tree = builder.build();

// Build from existing tree
gitmanip::TreeBuilder builder(repo, existing_tree);
builder.insert_blob("new_file.txt", "new content");
auto modified_tree = builder.build();
```

### Blame Operations

```cpp
// Basic blame
auto blame = gitmanip::Blame::file(repo, "src/main.cpp");

// Blame at specific commit
auto blame = gitmanip::Blame::file_at(repo, "src/main.cpp", commit_oid);

// With options
gitmanip::BlameOptions opts;
opts.ignore_whitespace = true;           // Ignore whitespace changes
opts.track_copies_same_file = true;      // Track moves within file
opts.track_copies_same_commit = true;    // Track copies in same commit
opts.min_line = 10;                      // Only blame lines 10-50
opts.max_line = 50;
auto blame = gitmanip::Blame::file(repo, "file.cpp", opts);

// Iterate blame hunks (contiguous lines from same commit)
for (const auto& hunk : blame.hunks()) {
    hunk.start_line;          // Starting line (1-indexed)
    hunk.end_line();          // Ending line (1-indexed, inclusive)
    hunk.lines_in_hunk;       // Number of lines

    hunk.final_commit_id;     // Commit that last changed these lines
    hunk.final_signature;     // Author of that commit

    hunk.orig_commit_id;      // Original commit (may differ if copied)
    hunk.orig_path;           // Original path (may differ if renamed)
    hunk.orig_signature;      // Original author

    hunk.boundary;            // Is this a boundary commit?
}

// Get blame for specific line
if (auto hunk = blame.line(42)) {
    std::cout << "Line 42 last modified by: " << hunk->final_signature.name << "\n";
}

// Just get the commit for a line
if (auto oid = blame.line_commit(42)) {
    auto commit = repo.lookup_commit(*oid);
}
```

### File History

```cpp
// Trace file history (follows renames)
auto history = gitmanip::FileHistory::trace(repo, "src/renamed.cpp");

for (const auto& entry : history.entries()) {
    entry.commit_id;          // Commit that modified the file
    entry.path;               // Path at this commit
    entry.previous_path;      // Previous path if renamed
    entry.change_type;        // Added, Modified, Renamed, Copied, Deleted
    entry.lines_added;
    entry.lines_deleted;
}

// Find when a file was introduced
if (auto oid = gitmanip::FileHistory::find_introduction(repo, "src/new.cpp")) {
    auto commit = repo.lookup_commit(*oid);
    std::cout << "File added by: " << commit.author().name << "\n";
}

// Find when a file was deleted
if (auto oid = gitmanip::FileHistory::find_deletion(repo, "src/old.cpp")) {
    auto commit = repo.lookup_commit(*oid);
}

// Get all historical paths for a file (traces through renames)
auto paths = gitmanip::FileHistory::all_paths(repo, "src/current_name.cpp");
// paths might be: ["src/original.cpp", "src/old_name.cpp", "src/current_name.cpp"]

// History options
gitmanip::FileHistoryOptions opts;
opts.follow_renames = true;      // Follow renames (default)
opts.rename_threshold = 50;      // Similarity threshold for renames
opts.max_commits = 100;          // Limit history depth
auto history = gitmanip::FileHistory::trace(repo, "file.cpp", opts);
```

### Commit Search

```cpp
// Search by message content
auto commits = gitmanip::CommitSearch::by_message(repo, "bug fix");

// Search by regex in message
auto commits = gitmanip::CommitSearch::by_message_regex(repo, "JIRA-\\d+");

// Find commits that modified a file
auto commits = gitmanip::CommitSearch::touching_file(repo, "src/main.cpp");
auto commits = gitmanip::CommitSearch::touching_files(repo, {"src/a.cpp", "src/b.cpp"});

// Find commits that added/removed specific text (pickaxe)
auto commits = gitmanip::CommitSearch::by_content(repo, "deprecated_function");
auto commits = gitmanip::CommitSearch::by_content_regex(repo, "TODO.*fix");

// Search with options
gitmanip::CommitSearchOptions opts;
opts.max_results = 50;
opts.author = "alice@example.com";
opts.since = some_time_point;
opts.until = another_time_point;
opts.case_insensitive = true;
auto commits = gitmanip::CommitSearch::by_message(repo, "fix", opts);

// Get all commits matching options
auto commits = gitmanip::CommitSearch::all(repo, opts);

// Find merge base
if (auto base = gitmanip::CommitSearch::merge_base(repo, oid1, oid2)) {
    auto commit = repo.lookup_commit(*base);
}

// Check ancestry
bool is_ancestor = gitmanip::CommitSearch::is_ancestor(repo, ancestor_oid, descendant_oid);
```

### Rebase Operations

```cpp
// Create a rebase plan from a range of commits
auto plan = gitmanip::RebasePlan::from_range(repo, "main", "feature");

// Or build manually
gitmanip::RebasePlan plan(repo, "main");  // onto main
for (auto& commit : commits) {
    plan.add(gitmanip::RebaseAction::pick(commit));
}

// Modify the plan
plan.replace(0, gitmanip::RebaseAction::reword(commit, "New message"));
plan.replace(1, gitmanip::RebaseAction::squash(commit2));
plan.replace(2, gitmanip::RebaseAction::fixup(commit3));
plan.replace(3, gitmanip::RebaseAction::drop(commit4));

// Reorder commits
plan.move(3, 0);  // Move action 3 to position 0

// Bulk operations
plan.reword_all([](const std::string& msg) {
    return "[PREFIX] " + msg;
});
plan.drop_all();  // Mark everything as drop

// Print plan (git rebase -i format)
std::cout << plan.to_string();
// Output:
// pick abc1234 First commit
// reword def5678 Second commit
// squash 789abcd Third commit

// Validate
if (!plan.is_valid()) {
    for (const auto& err : plan.validation_errors()) {
        std::cerr << err << "\n";
    }
}

// Execute the rebase
gitmanip::Operations ops(repo);
auto result = ops.execute(plan, [](const gitmanip::RebaseProgress& p) {
    std::cout << p.current_action << "/" << p.total_actions << "\n";
    return true;  // Continue (return false to cancel)
});

if (result.success) {
    std::cout << "New HEAD: " << result.new_head.short_id() << "\n";
    // result.commit_mapping contains old_oid -> new_oid mappings
} else {
    std::cerr << "Failed: " << result.conflict_message.value_or("unknown") << "\n";
}

// Dry run (don't modify repo)
ops.set_dry_run(true);
auto result = ops.execute(plan);
```

### Modifying Commit Changes

```cpp
// During rebase, modify what changes are included
plan.modify_changes(0, gitmanip::ChangeModification::drop_file("unwanted.txt"));
plan.modify_changes(1, gitmanip::ChangeModification::keep_only_files({"important.cpp"}));
plan.modify_changes(2, gitmanip::ChangeModification::insert_file("new.txt", "content"));

// CommitBuilder for more control
gitmanip::CommitBuilder builder(repo, base_commit);
builder.set_message("New message")
       .set_author(new_author)
       .add_file("new.txt", "content")
       .remove_file("old.txt")
       .modify_file("existing.txt", [](std::string_view content) {
           return std::string(content) + "\nappended";
       });

auto new_commit = builder.build_commit();
```

### Convenience Operations

```cpp
gitmanip::Operations ops(repo);

// Reword a single commit
auto new_oid = ops.reword_commit(commit, "New message");

// Squash commits together
std::vector<const gitmanip::Commit*> to_squash = {&c1, &c2, &c3};
auto new_oid = ops.squash_commits(to_squash, "Combined commit");

// Drop a commit
auto result = ops.drop_commit(commit);

// Amend HEAD
auto new_oid = ops.amend_head("New message");
auto new_oid = ops.amend_head(std::nullopt, new_tree);

// Cherry-pick
auto new_oid = ops.cherry_pick(commit);
auto new_oid = ops.cherry_pick(commit, "Custom message");

// Revert
auto new_oid = ops.revert(commit);
```

### Commit and Tag Signing

```cpp
// Load signing configuration from git config
auto config = gitmanip::SigningConfig::from_repo(repo);
std::cout << "Format: " << (config.format == gitmanip::SigningFormat::SSH ? "ssh" : "gpg") << "\n";
std::cout << "Key: " << config.signing_key << "\n";

// Create a signer from config
auto signer = gitmanip::Signer::create(config);

// Or create signers directly
auto gpg_signer = gitmanip::Signer::create_gpg("user@example.com");
auto ssh_signer = gitmanip::Signer::create_ssh("/path/to/key",
                                                 "ssh-keygen",
                                                 "/path/to/allowed_signers");

// Check if a commit is signed
if (commit.is_signed()) {
    auto sig = commit.signature();         // The GPG/SSH signature
    auto data = commit.signed_data();      // The data that was signed

    // Verify the signature
    auto result = signer->verify(*data, *sig);
    if (result.valid) {
        std::cout << "Valid signature by " << result.signer_identity << "\n";
        if (result.trusted) {
            std::cout << "Key is trusted\n";
        }
    }
}

// Create signed commits using CommitBuilder
gitmanip::CommitBuilder builder(repo, base_commit);
builder.set_message("Signed commit")
       .set_signer(signer.get())
       .build();

// Create signed commits directly
auto oid = gitmanip::create_signed_commit(
    repo, "Signed message", tree, parents, *signer);

// Create signed tags
auto head = repo.lookup_commit("HEAD");
gitmanip::Tag::create_signed(repo, "v1.0.0", head, "Release 1.0.0", *signer);

// Check if a tag is signed
auto tag_info = gitmanip::Tag::get(repo, "v1.0.0");
if (tag_info && tag_info->is_signed) {
    std::cout << "Tag signature: " << *tag_info->gpg_signature << "\n";
}

// Verify a signed tag
if (gitmanip::Tag::verify(repo, "v1.0.0", *signer)) {
    std::cout << "Tag signature is valid\n";
}

// Configuration options read from git config:
// - user.signingkey: Key ID (GPG) or path to SSH key
// - gpg.format: "openpgp" (default) or "ssh"
// - gpg.program: Custom GPG binary path
// - gpg.ssh.program: Custom SSH signing binary (ssh-keygen)
// - gpg.ssh.allowedSignersFile: For SSH verification
// - commit.gpgsign: Sign commits by default
// - tag.gpgsign: Sign tags by default
```

### Error Handling

```cpp
try {
    auto repo = gitmanip::Repository::open("/nonexistent");
} catch (const gitmanip::GitError& e) {
    std::cerr << "Error: " << e.what() << "\n";
    std::cerr << "Code: " << static_cast<int>(e.code()) << "\n";
    // e.code() is gitmanip::ErrorCode enum
    // e.location() has source_location info
}

// Error codes
ErrorCode::RepositoryNotFound
ErrorCode::RepositoryCorrupted
ErrorCode::ReferenceNotFound
ErrorCode::CommitNotFound
ErrorCode::InvalidOid
ErrorCode::ConflictDetected
ErrorCode::IndexLocked
ErrorCode::WorkdirDirty
ErrorCode::MergeConflict
// ... and more
```

## Thread Safety

- `Repository` objects are **NOT** thread-safe. Use separate instances per thread.
- Objects returned from a Repository (Commit, Tree, etc.) hold a reference to it and must not outlive it.
- libgit2 initialization is handled automatically and is thread-safe.

## Dependencies

- [libgit2](https://libgit2.org/) - Git implementation
- [fmt](https://github.com/fmtlib/fmt) - String formatting
- [spdlog](https://github.com/gabime/spdlog) - Logging
- [Google Test](https://github.com/google/googletest) - Testing (optional)

## License

MIT License - see LICENSE file for details.
