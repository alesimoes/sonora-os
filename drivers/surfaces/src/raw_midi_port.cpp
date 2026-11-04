// SPDX-License-Identifier: GPL-3.0-or-later
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "sonora/surfaces/midi_port.h"

namespace sonora::surfaces {

RawMidiPort* RawMidiPort::open(const std::string& path, std::string& error) {
    const int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open " + path + ": " + std::strerror(errno);
        return nullptr;
    }
    return new RawMidiPort(fd, fd);
}

RawMidiPort::RawMidiPort(int read_fd, int write_fd) : read_fd_(read_fd), write_fd_(write_fd) {
    if (read_fd_ >= 0) ::fcntl(read_fd_, F_SETFL, ::fcntl(read_fd_, F_GETFL) | O_NONBLOCK);
    if (write_fd_ >= 0) ::fcntl(write_fd_, F_SETFL, ::fcntl(write_fd_, F_GETFL) | O_NONBLOCK);
}

RawMidiPort::~RawMidiPort() {
    if (read_fd_ >= 0) ::close(read_fd_);
    if (write_fd_ >= 0 && write_fd_ != read_fd_) ::close(write_fd_);
}

std::size_t RawMidiPort::read(std::uint8_t* buf, std::size_t cap) {
    if (read_fd_ < 0) return 0;
    for (;;) {
        const ssize_t n = ::read(read_fd_, buf, cap);
        if (n >= 0) return static_cast<std::size_t>(n);
        if (errno == EINTR) continue;
        return 0;  // EAGAIN, or a device that went away: nothing to read
    }
}

bool RawMidiPort::write(const std::uint8_t* bytes, std::size_t n) {
    if (write_fd_ < 0) return false;
    std::size_t done = 0;
    while (done < n) {
        const ssize_t w = ::write(write_fd_, bytes + done, n - done);
        if (w > 0) {
            done += static_cast<std::size_t>(w);
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace sonora::surfaces
