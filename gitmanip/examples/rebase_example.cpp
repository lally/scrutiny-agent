// Rebase example: Demonstrating programmatic rebase operations

#include <gitmanip/gitmanip.hpp>
#include <iostream>

void print_commits(gitmanip::Repository& repo, const std::string& ref_name) {
    std::cout << "\nCommits on " << ref_name << ":\n";
    auto walker = repo.walk_commits();
    walker.push_ref(ref_name);
    walker.sort(gitmanip::CommitWalker::SortOrder::Topological);

    for (auto&& commit : walker.walk()) {
        std::cout << "  " << commit.id().short_id() << " " << commit.summary() << "\n";
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <repository-path> <base-ref> [head-ref]\n";
        std::cerr << "Example: " << argv[0] << " /path/to/repo main feature-branch\n";
        return 1;
    }

    std::string repo_path = argv[1];
    std::string base_ref = argv[2];
    std::string head_ref = argc > 3 ? argv[3] : "HEAD";

    try {
        auto repo = gitmanip::Repository::open(repo_path);

        std::cout << "Creating rebase plan from " << base_ref << " to " << head_ref << "\n";

        // Get commits between the two refs
        auto commits = repo.commits_between(base_ref, head_ref);

        std::cout << "Found " << commits.size() << " commits to rebase:\n";
        for (const auto& commit : commits) {
            std::cout << "  " << commit.id().short_id() << " " << commit.summary() << "\n";
        }

        // Create a rebase plan
        auto plan = gitmanip::RebasePlan::from_range(repo, base_ref, head_ref);

        // Print the plan in git rebase -i format
        std::cout << "\nRebase plan:\n" << plan.to_string();

        // Example: Reword all commit messages to add a prefix
        std::cout << "\nModifying plan to add prefix to all messages...\n";
        plan.reword_all([](const std::string& msg) {
            return "[REBASED] " + msg;
        });

        std::cout << "Modified plan:\n" << plan.to_string();

        // Validate the plan
        if (!plan.is_valid()) {
            std::cerr << "Plan is invalid:\n";
            for (const auto& error : plan.validation_errors()) {
                std::cerr << "  - " << error << "\n";
            }
            return 1;
        }

        // Execute the rebase (dry run by default for safety)
        std::cout << "\nExecuting rebase (dry run)...\n";
        gitmanip::Operations ops(repo);
        ops.set_dry_run(true);

        auto result = ops.execute(plan, [](const gitmanip::RebaseProgress& progress) {
            std::cout << "  Processing " << (progress.current_action + 1)
                      << "/" << progress.total_actions
                      << ": " << progress.action->label()
                      << " " << progress.action->original_oid.short_id() << "\n";
            return true;  // Continue
        });

        if (result.success) {
            std::cout << "\nRebase would succeed!\n";
            std::cout << "New head would be: " << result.new_head.short_id() << "\n";
        } else {
            std::cout << "\nRebase would fail: "
                      << result.conflict_message.value_or("unknown error") << "\n";
        }

        return 0;

    } catch (const gitmanip::GitError& e) {
        std::cerr << "Git error: " << e.what() << "\n";
        return 1;
    }
}
