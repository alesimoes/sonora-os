// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADR-0012 Unverified 10: does RealtimeSanitizer intercept the raw
// syscall() inside common/platform's wake_all()? Built only with
// -fsanitize=realtime (Clang 20 or later; engine/plugin_client's CMakeLists
// says when it is skipped). Run with RTSAN_OPTIONS=halt_on_error=false so a
// report does not end the run: the output is the answer.
//   - No report: RTSan does not see the wake; nothing to suppress.
//   - One report naming syscall: it does; the RTSan job then allows that
//     one call with a scoped suppression or reports it as the one known
//     syscall (ADR-0012 Consequences). Either way it must be the only one.
#include "sonora/platform/wake.h"
#include "sonora/rt.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

std::atomic<uint32_t> word{0};

// A callback in miniature: the realtime context RTSan checks starts here.
void callback() noexcept SONORA_RT {
    word.fetch_add(1, std::memory_order_release);
    sonora::platform::wake_all(word);
}

}  // namespace

int main() {
    callback();
    std::printf("wake_all returned from a nonblocking function; any RTSan report above is the answer to "
                "ADR-0012 Unverified 10\n");
    return 0;
}
