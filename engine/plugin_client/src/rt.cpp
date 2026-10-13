// SPDX-License-Identifier: GPL-3.0-or-later
//
// The audio thread's half of PluginClient: ADR-0012 §7.2 on contracts/plugin
// layout 1. Only what runs in the callback lives in this file, so that
// tests/rt_reachability.cmake can check this one object for allocation,
// locks, throws, I/O and any common/platform symbol but wake_all().
#include "tables.h"

#include "sonora/platform/wake.h"

#include <cstdint>
#include <cstring>

namespace sonora::plugin_client {

namespace {

void silence(float* const* out, uint32_t channels, uint32_t frames) noexcept SONORA_RT {
    for (uint32_t c = 0; c < channels; ++c) std::memset(out[c], 0, std::size_t(frames) * sizeof(float));
}

// ADR-0012 §3 and auditor L4: on the first missed block, a linear decay from
// the last sample played to 0 over kFadeFrames, then silence. The position
// carries over blocks, so a quantum under 64 continues the same ramp.
void fade_out(PluginClient::Node& n, float* const* out, uint32_t channels, uint32_t frames) noexcept SONORA_RT {
    if (n.playing) {
        for (uint32_t c = 0; c < channels; ++c) n.fade_from[c] = n.last[c];
        n.fade_pos = 0;
        n.playing = false;
    }
    for (uint32_t c = 0; c < channels; ++c) {
        float* o = out[c];
        const float from = n.fade_from[c];
        for (uint32_t i = 0; i < frames; ++i) {
            const uint32_t k = n.fade_pos + i + 1;
            o[i] = k < kFadeFrames ? from * (float(kFadeFrames - k) / float(kFadeFrames)) : 0.f;
        }
        n.last[c] = o[frames - 1];
    }
    const uint32_t pos = n.fade_pos + frames;
    n.fade_pos = pos < kFadeFrames ? pos : kFadeFrames;
}

// The first block collected after a miss (or a channel's first) comes in
// linearly from 0, so recovery is not a step (§3).
void ramp_in(float* const* out, uint32_t channels, uint32_t frames) noexcept SONORA_RT {
    for (uint32_t c = 0; c < channels; ++c)
        for (uint32_t i = 0; i < frames; ++i) out[c][i] *= float(i) / float(frames);
}

// Into the authoritative store first, whatever happens to the channel this
// callback: a held change reaches the host from here (hold_param, M2).
void keep(PluginClient::Store& s, const ParamEvent& e) noexcept SONORA_RT {
    if (e.param == plugin::kBypassParam) {
        if (e.instance < plugin::kMaxInstances) s.bypass[e.instance] = e.value;
        return;
    }
    if (const int64_t slot = plugin::param_slot(s.map, e.instance, e.param); slot >= 0)
        s.values[slot] = e.value;
}

}  // namespace

void PluginClient::begin_callback(uint64_t now_ns) noexcept SONORA_RT {
    // Flag first, then load: with detach_clock's store-then-check, both
    // sequentially consistent, a callback either sees null or is seen
    // inside, so the clock is never unmapped under it.
    in_callback_.store(1, std::memory_order_seq_cst);
    cb_clock_ = clock_.load(std::memory_order_seq_cst);
    ++callback_;
    callbacks_seen_.store(callback_, std::memory_order_relaxed);
    budget_left_ = plugin::kValueCopyBudgetBytes;
    submitted_any_ = false;
    if (cb_clock_) plugin::publish_callback_start(*cb_clock_, now_ns);
}

void PluginClient::end_callback() noexcept SONORA_RT {
    if (submitted_any_ && cb_clock_) {
        plugin::publish_tick(*cb_clock_);
        // Gated off until ADR-0012 Unverified 1 passes on bare metal (A1).
        if (plugin::kHostWakeEnabled) platform::wake_all(cb_clock_->tick);
    }
    cb_clock_ = nullptr;
    in_callback_.store(0, std::memory_order_seq_cst);
}

Outcome PluginClient::process_chain(const InsertChainRef& ref, const float* const* in, float* const* out,
                                    uint32_t frames, const ParamEvent* events,
                                    uint32_t event_count) noexcept SONORA_RT {
    if (frames == 0) return Outcome::Missed;
    if (ref.node_state >= config_.max_nodes ||
        (ref.param_store >= config_.max_stores && ref.param_store != kNoStore) || ref.channels == 0 ||
        ref.channels > config_.max_chain_channels) {
        // Nothing of the node's can be trusted to index with; the plan
        // builder never produces this, so it is counted and nothing else.
        bad_refs_.store(bad_refs_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        silence(out, ref.channels, frames);
        return Outcome::Missed;
    }
    Node& node = nodes_[ref.node_state];
    // Once per node per callback (code-reviewer M2 on PR #49): a caller
    // that runs the plan per slice of a split block would otherwise see its
    // own submit as a busy host and remove the chain after 8 slices.
    if (node.processed_callback == callback_) {
        node.repeat_calls.bump();
        if (event_count) node.events_dropped.bump(event_count);
        silence(out, ref.channels, frames);
        return Outcome::Missed;
    }
    node.processed_callback = callback_;

    // In place (in[c] == out[d]): the collect below writes `out` before
    // the submit reads `in`, so the input is copied aside first.
    if (frames <= config_.block_frames) {
        bool aliased = false;
        for (uint32_t c = 0; c < ref.channels && !aliased; ++c)
            for (uint32_t d = 0; d < ref.channels && !aliased; ++d) {
                const auto i0 = reinterpret_cast<std::uintptr_t>(in[c]);
                const auto o0 = reinterpret_cast<std::uintptr_t>(out[d]);
                const std::uintptr_t bytes = std::uintptr_t(frames) * sizeof(float);
                aliased = in[c] && i0 < o0 + bytes && o0 < i0 + bytes;
            }
        if (aliased) {
            for (uint32_t c = 0; c < ref.channels; ++c) {
                float* copy = alias_scratch_.data() + std::size_t(c) * config_.block_frames;
                if (in[c]) std::memcpy(copy, in[c], std::size_t(frames) * sizeof(float));
                else std::memset(copy, 0, std::size_t(frames) * sizeof(float));
                alias_in_[c] = copy;
            }
            in = alias_in_.data();
        }
    }

    Store& store = ref.param_store == kNoStore ? *empty_store_ : stores_[ref.param_store];
    if (ref.param_store == kNoStore) {
        if (event_count) node.events_dropped.bump(event_count);
        event_count = 0;
    }
    for (uint32_t i = 0; i < event_count; ++i) keep(store, events[i]);

    // Step 0 (H1): a slot reassigned to another host is never read or
    // written; this is a miss and a fault against the node.
    if (ref.channel_slot >= config_.max_channels ||
        !plugin::slot_current(slots_[ref.channel_slot].generation, ref.channel_generation)) {
        node.generation_faults.bump();
        node.silent_blocks.bump();
        fade_out(node, out, ref.channels, frames);
        return Outcome::Missed;
    }
    Slot& s = slots_[ref.channel_slot];
    if (s.layout.channels != ref.channels || s.layout.param_slots != store.map.param_slots ||
        frames > s.layout.max_frames) {
        node.shape_faults.bump();
        node.silent_blocks.bump();
        fade_out(node, out, ref.channels, frames);
        return Outcome::Missed;
    }

    // A node now on another channel than last callback (a plan swap to a
    // new host): the old channel's in-flight block is discarded, and the
    // silence this costs is the swap's, not a miss of the new host (M5).
    const bool switched = node.bound_slot != ref.channel_slot || node.bound_generation != ref.channel_generation;
    node.bound_slot = ref.channel_slot;
    node.bound_generation = ref.channel_generation;
    const LastSubmit last = s.last;
    s.last = LastSubmit::None;

    if (s.removed.load(std::memory_order_relaxed)) {
        node.silent_blocks.bump();
        fade_out(node, out, ref.channels, frames);
        return Outcome::Removed;
    }
    // Contracts/plugin, "Frame count": every block is block_frames in v1.
    if (frames != s.eng.block_frames) {
        s.frame_mismatches.bump();
        if (event_count) plugin::hold_param(s.eng);
        node.silent_blocks.bump();
        fade_out(node, out, ref.channels, frames);
        return Outcome::Missed;
    }

    plugin::ChannelHeader& h = *s.header;
    const bool idle = plugin::channel_idle(h, s.eng);

    // Step 1: the block submitted in the previous callback, if it is
    // complete. A block completed any later is discarded, never played late.
    uint32_t bad = 0;
    bool got = false;
    if (idle && last == LastSubmit::Submitted && s.submitted_callback + 1 == callback_) {
        for (uint32_t c = 0; c < ref.channels; ++c)
            bad += plugin::collect_output(out[c], plugin::output_plane(s.region, s.layout, c), frames);
        got = bad == 0;
    }
    Outcome outcome = Outcome::Collected;
    if (got) {
        if (!node.playing) ramp_in(out, ref.channels, frames);
        node.playing = true;
        for (uint32_t c = 0; c < ref.channels; ++c) node.last[c] = out[c][frames - 1];
        s.collected.bump();
        s.streak_count = 0;
        s.streak.store(0, std::memory_order_relaxed);
    } else {
        outcome = Outcome::Missed;
        if (bad) {
            // The zeroed copy is never played (rt-engineer M4).
            s.nonfinite_samples.bump(bad);
            s.nonfinite_blocks.bump();
        }
        node.silent_blocks.bump();
        fade_out(node, out, ref.channels, frames);
        // The host's miss when it was given a block it has not returned in
        // time, or kept the previous one so long nothing could be given.
        // Not a channel's first callbacks, nor a swap's silent block.
        const bool counted = !switched && (bad || last == LastSubmit::Submitted || last == LastSubmit::Busy);
        if (counted) {
            s.misses.bump();
            ++s.streak_count;
            s.streak.store(s.streak_count, std::memory_order_relaxed);
            if (s.streak_count >= kMaxConsecutiveMisses) {
                // The control thread names the culprit from what it
                // sampled during the streak, kills the host and applies
                // the restart policy (§5.3); nothing more is submitted.
                s.removed.store(1, std::memory_order_release);
                return Outcome::Removed;
            }
        }
    }

    // Step 2: never write a slot the host may be reading.
    if (!idle) {
        s.busy_skips.bump();
        if (event_count) plugin::hold_param(s.eng);
        s.last = LastSubmit::Busy;
        return outcome;
    }
    if (s.eng.resync) {
        // A2.2: the store copies of one callback share one budget. A
        // channel's first submit waits for its copy (it plays silence
        // either way); a held change's copy waits and the submit goes on.
        const std::size_t bytes = plugin::value_copy_bytes(s.layout);
        if (bytes <= budget_left_) {
            budget_left_ -= bytes;
            plugin::store_values(h, s.eng, s.table, s.layout, store.values, store.bypass, s.map.instance_count);
        } else if (s.eng.block == 0) {
            s.budget_waits.bump();
            return outcome;
        } else {
            s.resync_deferred.bump();
        }
    }
    for (uint32_t c = 0; c < ref.channels; ++c) {
        float* dst = plugin::input_plane(s.region, s.layout, c);
        if (in[c]) std::memcpy(dst, in[c], std::size_t(frames) * sizeof(float));
        else std::memset(dst, 0, std::size_t(frames) * sizeof(float));
    }
    const uint64_t tag = plugin::next_block(s.eng);
    for (uint32_t i = 0; i < event_count; ++i) {
        const ParamEvent& e = events[i];
        const plugin::ParamChange p{tag, e.instance, e.param, e.offset < frames ? e.offset : frames - 1, e.value};
        if (!plugin::send_param(h, s.eng, s.table, s.map, p)) s.params_refused.bump();
    }
    plugin::submit_block(h, s.eng, frames);
    s.submitted_callback = callback_;
    s.last = LastSubmit::Submitted;
    s.submitted.bump();
    submitted_any_ = true;
    return outcome;
}

}  // namespace sonora::plugin_client
