// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What a chain needs from one plugin, whatever its format. The chain
// (chain.h) owns the audio buffers, the canaries, NaN/Inf sanitising and
// latency accounting; a Processor only runs plugin code on buffers the chain
// handed it. LV2 (lv2_host.h) and VST3 (vst3_host.h) are the two formats.

#include "sonora/plugin/protocol.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sonora::plugin_host {

enum class PortKind { Audio, Control, Atom, CV, Other };

// One row of --list. For LV2 a port; for VST3 an audio bus (with its
// channel count) or a parameter (index = ParamID, values normalised 0..1).
struct PortInfo {
    uint32_t index = 0;
    std::string symbol, name;
    PortKind kind = PortKind::Other;
    bool input = false;
    bool optional = false;  // lv2:connectionOptional
    // A key (side-chain) input: LV2 lv2:isSideChain, VST3 an aux input bus.
    // Fed from the chain's key channels, never from the signal path.
    bool sidechain = false;
    float min = 0.f, max = 0.f, def = 0.f;
    bool reports_latency = false;
    uint32_t channels = 0;  // VST3 audio buses only
    std::string units;      // VST3 parameters only
};

struct PluginInfo {
    std::string format;  // "lv2" or "vst3"
    std::string uri, name, class_label;
    std::vector<PortInfo> ports;
    // Main audio only. A key input is counted apart: it does not decide
    // whether the plugin fits the chain's channels.
    uint32_t audio_in = 0, audio_out = 0;
    uint32_t sidechain_in = 0;
    int latency_port = -1;         // LV2: the port that reports it
    int64_t latency_frames = -1;   // VST3 --list: getLatencySamples at 48 kHz / 128
    std::vector<std::string> required_features;
    std::vector<std::string> missing_features;  // required and not provided
};

struct ChainConfig {
    uint32_t sample_rate = 48000;
    uint32_t max_frames = 1024;
    uint32_t block_frames = 128;
    uint32_t channels = 1;
    // Key channels beside the signal (host --sidechain): every plugin with a
    // key input reads them, channel k of its key from channel k, a mono key
    // feeding every channel. 0: key inputs are connected to silence.
    uint32_t sidechain_channels = 0;
};

struct ChainError {
    plugin::Refusal reason = plugin::Refusal::None;
    int instance = -1;
    std::string message;
};

class Processor {
public:
    virtual ~Processor() = default;

    const PluginInfo& info() const { return info_; }
    // Instances run per chain slot: 1, or one per channel for a mono plugin.
    virtual uint32_t replicas() const noexcept = 0;

    // Processing thread. No allocation, no lock, no I/O. Reads the input
    // buffers and writes the output buffers given at instantiation.
    virtual void run(uint32_t frames) noexcept = 0;
    // False once the plugin wrote somewhere it was not given, beyond what
    // the chain's own buffer canaries see (VST3: a channel past the bus's
    // declared count). Checked by the chain after every run().
    virtual bool intact() const noexcept { return true; }
    // The plugin's own latency, as last reported. Any thread; it can change
    // under run() when control() re-reads it.
    virtual uint32_t latency() const noexcept = 0;
    // The main thread: the one that created the plugin, while run() goes on
    // on the processing thread. Not RT. Re-reads what the plugin announced
    // from another thread (VST3: restartComponent(kLatencyChanged)), with
    // the calls a format allows only there. Woken through notify_control().
    virtual void control() {}
    // Validation passed. Not RT: a format may ask the plugin things here it
    // must not ask on the processing thread (VST3: the latency of a plugin
    // that only knows it once it has processed).
    virtual void validated() {}
    // The main thread, never while run() may be running. Deactivates and
    // re-activates every instance, so a value that only takes effect at
    // activation (the latency-changing ones of ADR-0011 §1.5: an EQ's
    // phase mode, a limiter's look-ahead) does, and re-reads the latency.
    virtual void reactivate() = 0;
    // A parameter change from the ring, applied to the next run(). `id` is
    // the LV2 port index or the VST3 ParamID. False when the id is not a
    // writable parameter of this plugin.
    virtual bool set_param(uint32_t id, float value) noexcept = 0;
    // A parameter by name, for --set: LV2 port symbol, VST3 title or "#id".
    virtual bool param_id(const std::string& symbol, uint32_t& id) const = 0;

protected:
    PluginInfo info_;
};

// Where processors come from. `in` and `out` are config.channels buffers of
// config.max_frames samples each, `key` config.sidechain_channels of them,
// all owned by the chain and stable for the processor's life. Returns null
// with `error` set on refusal.
class PluginSource {
public:
    virtual ~PluginSource() = default;
    virtual std::unique_ptr<Processor> instantiate(const std::string& id, const ChainConfig& config,
                                                   const std::vector<float*>& in,
                                                   const std::vector<float*>& out,
                                                   const std::vector<float*>& key,
                                                   ChainError& error) = 0;
};

// A plugin announced something the main thread must act on (Processor::
// control). Any thread, the processing one included: bumps control_word()
// and wakes it, no lock, no allocation.
void notify_control() noexcept;
std::atomic<uint32_t>& control_word() noexcept;

std::string json_escape(const std::string& s);
std::string port_kind_name(PortKind k);

}  // namespace sonora::plugin_host
