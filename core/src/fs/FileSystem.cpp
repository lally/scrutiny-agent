// FileSystem.cpp
// The first `Core` filesystem C surface (`grc_fs_*`). Working-tree file
// reads for the remote agent's `fs.readFile`. Pure I/O -- the kernel
// arbitrates, so the agent runs these with free parallelism (no
// per-repo serialization, unlike libgit2).
//
// Allocation contract: the returned buffer is `new char[]`, matching
// `grc_free_string`'s `delete[]` (GitReviewCore.cpp) so the caller's
// `grc_free_string(*out_content)` is correct.

#include "GitReviewCore.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <string>

extern "C" {

GRCError grc_fs_read_file(const char* path, char** out_content,
                          int64_t* out_size) {
    if (path == nullptr || out_content == nullptr || out_size == nullptr)
        return GRC_ERROR_INVALID_ARGUMENT;
    *out_content = nullptr;
    *out_size = 0;

    std::error_code ec;
    const std::filesystem::file_status st = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::is_regular_file(st))
        return GRC_ERROR_CONNECTION_FAILED;  // ENOENT / dir / EACCES / ...

    std::ifstream f(path, std::ios::binary);
    if (!f) return GRC_ERROR_CONNECTION_FAILED;
    std::string data((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    if (f.bad()) return GRC_ERROR_CONNECTION_FAILED;

    char* buf = new (std::nothrow) char[data.size() + 1];
    if (buf == nullptr) return GRC_ERROR_ALLOCATION_FAILED;
    std::memcpy(buf, data.data(), data.size());
    buf[data.size()] = '\0';
    *out_content = buf;
    *out_size = static_cast<int64_t>(data.size());
    return GRC_SUCCESS;
}

}  // extern "C"
