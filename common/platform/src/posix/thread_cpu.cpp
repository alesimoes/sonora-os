// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/platform/thread_cpu.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace sonora::platform {

// /proc/self/task/<tid>/stat is Linux procfs, the only POSIX system we ship
// on. A POSIX system without it reports unavailable rather than zeros.
ThreadCpuTimes thread_cpu_times() {
    ThreadCpuTimes out;
    const long tick = sysconf(_SC_CLK_TCK);
    std::error_code ec;
    std::filesystem::directory_iterator dir("/proc/self/task", ec);
    if (ec || tick <= 0) {
        out.unavailable_reason = "/proc/self/task not readable";
        return out;
    }
    out.available = true;
    out.resolution_ns = 1'000'000'000 / tick;
    for (const auto& entry : dir) {
        std::ifstream f(entry.path() / "stat");
        if (!f) continue;  // the thread exited between listing and reading
        const std::string line((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
        // The name is parenthesised and may itself contain spaces or ')'.
        const auto open = line.find('(');
        const auto close = line.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close < open) continue;
        std::istringstream rest(line.substr(close + 1));
        std::string field;
        int64_t utime = -1, stime = -1;
        // After the name: state is field 1, utime field 12, stime field 13.
        for (int i = 1; i <= 13 && rest >> field; ++i) {
            if (i == 12) utime = std::stoll(field);
            if (i == 13) stime = std::stoll(field);
        }
        if (utime < 0 || stime < 0) continue;
        out.threads.push_back({line.substr(open + 1, close - open - 1),
                               (utime + stime) * out.resolution_ns});
    }
    return out;
}

}  // namespace sonora::platform
