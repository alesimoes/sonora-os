// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/platform/file.h"

namespace sonora::platform {

// Standard C11 exclusive create. Nothing here starts processes, so there is
// no inheritance to prevent yet.
std::FILE* create_exclusive(const std::string& path) { return std::fopen(path.c_str(), "wbx"); }

}  // namespace sonora::platform
