// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/platform/wake.h"

#include <climits>
#include <ctime>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__has_feature)
#if __has_feature(realtime_sanitizer)
#define SONORA_WAKE_RTSAN_SCOPE 1
// The runtime's own pair, declared here rather than through
// <sanitizer/rtsan_interface.h>: that header does not mark them noexcept, so
// calling them from a noexcept function costs a terminate landing pad
// (std::terminate, __cxa_begin_catch) that the A3 reachability test refuses.
// They are C functions of the runtime and never throw.
extern "C" void __rtsan_disable() noexcept;
extern "C" void __rtsan_enable() noexcept;
#endif
#endif

namespace sonora::platform {

// The futex word must be a plain aligned u32 (contracts/plugin asserts the
// atomic has that representation). Shared, not FUTEX_PRIVATE: the waiter
// and the waker are different processes, and the kernel keys a shared futex
// on the file and offset, so a read-only mapping and a writable one of the
// same memfd page meet on the same futex.

void wake_all(std::atomic<uint32_t>& word) noexcept SONORA_RT {
    // Result and errno ignored: there is nothing the audio thread could do
    // about a failed wake, and a host that missed it times out and looks.
#ifdef SONORA_WAKE_RTSAN_SCOPE
    // RTSan reports every syscall() from a nonblocking function (measured,
    // ADR-0012 Unverified 10, engine/plugin_client/tests/test_rtsan_wake.cpp).
    // This one is the audited exception: the rt-safety-auditor accepted one
    // shared FUTEX_WAKE per callback (A1, gated by kHostWakeEnabled) and
    // wake_all as the only common/platform symbol on the audio thread (A3),
    // with the RTSan job allowing it by a scope around this call alone.
    __rtsan_disable();
#endif
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
#ifdef SONORA_WAKE_RTSAN_SCOPE
    __rtsan_enable();
#endif
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
