// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The OS seam of the plugin boundary (ADR-0008: platform code at the edges,
// behind a neutral header). Only src/linux/ implements it today.
//
// Wake mechanism: a futex on a 32-bit word inside the shared region. The
// measurements (brain/research/adr-0007-measurements.md §3-4) found futex and
// eventfd equivalent (round trip p50 ~60 us, p99 210-245 us, container), and
// ruled out spinning at SCHED_FIFO (RT throttling stalls of ~50 ms). Futex
// wins on shape, not speed: the word lives in the region the engine already
// shares, so there is no second descriptor to create, pass and close per
// host. An eventfd implementation fits behind the same three functions.
//
// When engine/plugin_client lands (M4+), the engine's half of this seam
// (signal) moves to common/platform, where ADR-0008 §3 already reserves "the
// Phase 4 plugin wake (SONORA_RT, auditor-gated)". It lives here until then
// because plugins/host and its test driver are its only users.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace sonora::plugin_host {

// Bump `word` (release) and wake every sleeper on it. One syscall; no lock,
// no allocation. This is the call whose use on the audio thread needs the
// rt-safety-auditor's ruling (ADR-0007 update 1, item 4).
void signal(std::atomic<uint32_t>& word) noexcept;

// Sleep while `word` still equals `seen`, at most `timeout_ns`. Returns
// early on a change, a wake or a signal; the caller always re-checks its
// condition. Never for the audio thread.
void wait_for_change(std::atomic<uint32_t>& word, uint32_t seen, uint64_t timeout_ns) noexcept;

uint64_t monotonic_ns() noexcept;

// A shared-memory region handed from the engine to a host process.
struct SharedRegion {
    void* base = nullptr;
    std::size_t size = 0;
    int handle = -1;  // the OS handle that is passed to the host
};

bool create_region(std::size_t size, SharedRegion& out, std::string& error);
bool map_region(int handle, SharedRegion& out, std::string& error);
void release_region(SharedRegion& region);

// Process setup for the host. Each returns false if the OS refused, which
// the host reports in host_flags rather than treating as fatal: a container
// without CAP_SYS_NICE still has to run the tests.
bool lock_memory() noexcept;
bool make_current_thread_realtime(int priority) noexcept;
void die_with_parent() noexcept;

}  // namespace sonora::plugin_host
