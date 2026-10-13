// SPDX-License-Identifier: GPL-3.0-or-later
//
// CPU time per thread of the calling process: a diagnostic, for reports such
// as sonora-ui --measure. Not for the real-time path: it allocates and reads
// the filesystem.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sonora::platform {

struct ThreadCpu {
    std::string name;    // as the OS names it, e.g. "llvmpipe-3"
    int64_t cpu_ns = 0;  // user + system, since the thread started
};

struct ThreadCpuTimes {
    // False where the platform has no implementation. Callers print
    // "unavailable" instead of figures: zeros would read as "free".
    bool available = false;
    std::string unavailable_reason;
    int64_t resolution_ns = 0;  // granularity of cpu_ns
    std::vector<ThreadCpu> threads;
};

ThreadCpuTimes thread_cpu_times();

}  // namespace sonora::platform
