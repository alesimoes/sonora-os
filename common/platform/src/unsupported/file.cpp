// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/platform/file.h"

#include <cerrno>
#include <chrono>
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

bool free_bytes(const std::string& path, uint64_t& out) {
    std::error_code ec;
    const std::filesystem::space_info s = std::filesystem::space(path, ec);
    if (ec) {
        errno = ec.value();
        return false;
    }
    out = static_cast<uint64_t>(s.available);
    return true;
}

// Standard C++ cannot see a read-only mount or this process's own access
// without writing a probe, which a read-only check must never do.
bool read_only(const std::string& path) {
    std::error_code ec;
    const auto perms = std::filesystem::status(path, ec).permissions();
    if (ec) return true;
    return (perms & std::filesystem::perms::owner_write) == std::filesystem::perms::none;
}

// Standard C++ cannot see which volume a path is on.
bool same_filesystem(const std::string&, const std::string&) { return false; }

// No durability is claimed (sync_directory), and the check for `to` is not
// atomic with the rename. Windows, later: MoveFileExW (ADR-0018 §9).
bool rename_dir_durably(const std::string& from, const std::string& to, std::string& error) {
    std::error_code ec;
    if (std::filesystem::exists(to, ec)) {
        error = "cannot rename " + from + " to " + to + ": it exists";
        return false;
    }
    std::filesystem::rename(from, to, ec);
    if (ec) {
        error = "cannot rename " + from + " to " + to + ": " + ec.message();
        return false;
    }
    error = "renamed, but this platform cannot make the new name durable";
    return true;
}

FolderLock::~FolderLock() = default;

// Standard C++ has no file lock, so nothing is held and nothing excluded.
FolderLock::Result FolderLock::acquire(const std::string&, std::string&) { return Result::Held; }

// No per-user cache folder is known here yet: the catalog stays in memory.
std::string user_cache_dir() { return {}; }

std::string user_config_dir() { return {}; }

// Standard C++ has no change time; the modification time stands for both.
bool path_status(const std::string& path, PathStatus& out) {
    std::error_code ec;
    const auto st = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::exists(st)) return false;
    out.regular = std::filesystem::is_regular_file(st);
    out.directory = std::filesystem::is_directory(st);
    out.size = out.regular ? static_cast<uint64_t>(std::filesystem::file_size(path, ec)) : 0;
    const auto t = std::filesystem::last_write_time(path, ec);
    // Nanoseconds of the file clock, whose epoch is the platform's (1601
    // on Windows), not Unix's: comparable with this platform's own stamps,
    // which is all a fingerprint does with it (rt-engineer on #95).
    out.modified_ns =
        ec ? 0 : static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
    out.changed_ns = 0;
    return true;
}

}  // namespace sonora::platform
