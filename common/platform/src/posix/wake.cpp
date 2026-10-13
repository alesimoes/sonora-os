// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/platform/wake.h"

#include <climits>
#include <ctime>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace sonora::platform {

// The futex word must be a plain aligned u32 (contracts/plugin asserts the
// atomic has that representation). Shared, not FUTEX_PRIVATE: the waiter
// and the waker are different processes, and the kernel keys a shared futex
// on the file and offset, so a read-only mapping and a writable one of the
// same memfd page meet on the same futex.

void wake_all(std::atomic<uint32_t>& word) noexcept SONORA_RT {
    // Result and errno ignored: there is nothing the audio thread could do
    // about a failed wake, and a host that missed it times out and looks.
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

void wait_for_change(const std::atomic<uint32_t>& word, uint32_t seen, uint64_t timeout_ns) noexcept {
    timespec ts;
    ts.tv_sec = static_cast<time_t>(timeout_ns / 1000000000ull);
    ts.tv_nsec = static_cast<long>(timeout_ns % 1000000000ull);
    // FUTEX_WAIT only reads the word, so a PROT_READ mapping is enough.
    // EAGAIN (already changed), EINTR and ETIMEDOUT all mean "go and look",
    // which the caller does anyway.
    syscall(SYS_futex, const_cast<uint32_t*>(reinterpret_cast<const uint32_t*>(&word)), FUTEX_WAIT,
            static_cast<int>(seen), &ts, nullptr, 0);
}

}  // namespace sonora::platform
