// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/platform/wait_set.h"

#include <cerrno>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace sonora::platform {

// A self-pipe: notify() writes a byte, wait() polls the read end beside the
// caller's handles. The pipe holds what was written before the wait began,
// which is what makes a notify() unlosable without a lock around the poll.
// Without a pipe (out of descriptors at start-up) it degrades to a
// condition variable that cannot watch handles.
struct WaitSet::Impl {
    int rd = -1;
    int wr = -1;
    std::vector<pollfd> fds;

    std::mutex m;
    std::condition_variable cv;
    bool notified = false;

    Impl() {
        int p[2];
        if (::pipe(p) != 0) return;
        for (int fd : p) {
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        }
        rd = p[0];
        wr = p[1];
    }
    ~Impl() {
        if (rd >= 0) ::close(rd);
        if (wr >= 0) ::close(wr);
    }

    // Empties the pipe; true when anything was in it.
    bool drain() {
        bool any = false;
        char buf[64];
        for (;;) {
            const ssize_t n = ::read(rd, buf, sizeof buf);
            if (n > 0) {
                any = true;
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            return any;
        }
    }
};

WaitSet::WaitSet() : impl_(std::make_unique<Impl>()) {}
WaitSet::~WaitSet() = default;

bool WaitSet::watches_handles() const { return impl_->rd >= 0; }

void WaitSet::notify() {
    if (impl_->wr >= 0) {
        const char b = 1;
        // A full pipe (EAGAIN) already holds a wake-up: nothing is lost.
        while (::write(impl_->wr, &b, 1) < 0 && errno == EINTR) {
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->m);
        impl_->notified = true;
    }
    impl_->cv.notify_one();
}

WaitSet::Woke WaitSet::wait(const Watch* watches, std::size_t count, int timeout_ms) {
    Impl& s = *impl_;
    if (s.rd < 0) {
        std::unique_lock<std::mutex> lock(s.m);
        const auto pred = [&s]() { return s.notified; };
        if (timeout_ms < 0) s.cv.wait(lock, pred);
        else s.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred);
        const bool n = s.notified;
        s.notified = false;
        return n ? Woke::Notified : Woke::Timeout;
    }
    s.fds.clear();
    s.fds.push_back(pollfd{s.rd, POLLIN, 0});
    for (std::size_t i = 0; i < count; ++i) {
        if (watches[i].handle < 0) continue;
        const short events = watches[i].when == When::Writable ? POLLOUT : POLLIN;
        s.fds.push_back(pollfd{watches[i].handle, events, 0});
    }
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    int left = timeout_ms;
    for (;;) {
        const int r = ::poll(s.fds.data(), nfds_t(s.fds.size()), left);
        if (r > 0) break;
        if (r == 0) return Woke::Timeout;
        // EINVAL or ENOMEM: the handles cannot be polled this turn. The
        // caller's cadence is the timeout, so sleep it out on the pipe alone
        // rather than spin.
        if (errno != EINTR) s.fds.resize(1);
        if (timeout_ms >= 0) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
            if (ms.count() <= 0) return Woke::Timeout;
            left = int(ms.count());
        }
    }
    if (s.fds[0].revents != 0 && s.drain()) return Woke::Notified;
    for (std::size_t i = 1; i < s.fds.size(); ++i)
        if (s.fds[i].revents != 0) return Woke::Handle;
    return Woke::Timeout;
}

}  // namespace sonora::platform
