#include "gitmanip/rebase_plan.hpp"
#include "gitmanip/commit.hpp"
#include "gitmanip/error.hpp"
#include "gitmanip/repository.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <map>
#include <regex>
#include <sstream>

namespace gitmanip {

// RebaseAction implementation
std::string RebaseAction::label() const {
    switch (type) {
        case Type::Pick: return "pick";
        case Type::Reword: return "reword";
        case Type::Edit: return "edit";
        case Type::Squash: return "squash";
        case Type::Fixup: return "fixup";
        case Type::Drop: return "drop";
    }
    return "pick";
}

RebaseAction RebaseAction::pick(const Commit& commit) {
    RebaseAction action;
    action.type = Type::Pick;
    action.original_oid = commit.id();
    action.original_message = commit.message();
    return action;
}

RebaseAction RebaseAction::reword(const Commit& commit, std::string new_message) {
    RebaseAction action;
    action.type = Type::Reword;
    action.original_oid = commit.id();
    action.original_message = commit.message();
    action.new_message = std::move(new_message);
    return action;
}

RebaseAction RebaseAction::squash(const Commit& commit,
                                   std::optional<std::string> combined_message) {
    RebaseAction action;
    action.type = Type::Squash;
    action.original_oid = commit.id();
    action.original_message = commit.message();
    action.new_message = std::move(combined_message);
    return action;
}

RebaseAction RebaseAction::fixup(const Commit& commit) {
    RebaseAction action;
    action.type = Type::Fixup;
    action.original_oid = commit.id();
    action.original_message = commit.message();
    return action;
}

RebaseAction RebaseAction::drop(const Commit& commit) {
    RebaseAction action;
    action.type = Type::Drop;
    action.original_oid = commit.id();
    action.original_message = commit.message();
    return action;
}

RebaseAction RebaseAction::edit(const Commit& commit) {
    RebaseAction action;
    action.type = Type::Edit;
    action.original_oid = commit.id();
    action.original_message = commit.message();
    return action;
}

// ChangeModification implementation
ChangeModification ChangeModification::keep_all() {
    ChangeModification mod;
    mod.type = Type::KeepAll;
    return mod;
}

ChangeModification ChangeModification::drop_file(std::string path) {
    ChangeModification mod;
    mod.type = Type::DropFile;
    mod.paths.push_back(std::move(path));
    return mod;
}

ChangeModification ChangeModification::drop_files(std::vector<std::string> paths) {
    ChangeModification mod;
    mod.type = Type::DropFile;
    mod.paths = std::move(paths);
    return mod;
}

ChangeModification ChangeModification::keep_only_files(std::vector<std::string> paths) {
    ChangeModification mod;
    mod.type = Type::KeepOnlyFiles;
    mod.paths = std::move(paths);
    return mod;
}

ChangeModification ChangeModification::insert_file(std::string path,
                                                    std::vector<uint8_t> content) {
    ChangeModification mod;
    mod.type = Type::InsertFile;
    mod.paths.push_back(std::move(path));
    mod.content = std::move(content);
    return mod;
}

ChangeModification ChangeModification::insert_file(std::string path, std::string_view content) {
    return insert_file(std::move(path),
                       std::vector<uint8_t>(content.begin(), content.end()));
}

ChangeModification ChangeModification::replace_content(std::string path,
                                                        std::vector<uint8_t> content) {
    ChangeModification mod;
    mod.type = Type::ReplaceContent;
    mod.paths.push_back(std::move(path));
    mod.content = std::move(content);
    return mod;
}

ChangeModification ChangeModification::replace_content(std::string path,
                                                        std::string_view content) {
    return replace_content(std::move(path),
                           std::vector<uint8_t>(content.begin(), content.end()));
}

ChangeModification ChangeModification::keep_hunks(std::string path,
                                                   std::vector<size_t> hunk_indices) {
    ChangeModification mod;
    mod.type = Type::ModifyHunk;
    mod.paths.push_back(std::move(path));
    mod.hunk_indices = std::move(hunk_indices);
    return mod;
}

// RebasePlan implementation
struct RebasePlan::Impl {
    Repository* repo;
    Oid onto;
    std::vector<RebaseAction> actions;
    std::map<size_t, std::vector<ChangeModification>> modifications;
};

RebasePlan::RebasePlan(Repository& repo, std::string_view onto_ref)
    : impl_(std::make_unique<Impl>()) {
    impl_->repo = &repo;
    impl_->onto = repo.resolve_ref(onto_ref);
}

RebasePlan::RebasePlan(Repository& repo, const Oid& onto)
    : impl_(std::make_unique<Impl>()) {
    impl_->repo = &repo;
    impl_->onto = onto;
}

RebasePlan::~RebasePlan() = default;
RebasePlan::RebasePlan(RebasePlan&&) noexcept = default;
RebasePlan& RebasePlan::operator=(RebasePlan&&) noexcept = default;

RebasePlan RebasePlan::from_range(Repository& repo,
                                   std::string_view base_ref,
                                   std::string_view head_ref) {
    RebasePlan plan(repo, base_ref);

    auto commits = CommitWalker::between(repo, base_ref, head_ref);
    for (const auto& commit : commits) {
        plan.add(RebaseAction::pick(commit));
    }

    return plan;
}

size_t RebasePlan::size() const {
    return impl_->actions.size();
}

const RebaseAction& RebasePlan::action(size_t index) const {
    return impl_->actions.at(index);
}

std::vector<RebaseAction>& RebasePlan::actions() {
    return impl_->actions;
}

const std::vector<RebaseAction>& RebasePlan::actions() const {
    return impl_->actions;
}

RebasePlan& RebasePlan::add(RebaseAction action) {
    impl_->actions.push_back(std::move(action));
    return *this;
}

RebasePlan& RebasePlan::insert(size_t index, RebaseAction action) {
    impl_->actions.insert(impl_->actions.begin() + static_cast<ptrdiff_t>(index),
                          std::move(action));

    // Update modification indices
    std::map<size_t, std::vector<ChangeModification>> new_mods;
    for (auto& [idx, mods] : impl_->modifications) {
        if (idx >= index) {
            new_mods[idx + 1] = std::move(mods);
        } else {
            new_mods[idx] = std::move(mods);
        }
    }
    impl_->modifications = std::move(new_mods);

    return *this;
}

RebasePlan& RebasePlan::remove(size_t index) {
    impl_->actions.erase(impl_->actions.begin() + static_cast<ptrdiff_t>(index));
    impl_->modifications.erase(index);

    // Update remaining indices
    std::map<size_t, std::vector<ChangeModification>> new_mods;
    for (auto& [idx, mods] : impl_->modifications) {
        if (idx > index) {
            new_mods[idx - 1] = std::move(mods);
        } else {
            new_mods[idx] = std::move(mods);
        }
    }
    impl_->modifications = std::move(new_mods);

    return *this;
}

RebasePlan& RebasePlan::replace(size_t index, RebaseAction action) {
    impl_->actions.at(index) = std::move(action);
    return *this;
}

RebasePlan& RebasePlan::move(size_t from_index, size_t to_index) {
    if (from_index == to_index) return *this;

    auto action = std::move(impl_->actions[from_index]);
    impl_->actions.erase(impl_->actions.begin() + static_cast<ptrdiff_t>(from_index));

    size_t insert_pos = from_index < to_index ? to_index - 1 : to_index;
    impl_->actions.insert(impl_->actions.begin() + static_cast<ptrdiff_t>(insert_pos),
                          std::move(action));

    return *this;
}

RebasePlan& RebasePlan::clear() {
    impl_->actions.clear();
    impl_->modifications.clear();
    return *this;
}

RebasePlan& RebasePlan::pick_all(const std::vector<Commit>& commits) {
    for (const auto& commit : commits) {
        add(RebaseAction::pick(commit));
    }
    return *this;
}

RebasePlan& RebasePlan::drop_all() {
    for (auto& action : impl_->actions) {
        action.type = RebaseAction::Type::Drop;
    }
    return *this;
}

RebasePlan& RebasePlan::reword_all(std::function<std::string(const std::string&)> transformer) {
    for (auto& action : impl_->actions) {
        action.type = RebaseAction::Type::Reword;
        action.new_message = transformer(action.original_message);
    }
    return *this;
}

RebasePlan& RebasePlan::modify_changes(const Oid& commit_oid, ChangeModification mod) {
    for (size_t i = 0; i < impl_->actions.size(); ++i) {
        if (impl_->actions[i].original_oid == commit_oid) {
            return modify_changes(i, std::move(mod));
        }
    }
    throw GitError(ErrorCode::CommitNotFound,
                   fmt::format("commit {} not found in plan", commit_oid.short_id()));
}

RebasePlan& RebasePlan::modify_changes(size_t action_index, ChangeModification mod) {
    impl_->modifications[action_index].push_back(std::move(mod));
    return *this;
}

const std::vector<ChangeModification>& RebasePlan::get_modifications(size_t action_index) const {
    static const std::vector<ChangeModification> empty;
    auto it = impl_->modifications.find(action_index);
    return it != impl_->modifications.end() ? it->second : empty;
}

Oid RebasePlan::onto() const {
    return impl_->onto;
}

RebasePlan& RebasePlan::set_onto(const Oid& oid) {
    impl_->onto = oid;
    return *this;
}

RebasePlan& RebasePlan::set_onto(std::string_view ref) {
    impl_->onto = impl_->repo->resolve_ref(ref);
    return *this;
}

bool RebasePlan::is_valid() const {
    return validation_errors().empty();
}

std::vector<std::string> RebasePlan::validation_errors() const {
    std::vector<std::string> errors;

    if (impl_->onto.is_zero()) {
        errors.push_back("onto target is not set");
    }

    bool saw_non_squash = false;
    for (size_t i = 0; i < impl_->actions.size(); ++i) {
        const auto& action = impl_->actions[i];

        if (action.type == RebaseAction::Type::Squash ||
            action.type == RebaseAction::Type::Fixup) {
            if (i == 0 || !saw_non_squash) {
                errors.push_back(fmt::format(
                    "action {} ({}) cannot be at the start of the plan",
                    i, action.label()));
            }
        } else if (action.type != RebaseAction::Type::Drop) {
            saw_non_squash = true;
        }

        if (action.type == RebaseAction::Type::Reword && !action.new_message) {
            errors.push_back(fmt::format(
                "action {} (reword) is missing new_message", i));
        }
    }

    return errors;
}

std::string RebasePlan::to_string() const {
    std::ostringstream out;

    for (const auto& action : impl_->actions) {
        std::string summary = action.original_message;
        if (auto nl = summary.find('\n'); nl != std::string::npos) {
            summary = summary.substr(0, nl);
        }

        out << action.label() << " "
            << action.original_oid.short_id() << " "
            << summary << "\n";
    }

    return out.str();
}

RebasePlan RebasePlan::parse(Repository& repo, std::string_view plan_text,
                              std::string_view onto_ref) {
    RebasePlan plan(repo, onto_ref);

    std::regex line_regex(R"(^\s*(pick|reword|edit|squash|fixup|drop|p|r|e|s|f|d)\s+([a-fA-F0-9]+)\s*(.*)?$)");
    std::istringstream stream{std::string(plan_text)};
    std::string line;

    while (std::getline(stream, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        std::smatch match;
        if (!std::regex_match(line, match, line_regex)) {
            continue;  // Skip invalid lines
        }

        std::string cmd = match[1];
        std::string oid_str = match[2];

        // Resolve the OID
        Oid oid(oid_str);
        auto commit_opt = repo.try_lookup_commit(oid);
        if (!commit_opt) {
            throw GitError(ErrorCode::CommitNotFound,
                           fmt::format("commit {} not found", oid_str));
        }

        RebaseAction action;
        action.original_oid = oid;
        action.original_message = commit_opt->message();

        if (cmd == "pick" || cmd == "p") {
            action.type = RebaseAction::Type::Pick;
        } else if (cmd == "reword" || cmd == "r") {
            action.type = RebaseAction::Type::Reword;
        } else if (cmd == "edit" || cmd == "e") {
            action.type = RebaseAction::Type::Edit;
        } else if (cmd == "squash" || cmd == "s") {
            action.type = RebaseAction::Type::Squash;
        } else if (cmd == "fixup" || cmd == "f") {
            action.type = RebaseAction::Type::Fixup;
        } else if (cmd == "drop" || cmd == "d") {
            action.type = RebaseAction::Type::Drop;
        }

        plan.add(std::move(action));
    }

    return plan;
}

}  // namespace gitmanip
