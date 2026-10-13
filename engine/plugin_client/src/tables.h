// SPDX-License-Identifier: GPL-3.0-or-later
// The three tables of PluginClient (plugin_client.h), shared by the audio
// thread's half (rt.cpp) and the control thread's (client.cpp).
#pragma once

#include "sonora/plugin_client/plugin_client.h"

namespace sonora::plugin_client {

// Why the previous callback did not submit to a slot, which decides whether
// this callback's miss is the host's (ADR-0012 §3: one late host costs at
// least two silent blocks, both counted).
enum class LastSubmit : uint8_t {
    None,       // nothing submitted, for a reason that is not the host's
    Submitted,
    Busy,       // the host was still on an older block
};

// A counter the audio thread alone writes and the control thread reads.
struct RtCounter {
    std::atomic<uint64_t> value{0};
    void bump(uint64_t n = 1) noexcept { value.store(value.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); }
    uint64_t get() const noexcept { return value.load(std::memory_order_relaxed); }
    void reset() noexcept { value.store(0, std::memory_order_relaxed); }
};

struct PluginClient::Slot {
    plugin::SlotGeneration generation;

    // --- payload: written by the control thread before assign()'s release
    // store, read by the audio thread after slot_current()'s acquire.
    void* region = nullptr;
    plugin::ChannelHeader* header = nullptr;
    plugin::Layout layout;
    plugin::ParamMap map;
    plugin::EngineChannel eng;
    std::atomic<float>* table = nullptr;
    uint32_t latency_frames = 0;

    // --- the audio thread's, reset by assign().
    uint64_t submitted_callback = 0;
    LastSubmit last = LastSubmit::None;
    uint32_t streak_count = 0;
    uint64_t streak_by_instance[plugin::kMaxInstances] = {};

    // --- written by the audio thread, read by the control thread.
    std::atomic<uint32_t> removed{0};
    std::atomic<int32_t> culprit{-1};
    std::atomic<uint32_t> streak{0};
    RtCounter submitted, collected, misses, busy_skips, nonfinite_samples, nonfinite_blocks,
        params_refused, budget_waits, resync_deferred, frame_mismatches;
    RtCounter instance_misses[plugin::kMaxInstances];

    bool in_use = false;  // control thread only
};

struct PluginClient::Store {
    plugin::ParamMap map;
    float* values = nullptr;  // config.store_slots entries; map.param_slots used
    float bypass[plugin::kMaxInstances] = {};
};

struct PluginClient::Node {
    float* last = nullptr;       // [max_chain_channels]: the last sample played
    float* fade_from = nullptr;  // [max_chain_channels]: where the fade-out started
    uint32_t fade_pos = kFadeFrames;
    bool playing = false;  // the last block played was the chain's output
    uint32_t bound_slot = kNoSlot;
    uint32_t bound_generation = 0;
    RtCounter silent_blocks, generation_faults, shape_faults;
};

}  // namespace sonora::plugin_client
