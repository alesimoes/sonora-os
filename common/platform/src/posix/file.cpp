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

bool sync_data(std::FILE* f) {
    if (std::fflush(f) != 0) return false;
    return ::fdatasync(::fileno(f)) == 0 || errno == EINVAL;
}

bool sync_directory(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    // Some filesystems refuse fsync on a directory with EINVAL; their
    // entries are as durable as they will ever be.
    const bool ok = ::fsync(fd) == 0 || errno == EINVAL;
    const int err = errno;
    ::close(fd);
    errno = err;
    return ok;
}

}  // namespace sonora::platform
