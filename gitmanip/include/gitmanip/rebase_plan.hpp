#pragma once

#include "commit.hpp"
#include "types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace gitmanip {

class Repository;
class Commit;

// Represents a single operation in a rebase plan
struct RebaseAction {
    // The type of action to perform
    enum class Type {
        Pick,     // Keep commit as-is
        Reword,   // Change commit message
        Edit,     // Pause for manual editing
        Squash,   // Combine with previous, merge messages
        Fixup,    // Combine with previous, discard message
        Drop,     // Remove commit entirely
    };

    Type type = Type::Pick;
    Oid original_oid;
    std::string original_message;

    // New message for Reword/Squash (optional for Squash if auto-combining)
    std::optional<std::string> new_message;

    // Label for reference (displayed in plan output)
    std::string label() const;

    // Factory methods
    static RebaseAction pick(const Commit& commit);
    static RebaseAction reword(const Commit& commit, std::string new_message);
    static RebaseAction squash(const Commit& commit,
                               std::optional<std::string> combined_message = std::nullopt);
    static RebaseAction fixup(const Commit& commit);
    static RebaseAction drop(const Commit& commit);
    static RebaseAction edit(const Commit& commit);
};

// Modification to apply to a commit's changes
struct ChangeModification {
    enum class Type {
        KeepAll,         // Keep all changes (default)
        DropFile,        // Remove a specific file's changes
        KeepOnlyFiles,   // Keep only specified files
        ModifyHunk,      // Modify specific hunks in a file
        InsertFile,      // Add a new file
        ReplaceContent,  // Replace file content entirely
    };

    Type type = Type::KeepAll;

    // File path(s) this modification applies to
    std::vector<std::string> paths;

    // For InsertFile/ReplaceContent: the new content
    std::optional<std::vector<uint8_t>> content;

    // For ModifyHunk: indices of hunks to keep (others dropped)
    std::vector<size_t> hunk_indices;

    // Factory methods
    static ChangeModification keep_all();
    static ChangeModification drop_file(std::string path);
    static ChangeModification drop_files(std::vector<std::string> paths);
    static ChangeModification keep_only_files(std::vector<std::string> paths);
    static ChangeModification insert_file(std::string path, std::vector<uint8_t> content);
    static ChangeModification insert_file(std::string path, std::string_view content);
    static ChangeModification replace_content(std::string path, std::vector<uint8_t> content);
    static ChangeModification replace_content(std::string path, std::string_view content);
    static ChangeModification keep_hunks(std::string path, std::vector<size_t> hunk_indices);
};

// A complete rebase plan
class RebasePlan {
public:
    RebasePlan(Repository& repo, std::string_view onto_ref);
    RebasePlan(Repository& repo, const Oid& onto);
    ~RebasePlan();

    RebasePlan(const RebasePlan&) = delete;
    RebasePlan& operator=(const RebasePlan&) = delete;
    RebasePlan(RebasePlan&&) noexcept;
    RebasePlan& operator=(RebasePlan&&) noexcept;

    // Build plan from commits between two refs
    static RebasePlan from_range(Repository& repo,
                                 std::string_view base_ref,
                                 std::string_view head_ref);

    // Access the plan
    [[nodiscard]] size_t size() const;
    [[nodiscard]] const RebaseAction& action(size_t index) const;
    [[nodiscard]] std::vector<RebaseAction>& actions();
    [[nodiscard]] const std::vector<RebaseAction>& actions() const;

    // Modify the plan
    RebasePlan& add(RebaseAction action);
    RebasePlan& insert(size_t index, RebaseAction action);
    RebasePlan& remove(size_t index);
    RebasePlan& replace(size_t index, RebaseAction action);
    RebasePlan& move(size_t from_index, size_t to_index);
    RebasePlan& clear();

    // Bulk operations
    RebasePlan& pick_all(const std::vector<Commit>& commits);
    RebasePlan& drop_all();  // Mark all as Drop
    RebasePlan& reword_all(std::function<std::string(const std::string&)> transformer);

    // Add change modifications for specific commits
    RebasePlan& modify_changes(const Oid& commit_oid, ChangeModification mod);
    RebasePlan& modify_changes(size_t action_index, ChangeModification mod);
    [[nodiscard]] const std::vector<ChangeModification>&
        get_modifications(size_t action_index) const;

    // Onto target
    [[nodiscard]] Oid onto() const;
    RebasePlan& set_onto(const Oid& oid);
    RebasePlan& set_onto(std::string_view ref);

    // Validation
    [[nodiscard]] bool is_valid() const;
    [[nodiscard]] std::vector<std::string> validation_errors() const;

    // Output plan as text (like git rebase -i format)
    [[nodiscard]] std::string to_string() const;

    // Parse plan from text
    static RebasePlan parse(Repository& repo, std::string_view plan_text,
                            std::string_view onto_ref);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gitmanip
