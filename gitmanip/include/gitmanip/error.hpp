#pragma once

#include <exception>
#include <source_location>
#include <string>
#include <string_view>

namespace gitmanip {

enum class ErrorCode {
    Unknown,
    RepositoryNotFound,
    RepositoryCorrupted,
    ReferenceNotFound,
    CommitNotFound,
    InvalidOid,
    ConflictDetected,
    IndexLocked,
    WorkdirDirty,
    InvalidOperation,
    InvalidArgument,
    IoError,
    MergeConflict,
    EmptyCommit,
    SigningError,
    NotFound,
    NotSupported,
};

class GitError : public std::exception {
public:
    GitError(ErrorCode code, std::string message,
             std::source_location loc = std::source_location::current());

    [[nodiscard]] const char* what() const noexcept override;
    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] const std::source_location& location() const noexcept { return location_; }

private:
    ErrorCode code_;
    std::string message_;
    std::string what_;
    std::source_location location_;
};

class ConflictError : public GitError {
public:
    ConflictError(std::string message,
                  std::source_location loc = std::source_location::current());
};

class RepositoryError : public GitError {
public:
    RepositoryError(ErrorCode code, std::string message,
                    std::source_location loc = std::source_location::current());
};

namespace detail {
void check_libgit2_error(int error_code, std::string_view context,
                         std::source_location loc = std::source_location::current());
}

}  // namespace gitmanip
