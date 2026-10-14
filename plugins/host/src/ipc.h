// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The OS seam of the plugin boundary as the host and its engine-role test
// driver see it (ADR-0008: platform code at the edges, behind a neutral
// header). Only src/linux/ implements it today.
//
// The wake itself is common/platform's (wake.h: wake_all, wait_for_change),
// the one seam the engine's audio thread will use too (ADR-0012 §1.2). What
// is here is what only a host process, or a test standing in for the
// engine's control thread, needs: inherited descriptors, the sealed
// read-only clock region, scheduling, denormals, death of the parent.
//
// The regions cross as inherited descriptors (--shm-fd, --clock-fd), the
// clock sealed and reopened read-only exactly the way SharedRegion does it;
// the HostControl socket (contracts/plugin host_control.h) carries messages
// only, never a descriptor.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "sonora/rt.h"

namespace sonora::plugin_host {

// Bump `word` (release) and wake every sleeper on it. Not for the audio
// thread: the engine's audio thread bumps the clock tick and, only behind
// kHostWakeEnabled, calls platform::wake_all itself.
void signal(std::atomic<uint32_t>& word) noexcept SONORA_RT;

// Sleep while `word` still equals `seen`, at most `timeout_ns`. Returns
// early on a change, a wake or a signal; the caller always re-checks its
// condition. Works on a read-only mapping. Never for the audio thread.
void wait_for_change(const std::atomic<uint32_t>& word, uint32_t seen, uint64_t timeout_ns) noexcept;

uint64_t monotonic_ns() noexcept SONORA_RT;

// Sleep until CLOCK_MONOTONIC reaches `at_ns` (absolute, so a late wake does
// not push the next one later). The timer-paced host's wait (ADR-0012 §1.3).
void sleep_until_ns(uint64_t at_ns) noexcept;

// A shared-memory region handed from the engine to a host process.
struct SharedRegion {
    void* base = nullptr;
    std::size_t size = 0;
    int handle = -1;  // the OS handle that is passed to the host
    bool writable = false;
};

// A zeroed region of `size` bytes, mapped read-write here. Sealable.
bool create_region(std::size_t size, SharedRegion& out, std::string& error);
// The creator's half of a read-only share (auditor M1): seal the region
// against every new writable mapping (F_SEAL_FUTURE_WRITE where the kernel
// has it) and open a read-only descriptor of it into `read_only_handle`. A
// receiver can then map it PROT_READ only: PROT_WRITE fails with EACCES.
// Fails, with the reason in `error`, on a kernel without
// F_SEAL_FUTURE_WRITE: a read-only descriptor alone can be reopened
// writable through /proc, so there is no degraded mode (auditor L2).
bool share_read_only(SharedRegion& region, int& read_only_handle, std::string& error);
// The receiver's halves. map_region maps read-write (the channel region);
// map_region_read_only maps PROT_READ (the clock region).
bool map_region(int handle, SharedRegion& out, std::string& error);
bool map_region_read_only(int handle, SharedRegion& out, std::string& error);
void release_region(SharedRegion& region);
// Keep a region's pages resident (mlock). prefault faults them in, but
// without a lock (mlockall refused, a low RLIMIT_MEMLOCK) they can be
// reclaimed under pressure and fault again on the audio thread. False when
// the OS refused; the caller reports it, never silently (auditor L3).
bool lock_region(const SharedRegion& region, std::string& error);
void close_handle(int handle) noexcept;

// Process setup for the host. Each returns false if the OS refused, which
// the host reports in host_flags rather than treating as fatal: a container
// without CAP_SYS_NICE still has to run the tests.
bool lock_memory() noexcept;
bool make_current_thread_realtime(int priority) noexcept;

// A thread whose stack is `stack_bytes`, not the platform default. Under
// lock_memory every page of a new thread's stack is populated and locked, so
// the size is resident RAM, and it is written out with the core when a
// plugin crashes the host, which delays the exit the engine waits to see.
// Returns null if the OS refused. join_thread waits for it and frees it.
void* start_thread(void (*fn)(void*), void* arg, std::size_t stack_bytes) noexcept;
void join_thread(void* thread) noexcept;

// Flush-to-zero and denormals-are-zero on the calling thread (MXCSR on x86,
// FPCR.FZ on AArch64). A plugin's filter tail decaying into denormals costs
// up to two orders of magnitude per sample on x86 without them.
bool flush_denormals_on_this_thread() noexcept;

// A host that a plugin crashes must be seen gone at once, and its memory
// holds the session's audio. With a core_pattern pipe and core_pipe_limit
// set (systemd-coredump, apport), the kernel keeps the dying process until
// the helper has read and processed the whole core: CI saw a crashed host
// exit 55 to 137 blocks late. A plugin's core is not ours to collect, so
// the host is made undumpable; a person debugging one runs it under gdb.
bool no_core_dumps() noexcept;

// Ask to be killed when the engine dies, then check the engine is still the
// parent: if it died before the request was made, the request never fires.
// Returns false when the engine is already gone. `engine_pid` <= 0 skips the
// check (a person running the host by hand).
//
// Linux sends the death signal when the THREAD that forked this process
// exits, not when the engine process does. So the engine must spawn hosts
// from its long-lived control thread, and it watches its hosts with a pidfd
// (pidfd_open, polled on that thread), never with waitpid from a callback.
bool die_with_parent(int64_t engine_pid) noexcept;

// The idle loop's backstop for the case above: false once the engine that
// started us is no longer our parent.
bool parent_is(int64_t engine_pid) noexcept;

}  // namespace sonora::plugin_host
