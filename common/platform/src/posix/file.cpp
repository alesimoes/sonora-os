// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/platform/file.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/file.h>
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

FolderLock::~FolderLock() {
    // Closing the descriptor is what releases a flock.
    if (handle_ != -1) ::close(static_cast<int>(handle_));
}

FolderLock::Result FolderLock::acquire(const std::string& path, std::string& error) {
    if (handle_ != -1) return Result::Held;
    // flock and not fcntl: an fcntl lock belongs to the process, so a
    // second holder in it would be granted the lock, and closing any
    // descriptor on the folder would drop it.
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open " + path + ": " + std::strerror(errno);
        return Result::Failed;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int err = errno;
        ::close(fd);
        if (err == EWOULDBLOCK) {
            error = "another engine is using " + path;
            return Result::Busy;
        }
        error = "cannot lock " + path + ": " + std::strerror(err);
        return Result::Failed;
    }
    handle_ = fd;
    return Result::Held;
}

}  // namespace sonora::platform
