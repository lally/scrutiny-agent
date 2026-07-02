#include "gitmanip/error.hpp"

#include <git2.h>
#include <fmt/format.h>

namespace gitmanip {

GitError::GitError(ErrorCode code, std::string message, std::source_location loc)
    : code_(code), message_(std::move(message)), location_(loc) {
    what_ = fmt::format("{}:{} - {} ({})",
                        location_.file_name(),
                        location_.line(),
                        message_,
                        static_cast<int>(code_));
}

const char* GitError::what() const noexcept {
    return what_.c_str();
}

ConflictError::ConflictError(std::string message, std::source_location loc)
    : GitError(ErrorCode::ConflictDetected, std::move(message), loc) {}

RepositoryError::RepositoryError(ErrorCode code, std::string message, std::source_location loc)
    : GitError(code, std::move(message), loc) {}

namespace detail {

void check_libgit2_error(int error_code, std::string_view context, std::source_location loc) {
    if (error_code >= 0) {
        return;
    }

    const git_error* err = git_error_last();
    std::string message;
    ErrorCode code = ErrorCode::Unknown;

    if (err) {
        message = fmt::format("{}: {}", context, err->message);

        // Map libgit2 error classes to our error codes
        switch (err->klass) {
            case GIT_ERROR_REPOSITORY:
                code = ErrorCode::RepositoryCorrupted;
                break;
            case GIT_ERROR_REFERENCE:
                code = ErrorCode::ReferenceNotFound;
                break;
            case GIT_ERROR_OBJECT:
                code = ErrorCode::CommitNotFound;
                break;
            case GIT_ERROR_INDEX:
                code = ErrorCode::IndexLocked;
                break;
            case GIT_ERROR_MERGE:
                code = ErrorCode::MergeConflict;
                break;
            case GIT_ERROR_CHECKOUT:
                code = ErrorCode::WorkdirDirty;
                break;
            case GIT_ERROR_OS:
                code = ErrorCode::IoError;
                break;
            case GIT_ERROR_INVALID:
                code = ErrorCode::InvalidArgument;
                break;
            default:
                code = ErrorCode::Unknown;
                break;
        }
    } else {
        message = fmt::format("{}: unknown error (code {})", context, error_code);
    }

    // Handle specific error codes
    switch (error_code) {
        case GIT_ENOTFOUND:
            if (code == ErrorCode::Unknown) {
                code = ErrorCode::CommitNotFound;
            }
            break;
        case GIT_ECONFLICT:
            code = ErrorCode::ConflictDetected;
            break;
        case GIT_ELOCKED:
            code = ErrorCode::IndexLocked;
            break;
        case GIT_EUNMERGED:
            code = ErrorCode::MergeConflict;
            break;
        case GIT_EINVALIDSPEC:
            code = ErrorCode::InvalidOid;
            break;
        default:
            break;
    }

    throw GitError(code, std::move(message), loc);
}

}  // namespace detail

}  // namespace gitmanip
