// SPDX-License-Identifier: GPL-3.0-or-later
//
// Waking processes that sleep on a 32-bit word in shared memory (ADR-0012
// §1.2): the engine wakes plugin hosts sleeping on the clock region's tick,
// and the control thread waits for a host's done_word. POSIX (Linux): a
// shared futex on the word. Windows, later: a named event behind the same
// two calls. Elsewhere wake_all does nothing and wait_for_change sleeps out
// its timeout, which callers survive because they re-check their condition.
//
// wake_all() is the ONLY symbol of common/platform the audio thread may
// reach (ADR-0012 Consequences, auditor A3): noexcept, one raw syscall, no
// logging, no allocation, nothing on its failure path, errno ignored. It is
// called only behind sonora::plugin::kHostWakeEnabled, which is false until
// the bare-metal measurement of ADR-0012 Unverified 1 passes. Do not add a
// second SONORA_RT symbol here; a reachability test will hold that
// (ADR-0012 checklist 4).
#pragma once

#include "sonora/rt.h"

#include <atomic>
#include <cstdint>

namespace sonora::platform {

// Wakes every process sleeping on `word` in wait_for_change. Does not change
// the word: the caller bumps it first (release) when there is news. One
// syscall whatever the number of sleepers; on PREEMPT_RT it can sleep on a
// contended futex hash-bucket lock (ADR-0012 §1.1), which is why it is gated.
void wake_all(std::atomic<uint32_t>& word) noexcept SONORA_RT;

// Sleeps while `word` still holds `seen`, at most `timeout_ns`. Returns early
// on a change, a wake or a signal; the caller always re-checks. Works on a
// read-only mapping of the word (a sealed memfd mapped PROT_READ), which is
// how hosts see the clock region (ADR-0012 Unverified 2,
// tests/test_ipc.cpp). Never for the audio thread.
void wait_for_change(const std::atomic<uint32_t>& word, uint32_t seen, uint64_t timeout_ns) noexcept;

}  // namespace sonora::platform
