// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/platform/ipc.h"

#include "region_descriptor.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>

// glibc before 2.32 lacks the name; the kernel has had it since 5.1.
#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif

namespace sonora::platform {

namespace {

std::string os_error(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

// A region a client may map: well past any state block, small enough that a
// hostile descriptor cannot make a client map gigabytes.
constexpr std::size_t kMaxRegionBytes = std::size_t{256} << 20;

bool make_address(const std::string& endpoint, sockaddr_un& addr, std::string& error) {
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (endpoint.empty() || endpoint.size() >= sizeof addr.sun_path) {
        error = "socket path \"" + endpoint + "\" is empty or longer than " +
                std::to_string(sizeof addr.sun_path - 1) + " bytes";
        return false;
    }
    std::memcpy(addr.sun_path, endpoint.c_str(), endpoint.size());
    return true;
}

// Owner-only, because the socket is the engine's whole control surface. A
// directory that already exists must be ours alone and not a symlink: under
// /tmp anyone can create /tmp/sonora-<uid> first, and a directory open to
// others lets them swap the socket for their own.
bool make_parent_dirs(const std::string& endpoint, std::string& error) {
    const std::size_t slash = endpoint.rfind('/');
    if (slash == std::string::npos) {
        error = "socket path \"" + endpoint + "\" names no directory";
        return false;
    }
    const std::string dir = slash == 0 ? std::string("/") : endpoint.substr(0, slash);
    for (std::size_t i = 1; i <= dir.size(); ++i) {
        if (i != dir.size() && dir[i] != '/') continue;
        const std::string part = dir.substr(0, i);
        if (::mkdir(part.c_str(), 0700) != 0 && errno != EEXIST) {
            error = os_error(("cannot create " + part).c_str());
            return false;
        }
    }
    struct stat st {};
    if (::lstat(dir.c_str(), &st) != 0) {
        error = os_error(("cannot inspect " + dir).c_str());
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        error = "the socket's directory " + dir + " is not a directory (a symlink?)";
        return false;
    }
    if (st.st_uid != ::geteuid()) {
        error = "the socket's directory " + dir + " belongs to uid " +
                std::to_string(st.st_uid) + ", not to this user";
        return false;
    }
    if ((st.st_mode & 077) != 0) {
        char mode[8];
        std::snprintf(mode, sizeof mode, "%04o", static_cast<unsigned>(st.st_mode & 07777));
        error = "the socket's directory " + dir + " is open to others (mode " + mode +
                "); it must be 0700";
        return false;
    }
    return true;
}

// The user at the other end of a connected socket.
bool peer_uid(int fd, uid_t& uid, std::string& error) {
    ucred cred{};
    socklen_t len = sizeof cred;
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
        error = os_error("SO_PEERCRED");
        return false;
    }
    uid = cred.uid;
    return true;
}

int remaining_ms(std::chrono::steady_clock::time_point deadline, int timeout_ms) {
    if (timeout_ms < 0) return -1;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    return left > 0 ? static_cast<int>(left) : 0;
}

// 1: ready; 0: timed out; -1: error (errno set).
int wait_for(int fd, short events, int timeout_ms) {
    pollfd p{fd, events, 0};
    for (;;) {
        const int r = ::poll(&p, 1, timeout_ms);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

}  // namespace

// --- SharedRegion -----------------------------------------------------------

struct SharedRegion::Impl {
    int fd = -1;      // the memfd, in the creator only
    int ro_fd = -1;   // what is sent: opened read-only
    void* addr = nullptr;
    std::size_t bytes = 0;
    bool writable = false;
    // F_SEAL_FUTURE_WRITE took: no new writable mapping, by anyone.
    bool write_sealed = false;
    // create_shared_writable: fd itself may be handed to a child.
    bool shares_writable = false;

    ~Impl() {
        if (addr != nullptr) ::munmap(addr, bytes);
        if (fd >= 0) ::close(fd);
        if (ro_fd >= 0) ::close(ro_fd);
    }
};

bool ipc_supported() { return true; }

std::string default_control_endpoint() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg != nullptr && xdg[0] != '\0') return std::string(xdg) + "/sonora/engine.sock";
    return "/tmp/sonora-" + std::to_string(::getuid()) + "/engine.sock";
}

bool endpoint_live(const std::string& endpoint) {
    std::string error;
    return ControlChannel::connect(endpoint, error) != nullptr;
}

std::unique_ptr<SharedRegion> SharedRegion::create(const std::string& name, std::size_t bytes,
                                                   std::string& error) {
    auto impl = std::make_unique<Impl>();
    impl->fd = ::memfd_create(name.c_str(), MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (impl->fd < 0) {
        error = os_error("memfd_create");
        return nullptr;
    }
    if (::ftruncate(impl->fd, static_cast<off_t>(bytes)) != 0) {
        error = os_error("ftruncate");
        return nullptr;
    }
    impl->addr = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, impl->fd, 0);
    if (impl->addr == MAP_FAILED) {
        impl->addr = nullptr;
        error = os_error("mmap");
        return nullptr;
    }
    impl->bytes = bytes;
    impl->writable = true;

    // After our own mapping: FUTURE_WRITE refuses every writable mapping made
    // from now on, by anyone, through any descriptor of this file -- which
    // covers a client that reopens /proc/<pid>/fd/N read-write. Kernels
    // before 5.1 do not know it; the read-only descriptor still stops the
    // plain path there.
    if (::fcntl(impl->fd, F_ADD_SEALS,
                F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL) == 0) {
        impl->write_sealed = true;
    } else {
        ::fcntl(impl->fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
    }
    // A descriptor whose open file description is read-only: a MAP_SHARED
    // PROT_WRITE mapping of it fails with EACCES, and so does mprotect.
    const std::string self = "/proc/self/fd/" + std::to_string(impl->fd);
    impl->ro_fd = ::open(self.c_str(), O_RDONLY | O_CLOEXEC);
    if (impl->ro_fd < 0) {
        error = os_error("cannot open a read-only descriptor of the region");
        return nullptr;
    }
    return std::make_unique<SharedRegion>(std::move(impl));
}

std::unique_ptr<SharedRegion> SharedRegion::create_shared_writable(const std::string& name,
                                                                   std::size_t bytes,
                                                                   std::string& error) {
    auto impl = std::make_unique<Impl>();
    impl->fd = ::memfd_create(name.c_str(), MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (impl->fd < 0) {
        error = os_error("memfd_create");
        return nullptr;
    }
    if (::ftruncate(impl->fd, static_cast<off_t>(bytes)) != 0) {
        error = os_error("ftruncate");
        return nullptr;
    }
    impl->addr = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, impl->fd, 0);
    if (impl->addr == MAP_FAILED) {
        impl->addr = nullptr;
        error = os_error("mmap");
        return nullptr;
    }
    impl->bytes = bytes;
    impl->writable = true;
    impl->shares_writable = true;
    // The size is part of the layout both sides computed; a peer that
    // shrank the file would turn the creator's next access into SIGBUS.
    if (::fcntl(impl->fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
        error = os_error("cannot seal the region's size");
        return nullptr;
    }
    return std::make_unique<SharedRegion>(std::move(impl));
}

SharedRegion::SharedRegion(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SharedRegion::~SharedRegion() = default;
void* SharedRegion::writable_data() { return impl_->writable ? impl_->addr : nullptr; }
const void* SharedRegion::data() const { return impl_->addr; }
std::size_t SharedRegion::bytes() const { return impl_->bytes; }
bool SharedRegion::writable() const { return impl_->writable; }
bool SharedRegion::sealed_against_writes() const { return impl_->write_sealed; }

bool SharedRegion::lock(std::string& error) {
    if (impl_->addr == nullptr || impl_->bytes == 0) return true;
    if (::mlock(impl_->addr, impl_->bytes) == 0) return true;
    error = os_error(("mlock of " + std::to_string(impl_->bytes) + " bytes").c_str());
    return false;
}

int region_descriptor(const SharedRegion& region, bool writable) {
    const SharedRegion::Impl& impl = region.impl();
    if (writable) return impl.shares_writable ? impl.fd : -1;
    return impl.ro_fd;
}

namespace {

std::unique_ptr<SharedRegion> map_received(int fd, std::string& error) {
    auto impl = std::make_unique<SharedRegion::Impl>();
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        error = os_error("fstat of the received region");
        ::close(fd);
        return nullptr;
    }
    if (st.st_size <= 0 || static_cast<std::size_t>(st.st_size) > kMaxRegionBytes) {
        error = "the received region is " + std::to_string(st.st_size) + " bytes";
        ::close(fd);
        return nullptr;
    }
    const auto bytes = static_cast<std::size_t>(st.st_size);
    void* addr = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
    // The mapping holds the file; the descriptor is not needed.
    ::close(fd);
    if (addr == MAP_FAILED) {
        error = os_error("mmap of the received region");
        return nullptr;
    }
    impl->addr = addr;
    impl->bytes = bytes;
    impl->writable = false;
    return std::make_unique<SharedRegion>(std::move(impl));
}

}  // namespace

// --- ControlChannel ---------------------------------------------------------

struct ControlChannel::Impl {
    int fd = -1;
    std::atomic<bool> shut{false};

    ~Impl() {
        if (fd >= 0) ::close(fd);
    }
};

ControlChannel::ControlChannel(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ControlChannel::~ControlChannel() = default;

int ControlChannel::native_handle() const { return impl_->fd; }

bool ControlChannel::pair(std::unique_ptr<ControlChannel>& parent, int& child_handle, std::string& error) {
    int fds[2];
    // Both ends close-on-exec at birth, so a child forked by another thread
    // in the meantime inherits neither; the spawner clears it on the child's
    // end only, in the child, by dup2 into place.
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) != 0) {
        error = os_error("socketpair");
        return false;
    }
    auto impl = std::make_unique<Impl>();
    impl->fd = fds[0];
    parent = std::make_unique<ControlChannel>(std::move(impl));
    child_handle = fds[1];
    return true;
}

std::unique_ptr<ControlChannel> ControlChannel::adopt(int handle, std::string& error) {
    int type = 0;
    socklen_t len = sizeof type;
    if (handle < 0 || ::getsockopt(handle, SOL_SOCKET, SO_TYPE, &type, &len) != 0) {
        error = os_error(("descriptor " + std::to_string(handle) + " is not a socket").c_str());
        return nullptr;
    }
    if (type != SOCK_SEQPACKET) {
        error = "descriptor " + std::to_string(handle) + " is not a SOCK_SEQPACKET socket";
        return nullptr;
    }
    // Not passed on to anything this process starts (a VST3 scan child).
    const int flags = ::fcntl(handle, F_GETFD);
    if (flags < 0 || ::fcntl(handle, F_SETFD, flags | FD_CLOEXEC) != 0) {
        error = os_error("fcntl");
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->fd = handle;
    return std::make_unique<ControlChannel>(std::move(impl));
}

void ControlChannel::close_handle(int handle) {
    if (handle >= 0) ::close(handle);
}

std::unique_ptr<ControlChannel> ControlChannel::connect(const std::string& endpoint,
                                                        std::string& error) {
    sockaddr_un addr{};
    if (!make_address(endpoint, addr, error)) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (impl->fd < 0) {
        error = os_error("socket");
        return nullptr;
    }
    if (::connect(impl->fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        error = os_error(("cannot connect to " + endpoint).c_str());
        return nullptr;
    }
    // What listens there could be anyone's if the directory was not ours;
    // an interface hands its commands to its own user's engine only.
    uid_t uid = 0;
    if (!peer_uid(impl->fd, uid, error)) return nullptr;
    if (uid != ::geteuid()) {
        error = "what listens on " + endpoint + " runs as uid " + std::to_string(uid) +
                ", not as this user; refusing it";
        return nullptr;
    }
    return std::make_unique<ControlChannel>(std::move(impl));
}

namespace {

IoStatus send_impl(int fd, const std::atomic<bool>& shut, std::string_view datagram,
                   int attach_fd, int timeout_ms, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        if (shut.load(std::memory_order_acquire)) return IoStatus::Closed;
        iovec iov{const_cast<char*>(datagram.data()), datagram.size()};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))];
        if (attach_fd >= 0) {
            std::memset(control, 0, sizeof control);
            msg.msg_control = control;
            msg.msg_controllen = sizeof control;
            cmsghdr* c = CMSG_FIRSTHDR(&msg);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(c), &attach_fd, sizeof(int));
        }
        // MSG_NOSIGNAL: a peer that went away is a Closed, not a SIGPIPE
        // that kills the engine.
        const ssize_t n = ::sendmsg(fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n >= 0) return IoStatus::Ok;
        if (errno == EINTR) continue;
        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) return IoStatus::Closed;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            error = os_error("send");
            return IoStatus::Error;
        }
        const int left = remaining_ms(deadline, timeout_ms);
        if (left == 0) return IoStatus::WouldBlock;
        const int r = wait_for(fd, POLLOUT, left);
        if (r < 0) {
            error = os_error("poll");
            return IoStatus::Error;
        }
        if (r == 0) return IoStatus::WouldBlock;
    }
}

}  // namespace

IoStatus ControlChannel::send(std::string_view datagram, int timeout_ms, std::string& error) {
    return send_impl(impl_->fd, impl_->shut, datagram, -1, timeout_ms, error);
}

IoStatus ControlChannel::send_with_region(std::string_view datagram, const SharedRegion& region,
                                          int timeout_ms, std::string& error) {
    if (region.impl().ro_fd < 0) {
        error = "only the region's creator can pass it on";
        return IoStatus::Error;
    }
    return send_impl(impl_->fd, impl_->shut, datagram, region.impl().ro_fd, timeout_ms, error);
}

IoStatus ControlChannel::receive(std::string& datagram, std::size_t max_bytes,
                                 std::unique_ptr<SharedRegion>* region, int timeout_ms,
                                 std::string& error) {
    if (impl_->shut.load(std::memory_order_acquire)) return IoStatus::Closed;
    const int r = wait_for(impl_->fd, POLLIN, timeout_ms);
    if (r < 0) {
        error = os_error("poll");
        return IoStatus::Error;
    }
    if (r == 0) return IoStatus::WouldBlock;

    datagram.resize(max_bytes);
    iovec iov{datagram.data(), max_bytes};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 4)];
    msg.msg_control = control;
    msg.msg_controllen = sizeof control;
    ssize_t n;
    do {
        n = ::recvmsg(impl_->fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);

    // Every descriptor that came is either kept as the region or closed, so
    // a peer cannot exhaust ours by sending them.
    int fd = -1;
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); n >= 0 && c != nullptr; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        const std::size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (std::size_t i = 0; i < count; ++i) {
            int got = -1;
            std::memcpy(&got, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            if (fd < 0) {
                fd = got;
            } else {
                ::close(got);
            }
        }
    }
    const auto drop_fd = [&fd]() {
        if (fd >= 0) ::close(fd);
        fd = -1;
    };

    if (n < 0) {
        drop_fd();
        if (errno == EAGAIN || errno == EWOULDBLOCK) return IoStatus::WouldBlock;
        if (errno == ECONNRESET || errno == ENOTCONN) return IoStatus::Closed;
        error = os_error("receive");
        return IoStatus::Error;
    }
    // SOCK_SEQPACKET reports the peer's close as a zero-length read; nobody
    // here sends an empty datagram.
    if (n == 0) {
        drop_fd();
        return IoStatus::Closed;
    }
    if ((msg.msg_flags & MSG_TRUNC) != 0) {
        drop_fd();
        error = "a datagram longer than " + std::to_string(max_bytes) + " bytes";
        return IoStatus::Error;
    }
    if ((msg.msg_flags & MSG_CTRUNC) != 0) {
        drop_fd();
        error = "a datagram with more descriptors than one";
        return IoStatus::Error;
    }
    datagram.resize(static_cast<std::size_t>(n));
    if (fd >= 0) {
        if (region == nullptr) {
            // Nobody here asked for one: a peer that sends a descriptor
            // anyway breaks its contract (HostControl wire 1, the control
            // API). Closed, and the datagram refused, never ignored.
            drop_fd();
            error = "a descriptor came with a datagram where none is expected";
            return IoStatus::Error;
        } else {
            *region = map_received(fd, error);
            if (!*region) return IoStatus::Error;
        }
    }
    return IoStatus::Ok;
}

void ControlChannel::shutdown() {
    if (impl_->shut.exchange(true, std::memory_order_acq_rel)) return;
    // Wakes a thread in poll() or recvmsg() on this socket; the descriptor
    // itself stays open until the destructor, so no other thread can find
    // its number reused.
    ::shutdown(impl_->fd, SHUT_RDWR);
}

// --- ControlListener --------------------------------------------------------

struct ControlListener::Impl {
    int fd = -1;
    std::string endpoint;
    ino_t inode = 0;

    ~Impl() {
        if (fd >= 0) ::close(fd);
        // Only our own socket file: a later engine may have replaced it.
        struct stat st {};
        if (!endpoint.empty() && ::stat(endpoint.c_str(), &st) == 0 && st.st_ino == inode) {
            ::unlink(endpoint.c_str());
        }
    }
};

ControlListener::ControlListener(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ControlListener::~ControlListener() = default;
const std::string& ControlListener::endpoint() const { return impl_->endpoint; }

std::unique_ptr<ControlListener> ControlListener::listen(const std::string& endpoint,
                                                         std::string& error) {
    sockaddr_un addr{};
    if (!make_address(endpoint, addr, error)) return nullptr;
    if (!make_parent_dirs(endpoint, error)) return nullptr;
    auto impl = std::make_unique<Impl>();
    impl->fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (impl->fd < 0) {
        error = os_error("socket");
        return nullptr;
    }
    if (::bind(impl->fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        if (errno != EADDRINUSE) {
            error = os_error(("cannot bind " + endpoint).c_str());
            return nullptr;
        }
        if (endpoint_live(endpoint)) {
            error = "another engine is already listening on " + endpoint;
            return nullptr;
        }
        // Left by an engine that died without removing it.
        ::unlink(endpoint.c_str());
        if (::bind(impl->fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
            error = os_error(("cannot bind " + endpoint).c_str());
            return nullptr;
        }
    }
    ::chmod(endpoint.c_str(), 0600);
    if (::listen(impl->fd, 16) != 0) {
        error = os_error("listen");
        ::unlink(endpoint.c_str());
        return nullptr;
    }
    struct stat st {};
    if (::stat(endpoint.c_str(), &st) == 0) impl->inode = st.st_ino;
    impl->endpoint = endpoint;
    return std::make_unique<ControlListener>(std::move(impl));
}

std::unique_ptr<ControlChannel> ControlListener::accept(int timeout_ms, std::string& error) {
    const int r = wait_for(impl_->fd, POLLIN, timeout_ms);
    if (r < 0) {
        error = os_error("poll");
        return nullptr;
    }
    if (r == 0) return nullptr;
    const int fd = ::accept4(impl_->fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
        // The peer gave up between poll and accept; not an error of ours.
        if (errno == EAGAIN || errno == ECONNABORTED || errno == EINTR) return nullptr;
        error = os_error("accept");
        return nullptr;
    }
    auto impl = std::make_unique<ControlChannel::Impl>();
    impl->fd = fd;
    uid_t uid = 0;
    if (!peer_uid(fd, uid, error)) return nullptr;
    if (uid != ::geteuid()) {
        error = "refused a connection from uid " + std::to_string(uid) +
                ": the engine serves its own user only";
        return nullptr;
    }
    return std::make_unique<ControlChannel>(std::move(impl));
}

}  // namespace sonora::platform
