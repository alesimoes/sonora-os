// SPDX-License-Identifier: GPL-3.0-or-later
// Lets a neutral build link. It is not a port (ADR-0008): no handle is
// watched, so a wait ends on notify() or its timeout only.
#include "sonora/platform/wait_set.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace sonora::platform {

struct WaitSet::Impl {
    std::mutex m;
    std::condition_variable cv;
    bool notified = false;
};

WaitSet::WaitSet() : impl_(std::make_unique<Impl>()) {}
WaitSet::~WaitSet() = default;

bool WaitSet::watches_handles() const { return false; }

void WaitSet::notify() {
    {
        std::lock_guard<std::mutex> lock(impl_->m);
        impl_->notified = true;
    }
    impl_->cv.notify_one();
}

WaitSet::Woke WaitSet::wait(const Watch*, std::size_t, int timeout_ms) {
    std::unique_lock<std::mutex> lock(impl_->m);
    const auto pred = [this]() { return impl_->notified; };
    if (timeout_ms < 0) impl_->cv.wait(lock, pred);
    else impl_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred);
    const bool n = impl_->notified;
    impl_->notified = false;
    return n ? Woke::Notified : Woke::Timeout;
}

}  // namespace sonora::platform
