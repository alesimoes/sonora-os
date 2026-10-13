// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/platform/file.h"

#include <cerrno>

#include <fcntl.h>
#include <unistd.h>

namespace sonora::platform {

std::FILE* create_exclusive(const std::string& path) {
    // In the open itself, not a later fcntl: another thread may fork and
    // exec in between.
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0) return nullptr;
    std::FILE* f = ::fdopen(fd, "wb");
    if (f == nullptr) {
        const int err = errno;
        ::close(fd);
        // Created just now by the open above, so it is ours to remove.
        ::unlink(path.c_str());
        errno = err;
    }
    return f;
}

}  // namespace sonora::platform
