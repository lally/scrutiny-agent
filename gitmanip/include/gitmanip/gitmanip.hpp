#pragma once

/// @file gitmanip.hpp
/// @brief Main include file for the gitmanip library.
///
/// Include this single header to access all gitmanip functionality.
/// For more targeted includes, use the individual headers.
///
/// @section overview Overview
///
/// gitmanip is a modern C++23 library for programmatic Git repository manipulation.
/// It provides a high-level, type-safe interface built on top of libgit2.
///
/// @section features Key Features
///
/// - **Repository Access**: Open, create, and query Git repositories
/// - **Commit Operations**: Read commit data, walk history, compare commits
/// - **Diff & Blame**: Generate diffs, get line-by-line blame information
/// - **History Tracking**: Find file introductions, track renames, search commits
/// - **Rebase Operations**: Programmatically rebase, squash, reword, and drop commits
/// - **Tree Manipulation**: Build and modify Git trees for custom commits
/// - **Commit/Tag Signing**: GPG and SSH signing with automatic config loading
///
/// @section quick_start Quick Start
///
/// @code{.cpp}
/// #include <gitmanip/gitmanip.hpp>
/// #include <iostream>
///
/// int main() {
///     // Open a repository
///     auto repo = gitmanip::Repository::open("/path/to/repo");
///
///     // Get HEAD commit
///     auto head = repo.lookup_commit("HEAD");
///     std::cout << "HEAD: " << head.summary() << "\n";
///
///     // Walk recent commits
///     for (auto commit : repo.walk_commits().push_head().walk()) {
///         std::cout << commit.id().short_id() << " " << commit.summary() << "\n";
///     }
///
///     // Get blame for a file
///     auto blame = gitmanip::Blame::file(repo, "src/main.cpp");
///     for (const auto& hunk : blame.hunks()) {
///         std::cout << "Lines " << hunk.start_line << "-" << hunk.end_line()
///                   << " by " << hunk.final_signature.name << "\n";
///     }
///
///     return 0;
/// }
/// @endcode
///
/// @section headers Individual Headers
///
/// | Header | Description |
/// |--------|-------------|
/// | `<gitmanip/repository.hpp>` | Repository access and management |
/// | `<gitmanip/commit.hpp>` | Commit data and history walking |
/// | `<gitmanip/tree.hpp>` | Tree reading and building |
/// | `<gitmanip/diff.hpp>` | Diff generation and analysis |
/// | `<gitmanip/blame.hpp>` | Line-by-line blame/annotation |
/// | `<gitmanip/history.hpp>` | File history and commit search |
/// | `<gitmanip/rebase_plan.hpp>` | Rebase plan construction |
/// | `<gitmanip/operations.hpp>` | Rebase execution and commit building |
/// | `<gitmanip/signing.hpp>` | GPG/SSH commit and tag signing |
/// | `<gitmanip/status.hpp>` | Working directory and sync status |
/// | `<gitmanip/types.hpp>` | Core types (Oid, Signature, etc.) |
/// | `<gitmanip/error.hpp>` | Exception types |
///
/// @section error_handling Error Handling
///
/// gitmanip uses exceptions for error handling. All errors derive from
/// `gitmanip::GitError`, which includes an error code, message, and
/// source location.
///
/// @code{.cpp}
/// try {
///     auto repo = gitmanip::Repository::open("/nonexistent");
/// } catch (const gitmanip::GitError& e) {
///     std::cerr << "Error: " << e.what() << "\n";
///     std::cerr << "Code: " << static_cast<int>(e.code()) << "\n";
/// }
/// @endcode
///
/// @section thread_safety Thread Safety
///
/// - Repository objects are NOT thread-safe. Use separate Repository instances
///   per thread, or provide external synchronization.
/// - Commits, Trees, and other objects returned from a Repository share its
///   lifetime and should not be used after the Repository is destroyed.
/// - libgit2 initialization is handled automatically and is thread-safe.

#include "blame.hpp"
#include "commit.hpp"
#include "diff.hpp"
#include "error.hpp"
#include "history.hpp"
#include "operations.hpp"
#include "rebase_plan.hpp"
#include "refs.hpp"
#include "repository.hpp"
#include "signing.hpp"
#include "status.hpp"
#include "tree.hpp"
#include "types.hpp"

namespace gitmanip {

/// @brief Major version number.
constexpr int VERSION_MAJOR = 0;

/// @brief Minor version number.
constexpr int VERSION_MINOR = 2;

/// @brief Patch version number.
constexpr int VERSION_PATCH = 0;

/// @brief Full version string.
constexpr const char* VERSION_STRING = "0.2.0";

}  // namespace gitmanip
