// SPDX-License-Identifier: GPL-3.0-or-later
//
// sync_directory where no platform body exists (file.h). Its own file so
// that a platform with one (src/windows/file.cpp) replaces only this.
#include "sonora/platform/file.h"

#include <cerrno>

namespace sonora::platform {

// Standard C++ cannot sync a directory, so no durability is claimed.
bool sync_directory(const std::string&) {
    errno = ENOSYS;
    return false;
}

}  // namespace sonora::platform
