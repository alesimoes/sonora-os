// SPDX-License-Identifier: GPL-3.0-or-later
// Linux implementation of the host's side of the plugin boundary: memfd
// regions, the sealed read-only clock share, scheduling. The futex wake is
// common/platform's (wake.h).
#include "ipc.h"

#include "sonora/platform/wake.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>
#include <new>

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

// glibc before 2.32 lacks the name; the kernel has had it since 5.1.
#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif

namespace sonora::plugin_host {

void signal(std::atomic<uint32_t>& word) noexcept {
    word.fetch_add(1, std::memory_order_release);
    platform::wake_all(word);
}

void wait_for_change(const std::atomic<uint32_t>& word, uint32_t seen, uint64_t timeout_ns) noexcept {
    platform::wait_for_change(word, seen, timeout_ns);
}

uint64_t monotonic_ns() noexcept {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

void sleep_until_ns(uint64_t at_ns) noexcept {
    const timespec ts{static_cast<time_t>(at_ns / 1000000000ull), static_cast<long>(at_ns % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
}

bool create_region(std::size_t size, SharedRegion& out, std::string& error) {
    const int fd = static_cast<int>(
        syscall(SYS_memfd_create, "sonora-plugin-channel", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd < 0) {
        error = std::string("memfd_create: ") + std::strerror(errno);
        return false;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        error = std::string("ftruncate: ") + std::strerror(errno);
        close(fd);
        return false;
    }
    void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        error = std::string("mmap: ") + std::strerror(errno);
        close(fd);
        return false;
    }
    out.base = base;
    out.size = size;
    out.handle = fd;
    out.writable = true;
    return true;
}

// The same steps as common/platform's SharedRegion::create (posix/ipc.cpp):
// seal after our own writable mapping, then reopen read-only.
bool share_read_only(SharedRegion& region, int& read_only_handle, std::string& error) {
    if (fcntl(region.handle, F_ADD_SEALS,
              F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL) != 0) {
        // Refused, not degraded (auditor L2 on PR #45). Without the seal a
        // host can reopen /proc/<pid>/fd/N read-write (a memfd inode is
        // mode 0777) and map the clock writable, which is the forgery M1
        // exists to stop. A read-only descriptor alone does not stop it.
        error = std::string("cannot seal the region against writable mappings (F_SEAL_FUTURE_WRITE, "
                            "Linux 5.1 or later): ") +
                std::strerror(errno) + "; a host could map the clock writable, so it is not shared";
        return false;
    }
    const std::string self = "/proc/self/fd/" + std::to_string(region.handle);
    read_only_handle = open(self.c_str(), O_RDONLY | O_CLOEXEC);
    if (read_only_handle < 0) {
        error = std::string("cannot open a read-only descriptor of the region: ") + std::strerror(errno);
        return false;
    }
    return true;
}

namespace {

bool map_with(int handle, int prot, SharedRegion& out, std::string& error) {
    struct stat st;
    if (fstat(handle, &st) != 0) {
        error = std::string("fstat on the region descriptor: ") + std::strerror(errno);
        return false;
    }
    const std::size_t size = static_cast<std::size_t>(st.st_size);
    if (size == 0) {
        error = "the region is empty";
        return false;
    }
    void* base = mmap(nullptr, size, prot, MAP_SHARED, handle, 0);
    if (base == MAP_FAILED) {
        error = std::string("mmap: ") + std::strerror(errno);
        return false;
    }
    out.base = base;
    out.size = size;
    out.handle = handle;
    out.writable = (prot & PROT_WRITE) != 0;
    return true;
}

}  // namespace

bool map_region(int handle, SharedRegion& out, std::string& error) {
    return map_with(handle, PROT_READ | PROT_WRITE, out, error);
}

bool map_region_read_only(int handle, SharedRegion& out, std::string& error) {
    return map_with(handle, PROT_READ, out, error);
}

void release_region(SharedRegion& region) {
    if (region.base) munmap(region.base, region.size);
    if (region.handle >= 0) close(region.handle);
    region = SharedRegion{};
}

bool lock_region(const SharedRegion& region, std::string& error) {
    if (!region.base || region.size == 0) return true;
    if (mlock(region.base, region.size) == 0) return true;
    error = std::string("mlock of ") + std::to_string(region.size) + " bytes: " + std::strerror(errno);
    return false;
}

void close_handle(int handle) noexcept {
    if (handle >= 0) close(handle);
}


bool lock_memory() noexcept {
    return mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
}

void* start_thread(void (*fn)(void*), void* arg, std::size_t stack_bytes) noexcept {
    struct Start {
        void (*fn)(void*);
        void* arg;
    };
    auto* t = new (std::nothrow) pthread_t;
    auto* s = new (std::nothrow) Start{fn, arg};
    pthread_attr_t attr;
    bool ok = t && s && pthread_attr_init(&attr) == 0;
    if (ok) {
        ok = pthread_attr_setstacksize(&attr, std::max<std::size_t>(stack_bytes, PTHREAD_STACK_MIN)) == 0 &&
             pthread_create(
                 t, &attr,
                 [](void* p) -> void* {
                     const Start start = *static_cast<Start*>(p);
                     delete static_cast<Start*>(p);
                     start.fn(start.arg);
                     return nullptr;
                 },
                 s) == 0;
        pthread_attr_destroy(&attr);
    }
    if (!ok) {
        delete t;
        delete s;
        return nullptr;
    }
    return t;
}

void join_thread(void* thread) noexcept {
    if (!thread) return;
    auto* t = static_cast<pthread_t*>(thread);
    pthread_join(*t, nullptr);
    delete t;
}

bool make_current_thread_realtime(int priority) noexcept {
    if (priority <= 0) return false;
    sched_param p{};
    p.sched_priority = priority;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) == 0;
}

bool flush_denormals_on_this_thread() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    // FTZ is bit 15, DAZ bit 6.
    _mm_setcsr(_mm_getcsr() | 0x8040u);
    return (_mm_getcsr() & 0x8040u) == 0x8040u;
#elif defined(__aarch64__)
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= uint64_t(1) << 24;  // FZ flushes denormal inputs and outputs alike
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
    return true;
#else
    return false;
#endif
}

bool no_core_dumps() noexcept {
    // Not RLIMIT_CORE: a core_pattern pipe ignores a zero limit. A process
    // that is not dumpable is skipped by the kernel before any helper runs.
    return prctl(PR_SET_DUMPABLE, 0) == 0;
}

bool die_with_parent(int64_t engine_pid) noexcept {
    // A host outliving its engine would hold the plugins' resources and the
    // region forever. The engine is the only parent a host ever has.
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    // If the engine died between fork and prctl, this process was reparented
    // and the death signal will never come.
    return parent_is(engine_pid);
}

bool parent_is(int64_t engine_pid) noexcept {
    return engine_pid <= 0 || static_cast<int64_t>(getppid()) == engine_pid;
}

}  // namespace sonora::plugin_host
