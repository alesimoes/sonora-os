// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/memory_lock.h"

#include <sys/mman.h>
#include <sys/resource.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace sonora {

bool lock_memory(std::string& why) {
    rlimit lim{};
    if (::getrlimit(RLIMIT_MEMLOCK, &lim) != 0) {
        why = std::string("getrlimit(RLIMIT_MEMLOCK): ") + std::strerror(errno);
        return false;
    }
    if (lim.rlim_cur != RLIM_INFINITY) {
        // An unprivileged process may raise its soft limit to the hard one;
        // root may raise both.
        const rlimit unlimited{RLIM_INFINITY, RLIM_INFINITY};
        if (::setrlimit(RLIMIT_MEMLOCK, &unlimited) != 0) {
            char text[160];
            std::snprintf(text, sizeof text,
                          "RLIMIT_MEMLOCK is %llu bytes, not unlimited (the"
                          " service sets LimitMEMLOCK=infinity; elsewhere,"
                          " ulimit -l unlimited)",
                          static_cast<unsigned long long>(lim.rlim_cur));
            why = text;
            return false;
        }
    }
    if (::mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        why = std::string("mlockall: ") + std::strerror(errno);
        return false;
    }
    return true;
}

long locked_kb() {
    std::FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    long kb = -1;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        if (std::sscanf(line, "VmLck: %ld kB", &kb) == 1) break;
    }
    std::fclose(f);
    return kb;
}

}  // namespace sonora
