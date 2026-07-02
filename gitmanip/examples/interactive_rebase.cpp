// Interactive rebase example: Demonstrating various rebase operations

#include <gitmanip/gitmanip.hpp>
#include <iostream>
#include <sstream>
#include <string>

void print_help() {
    std::cout << R"(
Commands:
  list              - List commits in the plan
  pick <n>          - Set action n to pick
  reword <n> <msg>  - Set action n to reword with new message
  squash <n>        - Set action n to squash
  fixup <n>         - Set action n to fixup
  drop <n>          - Set action n to drop
  move <from> <to>  - Move action from index to new index
  show <n>          - Show details of action n
  plan              - Show full plan text
  validate          - Validate the plan
  execute           - Execute the rebase (dry run)
  execute!          - Execute the rebase (for real!)
  help              - Show this help
  quit              - Exit
)";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <repository-path> <base-ref> [head-ref]\n";
        return 1;
    }

    try {
        auto repo = gitmanip::Repository::open(argv[1]);
        std::string base_ref = argv[2];
        std::string head_ref = argc > 3 ? argv[3] : "HEAD";

        std::cout << "Creating rebase plan from " << base_ref << " to " << head_ref << "\n";

        auto plan = gitmanip::RebasePlan::from_range(repo, base_ref, head_ref);
        gitmanip::Operations ops(repo);

        std::cout << "Found " << plan.size() << " commits. Type 'help' for commands.\n\n";

        std::string line;
        while (true) {
            std::cout << "> ";
            if (!std::getline(std::cin, line)) break;

            std::istringstream iss(line);
            std::string cmd;
            iss >> cmd;

            if (cmd.empty()) continue;

            if (cmd == "quit" || cmd == "q") {
                break;
            } else if (cmd == "help" || cmd == "h") {
                print_help();
            } else if (cmd == "list" || cmd == "l") {
                std::cout << "Actions:\n";
                for (size_t i = 0; i < plan.size(); ++i) {
                    const auto& action = plan.action(i);
                    std::cout << "  " << i << ": " << action.label()
                              << " " << action.original_oid.short_id()
                              << " " << action.original_message.substr(0, 50) << "\n";
                }
            } else if (cmd == "plan" || cmd == "p") {
                std::cout << plan.to_string();
            } else if (cmd == "validate" || cmd == "v") {
                if (plan.is_valid()) {
                    std::cout << "Plan is valid.\n";
                } else {
                    std::cout << "Plan has errors:\n";
                    for (const auto& err : plan.validation_errors()) {
                        std::cout << "  - " << err << "\n";
                    }
                }
            } else if (cmd == "pick") {
                size_t n;
                if (iss >> n && n < plan.size()) {
                    auto commit = repo.lookup_commit(plan.action(n).original_oid);
                    plan.replace(n, gitmanip::RebaseAction::pick(commit));
                    std::cout << "Set action " << n << " to pick.\n";
                }
            } else if (cmd == "reword") {
                size_t n;
                std::string msg;
                if (iss >> n && n < plan.size()) {
                    std::getline(iss, msg);
                    // Trim leading space
                    if (!msg.empty() && msg[0] == ' ') msg = msg.substr(1);
                    auto commit = repo.lookup_commit(plan.action(n).original_oid);
                    plan.replace(n, gitmanip::RebaseAction::reword(commit, msg));
                    std::cout << "Set action " << n << " to reword.\n";
                }
            } else if (cmd == "squash") {
                size_t n;
                if (iss >> n && n < plan.size()) {
                    auto commit = repo.lookup_commit(plan.action(n).original_oid);
                    plan.replace(n, gitmanip::RebaseAction::squash(commit));
                    std::cout << "Set action " << n << " to squash.\n";
                }
            } else if (cmd == "fixup") {
                size_t n;
                if (iss >> n && n < plan.size()) {
                    auto commit = repo.lookup_commit(plan.action(n).original_oid);
                    plan.replace(n, gitmanip::RebaseAction::fixup(commit));
                    std::cout << "Set action " << n << " to fixup.\n";
                }
            } else if (cmd == "drop") {
                size_t n;
                if (iss >> n && n < plan.size()) {
                    auto commit = repo.lookup_commit(plan.action(n).original_oid);
                    plan.replace(n, gitmanip::RebaseAction::drop(commit));
                    std::cout << "Set action " << n << " to drop.\n";
                }
            } else if (cmd == "move") {
                size_t from, to;
                if (iss >> from >> to && from < plan.size() && to < plan.size()) {
                    plan.move(from, to);
                    std::cout << "Moved action " << from << " to " << to << ".\n";
                }
            } else if (cmd == "show") {
                size_t n;
                if (iss >> n && n < plan.size()) {
                    const auto& action = plan.action(n);
                    auto commit = repo.lookup_commit(action.original_oid);

                    std::cout << "Action " << n << ":\n";
                    std::cout << "  Type: " << action.label() << "\n";
                    std::cout << "  OID: " << action.original_oid.to_string() << "\n";
                    std::cout << "  Author: " << commit.author().format() << "\n";
                    std::cout << "  Message:\n" << commit.message() << "\n";

                    if (action.new_message) {
                        std::cout << "  New message: " << *action.new_message << "\n";
                    }

                    auto diff = commit.diff_from_parent();
                    auto stats = diff.stats();
                    std::cout << "  Changes: " << stats.files_changed << " files, "
                              << "+" << stats.insertions << " -" << stats.deletions << "\n";
                }
            } else if (cmd == "execute") {
                ops.set_dry_run(true);
                std::cout << "Executing rebase (dry run)...\n";
                auto result = ops.execute(plan);
                if (result.success) {
                    std::cout << "Would succeed! New head: " << result.new_head.short_id() << "\n";
                } else {
                    std::cout << "Would fail: " << result.conflict_message.value_or("unknown") << "\n";
                }
            } else if (cmd == "execute!") {
                std::cout << "Are you sure? This will modify the repository! (yes/no): ";
                std::string confirm;
                std::getline(std::cin, confirm);
                if (confirm == "yes") {
                    ops.set_dry_run(false);
                    std::cout << "Executing rebase...\n";
                    auto result = ops.execute(plan, [](const gitmanip::RebaseProgress& p) {
                        std::cout << "  " << p.action->label() << " " << p.action->original_oid.short_id();
                        if (p.new_oid) {
                            std::cout << " -> " << p.new_oid->short_id();
                        }
                        std::cout << "\n";
                        return true;
                    });
                    if (result.success) {
                        std::cout << "Success! New head: " << result.new_head.to_string() << "\n";
                    } else {
                        std::cout << "Failed: " << result.conflict_message.value_or("unknown") << "\n";
                    }
                } else {
                    std::cout << "Cancelled.\n";
                }
            } else {
                std::cout << "Unknown command. Type 'help' for usage.\n";
            }
        }

        return 0;

    } catch (const gitmanip::GitError& e) {
        std::cerr << "Git error: " << e.what() << "\n";
        return 1;
    }
}
