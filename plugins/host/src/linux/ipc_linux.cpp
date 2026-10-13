// SPDX-License-Identifier: GPL-3.0-or-later
// Linux implementation of the plugin boundary seam: futex wake, memfd region.
#include "ipc.h"

#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>

#include <linux/futex.h>
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

namespace sonora::plugin_host {

namespace {

// The futex word must be a plain aligned u32; protocol.h asserts the atomic
// has that representation. Shared (not FUTEX_PRIVATE) because the waiter and
// the waker are different processes.
int* futex_addr(std::atomic<uint32_t>& word) {
    return reinterpret_cast<int*>(&word);
}

}  // namespace

void signal(std::atomic<uint32_t>& word) noexcept {
    word.fetch_add(1, std::memory_order_release);
    syscall(SYS_futex, futex_addr(word), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
}

void wait_for_change(std::atomic<uint32_t>& word, uint32_t seen, uint64_t timeout_ns) noexcept {
    timespec ts;
    ts.tv_sec = static_cast<time_t>(timeout_ns / 1000000000ull);
    ts.tv_nsec = static_cast<long>(timeout_ns % 1000000000ull);
    // Relative timeout. EAGAIN (word already changed), EINTR and ETIMEDOUT
    // all mean "go and look", which the caller does anyway.
    syscall(SYS_futex, futex_addr(word), FUTEX_WAIT, static_cast<int>(seen), &ts, nullptr, 0);
}

uint64_t monotonic_ns() noexcept {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

bool create_region(std::size_t size, SharedRegion& out, std::string& error) {
    const int fd = static_cast<int>(syscall(SYS_memfd_create, "sonora-plugin-channel", MFD_CLOEXEC));
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
    return true;
}

bool map_region(int handle, SharedRegion& out, std::string& error) {
    struct stat st;
    if (fstat(handle, &st) != 0) {
        error = std::string("fstat on the channel descriptor: ") + std::strerror(errno);
        return false;
    }
    const std::size_t size = static_cast<std::size_t>(st.st_size);
    if (size == 0) {
        error = "the channel region is empty";
        return false;
    }
    void* base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);
    if (base == MAP_FAILED) {
        error = std::string("mmap: ") + std::strerror(errno);
        return false;
    }
    out.base = base;
    out.size = size;
    out.handle = handle;
    return true;
}

void release_region(SharedRegion& region) {
    if (region.base) munmap(region.base, region.size);
    if (region.handle >= 0) close(region.handle);
    region = SharedRegion{};
}

bool lock_memory() noexcept {
    return mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
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
