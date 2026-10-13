// SPDX-License-Identifier: GPL-3.0-or-later
// Lets a neutral build link. It is not a port (ADR-0008).
#include "sonora/platform/thread_cpu.h"

namespace sonora::platform {

ThreadCpuTimes thread_cpu_times() {
    ThreadCpuTimes out;
    out.unavailable_reason = "per-thread CPU time is not available on this platform";
    return out;
}

}  // namespace sonora::platform
