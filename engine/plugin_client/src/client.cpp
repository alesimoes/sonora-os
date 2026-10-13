// SPDX-License-Identifier: GPL-3.0-or-later
// The control thread's half of PluginClient: the tables are sized here, once,
// and every slot, store and node is (re)written only while no live plan
// names it.
#include "tables.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace sonora::plugin_client {

namespace {

// A table larger than kMaxParamsPerChain would not fit one callback's copy
// budget, and its channel's first submit would wait for ever (auditor L2 on
// PR #49).
ClientConfig clamped(ClientConfig c) {
    if (c.store_slots > plugin::kMaxParamsPerChain) c.store_slots = plugin::kMaxParamsPerChain;
    return c;
}

}  // namespace

static_assert(std::atomic<plugin::ClockHeader*>::is_always_lock_free, "the clock pointer is read on the audio thread");

PluginClient::PluginClient(const ClientConfig& config)
    : config_(clamped(config)),
      slots_(new Slot[config_.max_channels]),
      stores_(new Store[config_.max_stores]),
      empty_store_(new Store()),
      nodes_(new Node[config_.max_nodes]),
      store_values_(std::size_t(config_.max_stores) * config_.store_slots, 0.f),
      node_samples_(std::size_t(config_.max_nodes) * config_.max_chain_channels * 2, 0.f),
      alias_scratch_(std::size_t(config_.max_chain_channels) * config_.block_frames, 0.f),
      alias_in_(config_.max_chain_channels, nullptr) {
    for (uint32_t i = 0; i < config_.max_stores; ++i)
        stores_[i].values = store_values_.data() + std::size_t(i) * config_.store_slots;
    // kNoStore's stand-in: no table, every bypass 0, never written.
    empty_store_->values = nullptr;
    for (uint32_t i = 0; i < config_.max_nodes; ++i) {
        float* base = node_samples_.data() + std::size_t(i) * config_.max_chain_channels * 2;
        nodes_[i].last = base;
        nodes_[i].fade_from = base + config_.max_chain_channels;
    }
    free_slots_.reserve(config_.max_channels);
    // Popped from the back: slot 0 first.
    for (uint32_t i = config_.max_channels; i-- > 0;) free_slots_.push_back(i);
}

void PluginClient::attach_clock(plugin::ClockHeader* clock) { clock_.store(clock, std::memory_order_seq_cst); }

bool PluginClient::detach_clock(uint64_t timeout_ms) {
    clock_.store(nullptr, std::memory_order_seq_cst);
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (in_callback_.load(std::memory_order_seq_cst)) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

PluginClient::~PluginClient() = default;

uint32_t PluginClient::acquire_slot() {
    if (free_slots_.empty()) return kNoSlot;
    const uint32_t s = free_slots_.back();
    free_slots_.pop_back();
    slots_[s].in_use = true;
    return s;
}

uint32_t PluginClient::assign(uint32_t slot, const ChannelBinding& b) {
    Slot& s = slots_[slot];
    s.region = b.region;
    s.header = static_cast<plugin::ChannelHeader*>(b.region);
    s.layout = b.layout;
    s.map = b.map;
    // The audio thread indexes per-instance arrays with it (auditor L2).
    if (s.map.instance_count > plugin::kMaxInstances) s.map.instance_count = plugin::kMaxInstances;
    s.eng = plugin::make_engine_channel(config_.block_frames);
    s.table = plugin::value_table(b.region, b.layout);
    s.latency_frames = b.latency_frames;
    s.submitted_callback = 0;
    s.last = LastSubmit::None;
    s.streak_count = 0;
    s.removed.store(0, std::memory_order_relaxed);
    s.streak.store(0, std::memory_order_relaxed);
    for (RtCounter* c : {&s.submitted, &s.collected, &s.misses, &s.busy_skips, &s.nonfinite_samples,
                         &s.nonfinite_blocks, &s.params_refused, &s.budget_waits, &s.resync_deferred,
                         &s.frame_mismatches})
        c->reset();
    // Last: the release store that makes everything above visible to an
    // audio thread whose slot_current() sees the new generation.
    return plugin::assign_slot(s.generation);
}

void PluginClient::retire(uint32_t slot) {
    Slot& s = slots_[slot];
    if (!s.in_use) return;
    // A new generation at once (code-reviewer L5 on PR #49): a plan still
    // naming the slot after a retire out of order sees a mismatch, never
    // the payload of a region its caller is about to unmap.
    plugin::assign_slot(s.generation);
    s.in_use = false;
    free_slots_.push_back(slot);
}

void PluginClient::reset_store(uint32_t store, const plugin::ParamMap& map, const float* values,
                               const float* bypass) {
    if (store >= config_.max_stores) return;  // kNoStore: nothing to reset
    Store& s = stores_[store];
    s.map = map;
    // A chain whose table would not fit the store is cut to it; the plan
    // builder refuses such a chain before it gets here (shape_faults).
    if (s.map.param_slots > config_.store_slots) s.map.param_slots = config_.store_slots;
    std::fill(s.values, s.values + config_.store_slots, 0.f);
    if (values) std::copy(values, values + s.map.param_slots, s.values);
    for (uint32_t i = 0; i < plugin::kMaxInstances; ++i) s.bypass[i] = bypass ? bypass[i] : 0.f;
}

void PluginClient::reset_node(uint32_t node) {
    if (node >= config_.max_nodes) return;
    Node& n = nodes_[node];
    std::fill(n.last, n.last + config_.max_chain_channels, 0.f);
    std::fill(n.fade_from, n.fade_from + config_.max_chain_channels, 0.f);
    n.fade_pos = kFadeFrames;
    n.playing = false;
    n.bound_slot = kNoSlot;
    n.bound_generation = 0;
    n.processed_callback = 0;
    n.silent_blocks.reset();
    n.generation_faults.reset();
    n.shape_faults.reset();
    n.repeat_calls.reset();
    n.events_dropped.reset();
}

SlotStatus PluginClient::slot_status(uint32_t slot) const {
    SlotStatus st;
    if (slot >= config_.max_channels) return st;
    const Slot& s = slots_[slot];
    st.generation = s.generation.value.load(std::memory_order_acquire);
    st.removed = s.removed.load(std::memory_order_acquire) != 0;
    st.streak = s.streak.load(std::memory_order_relaxed);
    st.submitted = s.submitted.get();
    st.collected = s.collected.get();
    st.misses = s.misses.get();
    st.busy_skips = s.busy_skips.get();
    st.nonfinite_samples = s.nonfinite_samples.get();
    st.nonfinite_blocks = s.nonfinite_blocks.get();
    st.params_refused = s.params_refused.get();
    st.budget_waits = s.budget_waits.get();
    st.resync_deferred = s.resync_deferred.get();
    st.frame_mismatches = s.frame_mismatches.get();
    st.latency_frames = s.latency_frames;
    return st;
}

NodeStatus PluginClient::node_status(uint32_t node) const {
    NodeStatus st;
    if (node >= config_.max_nodes) return st;
    const Node& n = nodes_[node];
    st.silent_blocks = n.silent_blocks.get();
    st.generation_faults = n.generation_faults.get();
    st.shape_faults = n.shape_faults.get();
    st.repeat_calls = n.repeat_calls.get();
    st.events_dropped = n.events_dropped.get();
    return st;
}

uint32_t PluginClient::latency_frames(uint32_t slot) const {
    return slot < config_.max_channels ? slots_[slot].latency_frames : 0;
}

bool PluginClient::removed(uint32_t slot) const {
    return slot < config_.max_channels && slots_[slot].removed.load(std::memory_order_acquire) != 0;
}

}  // namespace sonora::plugin_client
