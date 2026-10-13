// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// LV2 hosting through lilv. The only code in the tree that loads a plugin
// (ADR-0007 §1: nothing outside plugins/host links a plugin SDK).

#include "sonora/plugin/protocol.h"

#include <lilv/lilv.h>
#include <lv2/options/options.h>
#include <lv2/urid/urid.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sonora::plugin_host {

enum class PortKind { Audio, Control, Atom, CV, Other };

struct PortInfo {
    uint32_t index = 0;
    std::string symbol, name;
    PortKind kind = PortKind::Other;
    bool input = false;
    bool optional = false;  // lv2:connectionOptional
    float min = 0.f, max = 0.f, def = 0.f;
    bool reports_latency = false;
};

struct PluginInfo {
    std::string uri, name, class_label;
    std::vector<PortInfo> ports;
    uint32_t audio_in = 0, audio_out = 0;
    int latency_port = -1;
    std::vector<std::string> required_features;
    std::vector<std::string> missing_features;  // required and not provided
};

// URID map shared by every instance in this process. Plugins are allowed to
// call it from instantiate() and, in practice, sometimes from run(); a mutex
// is acceptable because this process has one processing thread and the map
// is only contended by plugins' own worker threads, if any.
class UridMap {
public:
    LV2_URID map(const char* uri);
    const char* unmap(LV2_URID id);
    LV2_URID_Map* map_feature() { return &map_; }
    LV2_URID_Unmap* unmap_feature() { return &unmap_; }
    UridMap();

private:
    std::mutex mutex_;
    std::unordered_map<std::string, LV2_URID> ids_;
    std::vector<std::string> uris_;  // index id - 1
    LV2_URID_Map map_;
    LV2_URID_Unmap unmap_;
};

class Lv2World {
public:
    Lv2World();
    ~Lv2World();
    Lv2World(const Lv2World&) = delete;
    Lv2World& operator=(const Lv2World&) = delete;

    // Installed plugins come from LV2_PATH or lilv's default path. A test
    // bundle is added explicitly so the test does not depend on the user's.
    bool load_bundle(const std::string& dir, std::string& error);

    const LilvPlugin* find(const std::string& uri) const;
    std::vector<const LilvPlugin*> all() const;
    PluginInfo describe(const LilvPlugin* plugin) const;

    LilvWorld* world() const { return world_; }
    UridMap& urids() { return urids_; }
    static bool feature_supported(const std::string& uri);

    struct Nodes {
        LilvNode *audio, *control, *atom, *cv, *input, *output;
        LilvNode *optional, *reports_latency, *latency_designation;
    } n{};

private:
    LilvWorld* world_ = nullptr;
    UridMap urids_;
};

struct ChainConfig {
    uint32_t sample_rate = 48000;
    uint32_t max_frames = 1024;
    uint32_t block_frames = 128;
    uint32_t channels = 1;
};

struct ChainError {
    plugin::Refusal reason = plugin::Refusal::None;
    int instance = -1;
    std::string message;
};

// A linear chain of LV2 instances processing `channels` channels. A mono
// plugin in a multichannel chain is replicated once per channel; any other
// mismatch between declared ports and the chain is refused.
//
// Every audio buffer the plugins see is private to this process and followed
// by a guard region of canary words. A plugin that writes more than it
// declared hits the canaries, which is how "declares 2 channels and writes 8"
// is caught instead of trusted.
class Lv2Chain {
public:
    Lv2Chain(Lv2World& world, const ChainConfig& config);
    ~Lv2Chain();
    Lv2Chain(const Lv2Chain&) = delete;
    Lv2Chain& operator=(const Lv2Chain&) = delete;

    bool add(const std::string& uri, ChainError& error);
    // Run blocks of silence and check what each plugin did against what it
    // declared. Must pass before the chain is offered to the engine.
    bool validate(ChainError& error);

    // The processing thread. No allocation, no lock (barring a plugin's own
    // URID calls), no I/O. Returns false, with `error` set, when a plugin
    // wrote outside its buffers; the process must then be considered corrupt.
    bool process(const float* const* in, float* const* out, uint32_t frames,
                 plugin::ChannelHeader* report, ChainError& error) noexcept;

    bool set_control(uint32_t instance, uint32_t port, float value) noexcept;
    bool set_control(uint32_t instance, const std::string& symbol, float value);

    std::size_t size() const { return slots_.size(); }
    uint32_t instance_latency(std::size_t i) const;
    uint32_t total_latency() const;
    uint32_t replicas(std::size_t i) const;
    const PluginInfo& info(std::size_t i) const;

private:
    struct Slot;
    float* buffer(std::size_t stage, uint32_t channel);
    void fill_canaries(uint32_t frames);
    bool canaries_intact(uint32_t from_frame, uint32_t words) const noexcept;
    void reset_atom_ports(Slot& s) noexcept;

    Lv2World& world_;
    ChainConfig config_;
    std::vector<std::unique_ptr<Slot>> slots_;
    // Stage k is slot k's input and slot k-1's output. Each channel buffer is
    // max_frames samples followed by guard_frames_ canary words.
    std::vector<std::vector<float>> stages_;
    uint32_t guard_frames_;
    uint32_t stride_;

    // Features handed to every instance. Built once, address-stable.
    int32_t min_block_ = 0, max_block_, nominal_block_;
    float rate_f_;
    std::vector<LV2_Options_Option> options_;
    LV2_Feature map_f_, unmap_f_, options_f_, bounded_f_;
    const LV2_Feature* features_[5];
    LV2_URID atom_sequence_, atom_chunk_;
};

std::string json_escape(const std::string& s);
std::string port_kind_name(PortKind k);

}  // namespace sonora::plugin_host
