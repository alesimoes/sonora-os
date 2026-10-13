// SPDX-License-Identifier: GPL-3.0-or-later
//
// The engine's side of the plugin boundary (ADR-0012 checklist 4): the
// audio thread's InsertChain step, on contracts/plugin layout version 1.
//
// Three fixed tables, allocated when the session is created and never grown:
//
//   channel slots   one per host channel (§5.4, kMaxChannels). The slot's
//                   payload (region, Layout, ParamMap, EngineChannel) is
//                   written by the control thread before assign()'s release
//                   store of the generation; the audio thread reads it only
//                   after slot_current() has seen that generation. A slot
//                   carries the host's block count, its miss streak and the
//                   removal flag, so a plan swap that keeps the host keeps
//                   them.
//   parameter stores  the authoritative values of a chain (§5.1), by index
//                   from the plan node (A2.5), never looked up by chain id.
//                   They outlive a host: a new host's first submit copies
//                   the store in.
//   node states     the fade state of an InsertChain node (rt-engineer M5 on
//                   PR #45), by index from the plan node, kept stable for the
//                   chain's stable id like a delay line. Per node, not per
//                   slot: a make-before-break swap fades out of the old host
//                   and ramps into the new one instead of cutting.
//
// The audio thread's call sequence per callback:
//
//     begin_callback(now_ns);
//     for each InsertChain node, in plan order:
//         process_chain(node, in, out, frames, events, n);
//     end_callback();
//
// Nothing in it waits, allocates, locks or reads a pointer from a region.
// The only common/platform symbol it can reach is wake_all(), and only
// behind contracts/plugin's kHostWakeEnabled, which is false (ADR-0012 §1.4,
// auditor A1). tests/rt_reachability.cmake holds both.
#pragma once

#include "sonora/plugin/protocol.h"
#include "sonora/rt.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace sonora::plugin_client {

// ADR-0012 §3: a missed block fades the last sample played out over this
// many frames (auditor L4). Below a quantum of 64 the fade spans blocks.
inline constexpr uint32_t kFadeFrames = 64;
// ADR-0012 §3: this many consecutive counted misses remove the channel.
inline constexpr uint32_t kMaxConsecutiveMisses = 8;
inline constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
// InsertChainRef::param_store for a chain with no parameter store, the value
// PR #48's plan nodes use: only a chain with no table (param_slots 0) runs
// without one, and its parameter events are dropped, counted.
inline constexpr uint32_t kNoStore = 0xFFFFFFFFu;

struct ClientConfig {
    uint32_t block_frames = 128;
    uint32_t max_channels = 256;  // channel slots, ADR-0012 §5.4's kMaxChannels
    uint32_t max_stores = 256;    // parameter stores (§5.1)
    // Values per store: a chain's dense parameters, at most the table cap.
    uint32_t store_slots = plugin::kMaxParamsPerChain;
    uint32_t max_nodes = 256;  // InsertChain node states (M5)
    // Audio channels of one chain, key channels included.
    uint32_t max_chain_channels = 4;
};

// What a plan's InsertChain node carries (ADR-0012 §7.1), written by the
// control thread when it builds the plan: an opaque handle the node holds
// by value and passes back to process_chain() every callback.
// latency_frames (PluginClient::latency_frames, or
// SupervisedChain::latency_frames) and chain_id stay in the plan node
// itself: the audio thread never reads either.
struct InsertChainRef {
    uint32_t channel_slot = kNoSlot;
    uint32_t channel_generation = 0;  // the slot generation the plan was built against
    uint32_t param_store = kNoStore;
    uint32_t node_state = 0;
    uint32_t channels = 0;  // the chain's audio channels, in and out
};

// A parameter change the audio thread has for a node this callback: from an
// RtCommand (a UI move, an editor touch) or automation. Dense index and
// format-native value (contracts/plugin, "Parameters"); param may be
// kBypassParam. offset is the frame inside this callback's block.
struct ParamEvent {
    uint32_t instance = 0;
    uint32_t param = 0;
    uint32_t offset = 0;
    float value = 0.f;
};

enum class Outcome : uint8_t {
    Collected,  // `out` is the chain's output for the previous callback's block
    Missed,     // `out` is the fade-out, then silence (ADR-0012 §3)
    Removed,    // the channel is out of use; `out` is silence
};

// What the control thread hands assign(): a host that reached Ready.
struct ChannelBinding {
    void* region = nullptr;  // the channel region, mapped read-write here
    plugin::Layout layout;   // the engine's own, from its configuration
    plugin::ParamMap map;    // from the scan's dense counts
    uint32_t latency_frames = 0;  // checked_latency after Ready, for the plan builder (PDC)
};

// Counters of one slot, for the control thread. Each is monotonic for the
// slot's current host and reset when the slot is assigned.
struct SlotStatus {
    uint32_t generation = 0;
    bool removed = false;
    uint32_t streak = 0;
    uint64_t submitted = 0;
    uint64_t collected = 0;
    uint64_t misses = 0;              // counted toward the streak
    uint64_t busy_skips = 0;          // submits skipped: the host still on an older block
    uint64_t nonfinite_samples = 0;   // NaN/Inf in collected copies (collect_output)
    uint64_t nonfinite_blocks = 0;    // blocks those made a miss and a fault
    uint64_t params_refused = 0;      // ring pushes refused (values_seq bumped instead)
    uint64_t budget_waits = 0;        // first submits put off by the A2 copy budget
    uint64_t resync_deferred = 0;     // held-change copies put off by it
    uint64_t frame_mismatches = 0;    // callbacks whose frame count was not block_frames
    uint32_t latency_frames = 0;
};

struct NodeStatus {
    uint64_t silent_blocks = 0;      // every block it did not play the chain's output
    uint64_t generation_faults = 0;  // the plan named a slot of another host (H1)
    uint64_t shape_faults = 0;       // channels or table size not those of the slot
    uint64_t repeat_calls = 0;       // a second process_chain for the node in one callback
    uint64_t events_dropped = 0;     // parameter events with no store to keep them
};

class PluginClient {
public:
    // Control thread, at session creation: every table, sized here.
    explicit PluginClient(const ClientConfig& config);
    ~PluginClient();
    PluginClient(const PluginClient&) = delete;
    PluginClient& operator=(const PluginClient&) = delete;

    const ClientConfig& config() const { return config_; }

    // ---- control thread

    // The session clock the audio thread stamps and ticks; without one it
    // runs unclocked (unit tests). Any time, though normally before the
    // backend is activated.
    void attach_clock(plugin::ClockHeader* clock);
    // Takes the clock away, and returns only once no callback can still be
    // using it, so the caller may then unmap it (code-reviewer M1, auditor
    // L3 on PR #49). The audio thread loads the pointer once per callback,
    // in begin_callback, after flagging itself inside one; this stores null
    // and then waits for that flag to clear: whichever runs second sees
    // the other (both sequentially consistent). Normally the backend is
    // already deactivated and this returns at once. False if a callback
    // stayed inside for `timeout_ms` (a callback that never reached
    // end_callback): the caller must then NOT unmap the clock.
    bool detach_clock(uint64_t timeout_ms = 5000);

    // A free slot, or kNoSlot when all are in use or awaiting retire.
    uint32_t acquire_slot();
    // Writes the slot's payload, then publishes a new generation (release),
    // which it returns for the plan node. Only for a slot from
    // acquire_slot() that no plan names (the epoch rule, H1).
    uint32_t assign(uint32_t slot, const ChannelBinding& binding);
    // Back to the free list. Only after every plan that named the slot has
    // been retired (ADR-0009 §4.1); its region may be released after this.
    void retire(uint32_t slot);
    // The store's dense layout and values, and the bypass of each instance.
    // Only for a store no live plan names.
    void reset_store(uint32_t store, const plugin::ParamMap& map, const float* values,
                     const float* bypass);
    // A node state back to "never played". Only for one no live plan names.
    void reset_node(uint32_t node);

    // The chain latency the slot's host reported at Ready (checked_latency,
    // the IPC block included), for the plan builder to copy into the
    // InsertChain node's latency_frames (PDC, §7.3). Never read on the
    // audio thread.
    uint32_t latency_frames(uint32_t slot) const;

    SlotStatus slot_status(uint32_t slot) const;
    NodeStatus node_status(uint32_t node) const;
    // The removal flag of §3, for the control thread's poll (acquire).
    bool removed(uint32_t slot) const;
    // Callbacks since construction, for the control thread's diagnostics.
    uint64_t callbacks() const { return callbacks_seen_.load(std::memory_order_relaxed); }

    // ---- audio thread

    // `now_ns`: CLOCK_MONOTONIC at the callback's start, a timestamp the
    // callback already takes (ADR-0012 §1.1, auditor L2).
    void begin_callback(uint64_t now_ns) noexcept SONORA_RT;
    // One InsertChain node (ADR-0012 §7.2 steps 0-2). `in` and `out` have
    // node.channels planes of `frames`; `in[c]` may be null (silence).
    // `out` is engine-private and receives what this node plays.
    //
    // Preconditions (code-reviewer M2 on PR #49), each checked:
    //  - Between begin_callback and end_callback, ONCE per node per
    //    callback, with the callback's whole block: never per slice of a
    //    split block. A second call for the same node in one callback plays
    //    silence, touches no slot and counts repeat_calls; the first call's
    //    output stands.
    //  - frames == ClientConfig::block_frames (contracts/plugin, "Frame
    //    count"). Another count is a miss with nothing submitted and every
    //    change held (frame_mismatches); never more than block_frames is
    //    read from `in` or written to `out` beyond `frames`.
    //  - `in` may alias `out`, even plane for plane (an in-place node): the
    //    input is copied aside before the output is written.
    //  - events: at most what the caller holds, with param_store a valid
    //    store or kNoStore.
    Outcome process_chain(const InsertChainRef& node, const float* const* in, float* const* out,
                          uint32_t frames, const ParamEvent* events, uint32_t event_count) noexcept SONORA_RT;
    // §7.2 step 3: one tick if anything was submitted, and the wake only
    // behind kHostWakeEnabled.
    void end_callback() noexcept SONORA_RT;

    struct Slot;
    struct Store;
    struct Node;

private:
    ClientConfig config_;
    std::atomic<plugin::ClockHeader*> clock_{nullptr};
    std::atomic<uint32_t> in_callback_{0};
    std::unique_ptr<Slot[]> slots_;
    std::unique_ptr<Store[]> stores_;
    std::unique_ptr<Store> empty_store_;  // what a kNoStore node uses: no table, no bypass
    std::unique_ptr<Node[]> nodes_;
    std::vector<float> store_values_;  // max_stores * store_slots
    std::vector<float> node_samples_;  // max_nodes * max_chain_channels * 2
    std::vector<float> alias_scratch_;  // max_chain_channels * block_frames
    std::vector<const float*> alias_in_;  // max_chain_channels planes into it
    std::vector<uint32_t> free_slots_;  // control thread only

    // Audio thread only.
    plugin::ClockHeader* cb_clock_ = nullptr;  // clock_ as loaded at begin_callback
    uint64_t callback_ = 0;
    std::size_t budget_left_ = 0;
    bool submitted_any_ = false;
    // Out-of-range indices in a node; nothing else could be counted.
    std::atomic<uint64_t> bad_refs_{0};
    std::atomic<uint64_t> callbacks_seen_{0};
};

}  // namespace sonora::plugin_client
