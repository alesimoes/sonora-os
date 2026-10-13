// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine locks its memory before the session exists. A page fault on the
// audio thread is a disk read, and an evicted capture ring takes one on the
// first block after it was paged out: an xrun nothing in the code explains.
// sonora-engine never called mlockall, so `memlock unlimited` in the image
// granted something nobody asked for.
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sonora/memory_lock.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

// With a finite limit, MCL_FUTURE turns the next mapping past it into a
// failed allocation, so lock_memory() refuses and says which limit. Run in
// a child, because the lowered hard limit cannot be raised again.
int test_finite_limit_is_refused() {
    const pid_t pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        // Root could raise the limit straight back.
        if (::geteuid() == 0 && ::setuid(65534) != 0) ::_exit(10);
        const rlimit small{64 * 1024, 64 * 1024};
        if (::setrlimit(RLIMIT_MEMLOCK, &small) != 0) ::_exit(11);
        std::string why;
        if (sonora::lock_memory(why)) ::_exit(12);
        if (why.find("RLIMIT_MEMLOCK") == std::string::npos) ::_exit(13);
        if (sonora::locked_kb() != 0) ::_exit(14);
        ::_exit(0);
    }
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    if (WEXITSTATUS(status) != 0) {
        std::fprintf(stderr, "FAIL: refusal child exited %d\n", WEXITSTATUS(status));
        return 1;
    }
    return 0;
}

int test_locks_now_and_later() {
    rlimit lim{};
    CHECK(::getrlimit(RLIMIT_MEMLOCK, &lim) == 0);
    const rlimit unlimited{RLIM_INFINITY, RLIM_INFINITY};
    if (lim.rlim_max != RLIM_INFINITY &&
        ::setrlimit(RLIMIT_MEMLOCK, &unlimited) != 0) {
        // Nothing this process may do can lock all of itself here; the
        // refusal above is what this environment can show.
        std::printf("test_memory_lock: success path skipped: hard"
                    " RLIMIT_MEMLOCK is %llu bytes and cannot be raised\n",
                    static_cast<unsigned long long>(lim.rlim_max));
        return 0;
    }
    std::string why;
    if (!sonora::lock_memory(why)) {
        std::fprintf(stderr, "FAIL: lock_memory: %s\n", why.c_str());
        return 1;
    }
    const long before = sonora::locked_kb();
    CHECK(before > 0);

    // MCL_FUTURE: what is allocated afterwards is locked too, as the
    // session's rings are.
    std::vector<char> ring(8 * 1024 * 1024);
    std::memset(ring.data(), 1, ring.size());
    const long after = sonora::locked_kb();
    CHECK(after - before >= 8 * 1024 * 9 / 10);
    return 0;
}

}  // namespace

int main() {
    if (test_finite_limit_is_refused() != 0) return 1;
    if (test_locks_now_and_later() != 0) return 1;
    std::printf("test_memory_lock: ok\n");
    return 0;
}
