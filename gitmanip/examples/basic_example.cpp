// Basic example: Opening a repository, reading commits, and creating new ones

#include <gitmanip/gitmanip.hpp>
#include <iostream>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <repository-path>\n";
        return 1;
    }

    try {
        // Open the repository
        auto repo = gitmanip::Repository::open(argv[1]);
        std::cout << "Opened repository: " << repo.path() << "\n";
        std::cout << "Working directory: " << repo.workdir() << "\n";
        std::cout << "Is bare: " << (repo.is_bare() ? "yes" : "no") << "\n";

        // Get HEAD info
        if (!repo.is_empty()) {
            auto head_oid = repo.head_oid();
            std::cout << "\nHEAD: " << head_oid.to_string() << "\n";

            // Lookup and display the HEAD commit
            auto head_commit = repo.lookup_commit(head_oid);
            std::cout << "Message: " << head_commit.summary() << "\n";
            std::cout << "Author: " << head_commit.author().format() << "\n";

            // Walk recent commits
            std::cout << "\nRecent commits:\n";
            auto walker = repo.walk_commits();
            walker.push_head();
            walker.sort(gitmanip::CommitWalker::SortOrder::Time);

            int count = 0;
            for (auto&& commit : walker.walk()) {
                std::cout << "  " << commit.id().short_id()
                          << " " << commit.summary() << "\n";
                if (++count >= 10) break;
            }

            // Show branches
            std::cout << "\nBranches:\n";
            for (const auto& branch : repo.branch_names()) {
                std::cout << "  " << branch << "\n";
            }

            // Show diff from HEAD to parent
            if (head_commit.parent_count() > 0) {
                auto diff = head_commit.diff_from_parent();
                auto stats = diff.stats();
                std::cout << "\nHEAD diff stats:\n";
                std::cout << "  Files changed: " << stats.files_changed << "\n";
                std::cout << "  Insertions: " << stats.insertions << "\n";
                std::cout << "  Deletions: " << stats.deletions << "\n";

                std::cout << "\nChanged files:\n";
                for (auto delta : diff.deltas()) {
                    const char* status_str = "?";
                    switch (delta.status) {
                        case gitmanip::DiffDelta::Status::Added: status_str = "A"; break;
                        case gitmanip::DiffDelta::Status::Deleted: status_str = "D"; break;
                        case gitmanip::DiffDelta::Status::Modified: status_str = "M"; break;
                        case gitmanip::DiffDelta::Status::Renamed: status_str = "R"; break;
                        default: break;
                    }
                    std::cout << "  " << status_str << " " << delta.new_path << "\n";
                }
            }
        }

        return 0;

    } catch (const gitmanip::GitError& e) {
        std::cerr << "Git error: " << e.what() << "\n";
        return 1;
    }
}
