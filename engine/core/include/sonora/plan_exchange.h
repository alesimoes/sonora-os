// SPDX-License-Identifier: GPL-3.0-or-later
//
// How a playback plan reaches the audio thread and how it is retired: the
// protocol of ADR-0009 §4.1, to the memory order.
//
// One counter, blocks_, written only by the audio thread, and the plan
// pointer plan_. Every access below is seq_cst; the argument rests on it.
//
//  - Audio thread, at the start of each block: blocks_.store(n + 1), then
//    plan_.load(). A new plan is detected by its epoch, never its address.
//  - Publisher: old = plan_.exchange(next), then s = blocks_.load(), then
//    {old, s} goes on the retire list. old is freed once blocks_ != s.
//
// A block that loaded `old` stored its count k before that load, and the
// load precedes the exchange in the single total order, so k <= s. Once
// blocks_ has moved past s, every block that could hold `old` has ended.
//
// A stalled callback does not move blocks_, so nothing is freed; after
// AudioBackend::deactivate() returns, reclaim_after_deactivate() frees
// everything at once. Nothing is freed on the strength of a timeout.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "sonora/playback_plan.h"
#include "sonora/rt.h"

namespace sonora {

class PlanExchange {
public:
    // ADR-0009 §4.1: fixed, allocated with the session. When it is full the
    // publisher waits for an entry to free; it never frees early and never
    // grows the list.
    static constexpr std::size_t kRetireCapacity = 8;

    // Whether a retired plan may be freed beyond the epoch rule: the disk
    // reader may still be writing its slots. Called on the publisher's
    // thread. Null means always.
    using MayFree = bool (*)(void* context, const PlaybackPlan* plan);

    PlanExchange() = default;
    // Frees the current plan and every retired one. Only once no process()
    // can run.
    ~PlanExchange();

    PlanExchange(const PlanExchange&) = delete;
    PlanExchange& operator=(const PlanExchange&) = delete;

    // --- audio thread ---
    // At the start of every block, before anything reads the plan. The
    // result stays valid until the next call.
    const PlaybackPlan* acquire() noexcept SONORA_RT {
        const uint64_t n = blocks_.load(std::memory_order_relaxed);
        blocks_.store(n + 1, std::memory_order_seq_cst);
        return plan_.load(std::memory_order_seq_cst);
    }

    // --- publisher: one non-real-time thread ---
    // Epochs for the plans this exchange will carry, from 1; 0 is "none".
    uint64_t next_epoch() { return ++last_epoch_; }

    bool can_publish() const { return retired_count_ < kRetireCapacity; }
    // Makes `next` the plan the audio thread reads from its next block and
    // retires the previous one. Returns false, changing nothing, when the
    // retire list is full; reclaim() and try again.
    bool publish(PlaybackPlan* next);
    // Frees every retired plan whose blocks have all ended and that
    // `may_free` releases. Returns how many are still held.
    std::size_t reclaim(MayFree may_free = nullptr, void* context = nullptr);
    // After AudioBackend::deactivate() has returned, or before the first
    // activate(): no block is running or will run until the next activate,
    // so every retired plan is unreachable from the audio thread. `may_free`
    // still applies.
    std::size_t reclaim_after_deactivate(MayFree may_free = nullptr,
                                         void* context = nullptr);

    // The plan the audio thread reads now. Publisher's thread only.
    PlaybackPlan* current() const { return plan_.load(std::memory_order_relaxed); }
    std::size_t retired() const { return retired_count_; }
    uint64_t blocks() const { return blocks_.load(std::memory_order_seq_cst); }
    // Whether `plan` is on the retire list. Publisher's thread only.
    bool is_retired(const PlaybackPlan* plan) const;

private:
    struct Retired {
        PlaybackPlan* plan;
        uint64_t blocks_at_retire;
    };

    std::size_t reclaim_where(bool stalled, MayFree may_free, void* context);

    alignas(64) std::atomic<uint64_t> blocks_{0};
    alignas(64) std::atomic<PlaybackPlan*> plan_{nullptr};
    Retired retired_[kRetireCapacity] = {};
    std::size_t retired_count_ = 0;
    uint64_t last_epoch_ = 0;
};

static_assert(std::atomic<uint64_t>::is_always_lock_free, "blocks_");
static_assert(std::atomic<PlaybackPlan*>::is_always_lock_free, "plan_");

}  // namespace sonora
