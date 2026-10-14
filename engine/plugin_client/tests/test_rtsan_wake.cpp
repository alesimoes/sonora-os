// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0012 Unverified 10: does RealtimeSanitizer intercept the raw
// syscall() inside common/platform's wake_all()? Built only with
// -fsanitize=realtime (Clang 20 or later; engine/plugin_client's CMakeLists
// says when it is skipped).
//
// Settled 2026-10-13 with clang-22: it does. An unscoped syscall(SYS_futex)
// from a nonblocking function is reported as unsafe-library-call `syscall`
// (the --raw-syscall canary below, which ctest expects to be reported).
// wake_all() therefore runs its one syscall with RTSan disabled for that
// call only (common/platform/src/posix/wake.cpp, the auditor's A1/A3), and
// the plain run must exit 0 under halt_on_error=true: no report at all.
#include "sonora/platform/wake.h"
#include "sonora/rt.h"

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

std::atomic<uint32_t> word{0};

// A callback in miniature: the realtime context RTSan checks starts here.
void callback() noexcept SONORA_RT {
    word.fetch_add(1, std::memory_order_release);
    sonora::platform::wake_all(word);
}

// The same futex wake without wake_all's scope: proves RTSan sees a raw
// syscall at all, so the clean run above is the scope working, not RTSan
// missing it.
void raw_wake() noexcept SONORA_RT {
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--raw-syscall") == 0) {
        raw_wake();
        std::printf("a raw syscall from a nonblocking function was not reported\n");
        return 0;
    }
    callback();
    // Twice: the scope must have re-enabled RTSan, not left it off.
    callback();
    std::printf("wake_all ran twice from a nonblocking function with no RTSan report\n");
    return 0;
}
