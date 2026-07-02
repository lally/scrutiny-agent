#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct git_repository;
struct git_commit;
struct git_tree;
struct git_index;
struct git_reference;
struct git_signature;
struct git_diff;
struct git_oid;

namespace gitmanip {

class Repository;
class Commit;
class Tree;
class Diff;
class Index;

// Object ID (SHA-1 hash)
class Oid {
public:
    static constexpr size_t Size = 20;
    static constexpr size_t HexSize = 40;

    Oid() = default;
    explicit Oid(const git_oid* oid);
    explicit Oid(std::string_view hex);

    [[nodiscard]] std::string to_string() const;
    [[nodiscard]] std::string short_id(size_t length = 7) const;
    [[nodiscard]] bool is_zero() const;
    [[nodiscard]] const std::array<uint8_t, Size>& data() const { return data_; }

    // Only compare the actual OID data, not the cache
    auto operator<=>(const Oid& other) const { return data_ <=> other.data_; }
    bool operator==(const Oid& other) const { return data_ == other.data_; }

    // For libgit2 interop
    [[nodiscard]] const git_oid* raw() const;

private:
    std::array<uint8_t, Size> data_{};
    // Cache for raw() - uses aligned storage to avoid incomplete type issue
    // git_oid is exactly 20 bytes (GIT_OID_RAWSZ)
    mutable std::array<uint8_t, Size> raw_cache_{};
    mutable bool raw_cache_valid_ = false;
};

// Author/committer signature
struct Signature {
    std::string name;
    std::string email;
    std::chrono::system_clock::time_point time;
    int offset_minutes = 0;  // Timezone offset

    [[nodiscard]] std::string format() const;

    static Signature now(std::string_view name, std::string_view email);
};

// A single change within a diff
struct DiffDelta {
    enum class Status {
        Unmodified,
        Added,
        Deleted,
        Modified,
        Renamed,
        Copied,
        Ignored,
        Untracked,
        Typechange,
        Unreadable,
        Conflicted,
    };

    Status status;
    std::string old_path;
    std::string new_path;
    Oid old_oid;
    Oid new_oid;
    uint32_t old_mode;
    uint32_t new_mode;
    uint32_t similarity;  // For renames/copies (0-100)
};

// A hunk of changes within a file
struct DiffHunk {
    int old_start;
    int old_lines;
    int new_start;
    int new_lines;
    std::string header;
};

// A single line change
struct DiffLine {
    enum class Origin : char {
        Context = ' ',
        Addition = '+',
        Deletion = '-',
        ContextEOFNL = '=',
        AddEOFNL = '>',
        DelEOFNL = '<',
        FileHeader = 'F',
        HunkHeader = 'H',
        Binary = 'B',
    };

    Origin origin;
    std::string content;
    int old_lineno;  // -1 if not applicable
    int new_lineno;  // -1 if not applicable
};

// File entry in a tree
struct TreeEntry {
    enum class Type {
        Blob,
        Tree,
        Commit,  // Submodule
    };

    std::string name;
    Oid oid;
    Type type;
    uint32_t filemode;
};

// Smart pointer deleters for libgit2 objects
namespace detail {

template <typename T>
struct GitDeleter;

template <>
struct GitDeleter<git_repository> {
    void operator()(git_repository* ptr) const;
};

template <>
struct GitDeleter<git_commit> {
    void operator()(git_commit* ptr) const;
};

template <>
struct GitDeleter<git_tree> {
    void operator()(git_tree* ptr) const;
};

template <>
struct GitDeleter<git_index> {
    void operator()(git_index* ptr) const;
};

template <>
struct GitDeleter<git_reference> {
    void operator()(git_reference* ptr) const;
};

template <>
struct GitDeleter<git_signature> {
    void operator()(git_signature* ptr) const;
};

template <>
struct GitDeleter<git_diff> {
    void operator()(git_diff* ptr) const;
};

template <typename T>
using GitPtr = std::unique_ptr<T, GitDeleter<T>>;

}  // namespace detail

}  // namespace gitmanip
