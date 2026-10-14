// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/platform/file.h"

#include <cerrno>

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

}  // namespace sonora::platform
