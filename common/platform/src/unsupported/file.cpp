// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/platform/file.h"

#include <cerrno>
#include <filesystem>

namespace sonora::platform {

// Standard C11 exclusive create. Nothing here starts processes, so there is
// no inheritance to prevent yet.
std::FILE* create_exclusive(const std::string& path) { return std::fopen(path.c_str(), "wbx"); }

// Standard C flushes to the OS and no further, so no durability is claimed.
bool sync_data(std::FILE* f) {
    if (std::fflush(f) != 0) return false;
    errno = ENOSYS;
    return false;
}

bool sync_directory(const std::string&) {
    errno = ENOSYS;
    return false;
}

FolderLock::~FolderLock() = default;

// Standard C++ has no file lock, so nothing is held and nothing excluded.
FolderLock::Result FolderLock::acquire(const std::string&, std::string&) { return Result::Held; }

// No per-user cache folder is known here yet: the catalog stays in memory.
std::string user_cache_dir() { return {}; }

// Standard C++ has no change time; the modification time stands for both.
bool path_status(const std::string& path, PathStatus& out) {
    std::error_code ec;
    const auto st = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(st)) return false;
    out.regular = std::filesystem::is_regular_file(st);
    out.directory = std::filesystem::is_directory(st);
    out.size = out.regular ? static_cast<uint64_t>(std::filesystem::file_size(path, ec)) : 0;
    const auto t = std::filesystem::last_write_time(path, ec);
    out.modified_ns = ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
    out.changed_ns = 0;
    return true;
}

}  // namespace sonora::platform
