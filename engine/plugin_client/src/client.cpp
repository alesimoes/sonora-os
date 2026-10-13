// SPDX-License-Identifier: GPL-3.0-or-later
// The control thread's half of PluginClient: the tables are sized here, once,
// and every slot, store and node is (re)written only while no live plan
// names it.
#include "tables.h"

#include <algorithm>

namespace sonora::plugin_client {

PluginClient::PluginClient(const ClientConfig& config)
    : config_(config),
      slots_(new Slot[config.max_channels]),
      stores_(new Store[config.max_stores]),
      nodes_(new Node[config.max_nodes]),
      store_values_(std::size_t(config.max_stores) * config.store_slots, 0.f),
      node_samples_(std::size_t(config.max_nodes) * config.max_chain_channels * 2, 0.f) {
    for (uint32_t i = 0; i < config.max_stores; ++i)
        stores_[i].values = store_values_.data() + std::size_t(i) * config.store_slots;
    for (uint32_t i = 0; i < config.max_nodes; ++i) {
        float* base = node_samples_.data() + std::size_t(i) * config.max_chain_channels * 2;
        nodes_[i].last = base;
        nodes_[i].fade_from = base + config.max_chain_channels;
    }
    free_slots_.reserve(config.max_channels);
    // Popped from the back: slot 0 first.
    for (uint32_t i = config.max_channels; i-- > 0;) free_slots_.push_back(i);
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
    s.eng = plugin::make_engine_channel(config_.block_frames);
    s.table = plugin::value_table(b.region, b.layout);
    s.latency_frames = b.latency_frames;
    s.submitted_callback = 0;
    s.last = LastSubmit::None;
    s.streak_count = 0;
    std::fill(std::begin(s.streak_by_instance), std::end(s.streak_by_instance), 0);
    s.removed.store(0, std::memory_order_relaxed);
    s.culprit.store(-1, std::memory_order_relaxed);
    s.streak.store(0, std::memory_order_relaxed);
    for (RtCounter* c : {&s.submitted, &s.collected, &s.misses, &s.busy_skips, &s.nonfinite_samples,
                         &s.nonfinite_blocks, &s.params_refused, &s.budget_waits, &s.resync_deferred,
                         &s.frame_mismatches})
        c->reset();
    for (RtCounter& c : s.instance_misses) c.reset();
    // Last: the release store that makes everything above visible to an
    // audio thread whose slot_current() sees the new generation.
    return plugin::assign_slot(s.generation);
}

void PluginClient::retire(uint32_t slot) {
    Slot& s = slots_[slot];
    if (!s.in_use) return;
    s.in_use = false;
    free_slots_.push_back(slot);
}

void PluginClient::reset_store(uint32_t store, const plugin::ParamMap& map, const float* values,
                               const float* bypass) {
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
    Node& n = nodes_[node];
    std::fill(n.last, n.last + config_.max_chain_channels, 0.f);
    std::fill(n.fade_from, n.fade_from + config_.max_chain_channels, 0.f);
    n.fade_pos = kFadeFrames;
    n.playing = false;
    n.bound_slot = kNoSlot;
    n.bound_generation = 0;
    n.silent_blocks.reset();
    n.generation_faults.reset();
    n.shape_faults.reset();
}

SlotStatus PluginClient::slot_status(uint32_t slot) const {
    const Slot& s = slots_[slot];
    SlotStatus st;
    st.generation = s.generation.value.load(std::memory_order_acquire);
    st.removed = s.removed.load(std::memory_order_acquire) != 0;
    st.culprit = s.culprit.load(std::memory_order_relaxed);
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
    for (uint32_t i = 0; i < plugin::kMaxInstances; ++i) st.instance_misses[i] = s.instance_misses[i].get();
    st.latency_frames = s.latency_frames;
    return st;
}

NodeStatus PluginClient::node_status(uint32_t node) const {
    const Node& n = nodes_[node];
    NodeStatus st;
    st.silent_blocks = n.silent_blocks.get();
    st.generation_faults = n.generation_faults.get();
    st.shape_faults = n.shape_faults.get();
    return st;
}

uint32_t PluginClient::latency_frames(uint32_t slot) const {
    return slot < config_.max_channels ? slots_[slot].latency_frames : 0;
}

bool PluginClient::removed(uint32_t slot) const {
    return slots_[slot].removed.load(std::memory_order_acquire) != 0;
}

}  // namespace sonora::plugin_client
