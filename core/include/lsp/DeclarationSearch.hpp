// DeclarationSearch.hpp -- text-search fallback for "go to definition" and
// "find references" when no language server can answer.
//
// A host without a toolchain (a Linux box asked about Swift, say), a
// server that is installed but knows nothing about the project, or a
// server that simply answers empty all leave the user with "no
// definitions found" for a symbol that is declared three files away.
// This module scans the workspace's source files for declaration-shaped
// lines mentioning the identifier under the cursor and ranks them, so
// the client always has *something* to show -- clearly labelled as a
// text match, never confused with a semantic answer.
//
// Pure: no server, no index, no network. Bounded by TextSearchLimits.
#pragma once

#include "lsp/LSPClient.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace gitreview {
namespace lsp {

struct TextSearchLimits {
    std::size_t maxFiles = 20000;       // files visited before giving up
    std::size_t maxFileBytes = 1u << 20; // larger files are skipped
    std::size_t maxResults = 500;       // results returned (after ranking)
};

/// The identifier ([A-Za-z_][A-Za-z0-9_]*) under `character` on `line`
/// (0-based, LSP-style) of `content`; a cursor just past an identifier
/// selects it. Empty when there is none.
std::string identifierAt(const std::string& content, int line, int character);

/// File extensions (lower-case, with the dot) that belong to `language`'s
/// family. For an unknown language the requesting file's own extension;
/// empty means "any text file".
std::vector<std::string> extensionsForSearch(Language language, const std::string& requestingFile);

/// 0 when `line` does not declare `name`; otherwise a rank -- 3 for a
/// type-level declaration (class/struct/enum/protocol/...), 2 for a
/// function, 1 for a variable/constant/case. Language-specific patterns
/// with a generic set for unknown languages.
int declarationRank(Language language, const std::string& line, const std::string& name);

/// Byte column of the first whole-word occurrence of `name` in `line`
/// at or after `from`, or npos.
std::size_t findWord(const std::string& line, const std::string& name, std::size_t from = 0);

/// Declaration candidates for `name` across the workspace, best first:
/// higher rank, then the requesting file, then the requesting file's
/// directory, then path order.
std::vector<LSPLocation> findDeclarations(const std::string& workspacePath, Language language,
                                          const std::string& requestingFile, const std::string& name,
                                          const TextSearchLimits& limits = {});

/// Whole-word occurrences of `name` across the workspace (the text
/// analogue of find-references). Declaration lines are dropped when
/// `includeDeclarations` is false. Requesting file first, then path order.
std::vector<LSPLocation> findOccurrences(const std::string& workspacePath, Language language,
                                         const std::string& requestingFile, const std::string& name,
                                         bool includeDeclarations, const TextSearchLimits& limits = {});

/// "file://" + path (no percent-encoding; matches the agent's URIs).
std::string toFileUri(const std::string& path);
/// Inverse of toFileUri; a plain path passes through unchanged.
std::string fromFileUri(const std::string& uri);

}  // namespace lsp
}  // namespace gitreview
