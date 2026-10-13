// SPDX-License-Identifier: GPL-3.0-or-later
// No cross-process wake on this platform yet (Windows: a named event, with
// the ASIO port, ADR-0008). Callers re-check their condition, so a sleep for
// the timeout is slow but correct.
#include "sonora/platform/wake.h"

#include <chrono>
#include <thread>

namespace sonora::platform {

void wake_all(std::atomic<uint32_t>&) noexcept SONORA_RT {}

void wait_for_change(const std::atomic<uint32_t>& word, uint32_t seen, uint64_t timeout_ns) noexcept {
    if (word.load(std::memory_order_acquire) != seen) return;
    std::this_thread::sleep_for(std::chrono::nanoseconds(timeout_ns));
}

}  // namespace sonora::platform
