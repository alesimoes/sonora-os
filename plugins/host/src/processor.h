// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What a chain needs from one plugin, whatever its format. The chain
// (chain.h) owns the audio buffers, the canaries, NaN/Inf sanitising and
// latency accounting; a Processor only runs plugin code on buffers the chain
// handed it. LV2 (lv2_host.h) and VST3 (vst3_host.h) are the two formats.

#include "sonora/plugin/protocol.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
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
    // Fed from the chain's key channels; with none routed, from the
    // plugin's own input (a key input on silence would key from silence).
    bool sidechain = false;
    float min = 0.f, max = 0.f, def = 0.f;
    bool reports_latency = false;
    uint32_t channels = 0;  // VST3 audio buses only
    // A parameter's unit: VST3 ParameterInfo::units, LV2 units:unit's
    // symbol. Empty when the plugin does not say.
    std::string units;
    // What the probe record says of a control input (contracts/plugin
    // probe_record.h): a toggle, a choice among `labels`, a value shown on a
    // log scale, and whether a host may automate it. `steps`: 0 continuous,
    // otherwise the number of values evenly spaced from min to max.
    bool toggle = false;
    bool enumeration = false;
    bool logarithmic = false;
    bool automatable = true;
    uint32_t steps = 0;
    std::vector<std::string> labels;
};

struct PluginInfo {
    std::string format;  // "lv2" or "vst3"
    std::string uri, name, class_label;
    // For the probe record: who made it, which release, what a browser
    // groups it by (broadest first), and the bundle it was found in,
    // absolute, which the engine fingerprints.
    std::string vendor, version, bundle;
    std::vector<std::string> categories;
    std::vector<PortInfo> ports;
    // Main audio only. A key input is counted apart: it does not decide
    // whether the plugin fits the chain's channels.
    uint32_t audio_in = 0, audio_out = 0;
    uint32_t sidechain_in = 0;
    int latency_port = -1;         // LV2: the port that reports it
    int64_t latency_frames = -1;   // VST3 --list: getLatencySamples at 48 kHz / 128
    std::vector<std::string> required_features;
    std::vector<std::string> missing_features;  // required and not provided
    int enabled_port = -1;  // LV2: the input port designated lv2:enabled
};

struct ChainConfig {
    uint32_t sample_rate = 48000;
    uint32_t max_frames = 1024;
    uint32_t block_frames = 128;
    uint32_t channels = 1;
    // Key channels beside the signal (host --sidechain): every plugin with a
    // key input reads them, channel k of its key from channel k, a mono key
    // feeding every channel. 0: no key; a key input reads the plugin's own
    // input, which keys like no key at all (ADR-0011 Amendment 2 item 3).
    uint32_t sidechain_channels = 0;
};

struct ChainError {
    plugin::Refusal reason = plugin::Refusal::None;
    int instance = -1;
    std::string message;
};

// A plugin's controller or editor touching a parameter (VST3 beginEdit,
// performEdit, endEdit), from whatever thread the plugin chose. `dense` is
// already mapped (contracts/plugin host_control.h, "Parameter touches"):
// a dense index, or kBypassParam with 1 = bypassed. Unmapped: a ParamID the
// host has no dense index for, counted and dropped. Never blocks: a sink
// that cannot take an event at once drops and counts it.
enum class TouchKind : uint8_t { Begin, Perform, End, Unmapped };

class TouchSink {
public:
    virtual ~TouchSink() = default;
    virtual void touch(uint32_t instance, TouchKind kind, uint32_t dense, float value) noexcept = 0;
};

// The result of a state save, the format's sections (host_control.h
// StateBlob). Empty: the plugin keeps no state of its own.
enum class SaveResult { Saved, Empty, Failed };

class Processor {
public:
    virtual ~Processor() = default;

    const PluginInfo& info() const { return info_; }
    // Instances run per chain slot: 1, or one per channel for a mono plugin.
    virtual uint32_t replicas() const noexcept = 0;

    // Processing thread. No allocation, no lock, no I/O. Reads the input
    // buffers and writes the output buffers given at instantiation.
    virtual void run(uint32_t frames) noexcept SONORA_RT = 0;
    // False once the plugin wrote somewhere it was not given, beyond what
    // the chain's own buffer canaries see (VST3: a channel past the bus's
    // declared count). Checked by the chain after every run().
    virtual bool intact() const noexcept SONORA_RT { return true; }
    // The plugin's own latency, as last reported. Any thread; it can change
    // under run() when control() re-reads it.
    virtual uint32_t latency() const noexcept SONORA_RT = 0;
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
    // False when an instance refused to activate again, or could not be
    // re-activated by `deadline_ns` (monotonic_ns; LV2: its worker still
    // inside work()), with the reason in `error`: it must not run.
    virtual bool reactivate(uint64_t deadline_ns, std::string& error) = 0;
    // Parameters by DENSE index (contracts/plugin, "Parameters"): the
    // writable parameters in the plugin's own order. LV2: input control
    // ports by port index. VST3: writable parameters in controller order.
    virtual uint32_t param_count() const noexcept = 0;
    // A change for the next run(), at frame `offset` inside it. The value is
    // format-native (LV2 plain, VST3 normalised). LV2 applies it at the
    // block start (ADR-0012 §5.1). False when `dense` is out of range or the
    // value is not finite; the caller counts it.
    virtual bool set_param(uint32_t dense, float value, uint32_t offset) noexcept SONORA_RT = 0;
    // A last-value table value: from the block start, superseding any change
    // already queued for this parameter in the next run().
    virtual bool replace_param(uint32_t dense, float value) noexcept SONORA_RT { return set_param(dense, value, 0); }
    // A parameter by name, for --set: LV2 port symbol, VST3 title or "#ParamID".
    virtual bool param_index(const std::string& symbol, uint32_t& dense) const = 0;
    // The stable key the session file stores (ADR-0012 §6.1): LV2 port
    // symbol, VST3 ParamID in decimal. And the default, as described.
    virtual std::string param_key(uint32_t dense) const = 0;
    // The name --set takes for it: LV2 port symbol, VST3 title.
    virtual std::string param_symbol(uint32_t dense) const = 0;
    virtual float param_default(uint32_t dense) const = 0;
    // The plugin's own bypass parameter (VST3 kIsBypass, LV2 lv2:enabled),
    // as a dense index, or -1 when the host bypasses it with a delay line.
    virtual int bypass_param() const noexcept SONORA_RT { return -1; }
    // The value of bypass_param() for bypassed or not.
    virtual float bypass_value(bool bypassed) const noexcept SONORA_RT { return bypassed ? 1.f : 0.f; }
    // The format-native range of a dense parameter: the LV2 port's, VST3's
    // normalised [0, 1].
    virtual void param_range(uint32_t, float& min, float& max) const {
        min = 0.f;
        max = 1.f;
    }
    // The description of a dense parameter, as the plugin gave it.
    virtual const PortInfo& param_port(uint32_t dense) const = 0;

    // The main thread, after instantiation. Touches from this plugin go to
    // `sink` as chain instance `instance`. A format without a source of
    // touches (LV2 until its UIs are hosted) ignores it.
    virtual void set_touch_sink(TouchSink*, uint32_t) {}
    // The main thread: a dense parameter's value as the plugin's own side
    // (its controller) holds it now, for ending a gesture whose end was
    // lost. False when the format cannot say.
    virtual bool current_value(uint32_t, float&) const { return false; }
    // The main thread, while run() goes on (the formats allow it: VST3
    // getState is a main-thread call, LV2 save() may run beside run()).
    virtual SaveResult save_state(std::vector<std::string>&, std::string&) { return SaveResult::Empty; }
    // The main thread, before the processing thread starts, never while
    // run() may run, and done by `deadline_ns` (monotonic_ns) or not at all.
    // False, with the reason, when the plugin refused it or it was not done.
    virtual bool restore_state(const std::vector<std::string>& sections, uint64_t /*deadline_ns*/,
                               std::string& error) {
        if (sections.empty()) return true;
        error = "this plugin keeps no state of its own";
        return false;
    }
    // Display text for a value (host_control.h DescribeValue). The main thread.
    virtual bool describe_value(uint32_t dense, float value, std::string& out) const {
        if (dense >= param_count()) return false;
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.6g", double(value));
        out = buf;
        return true;
    }

protected:
    PluginInfo info_;
};

// Where processors come from. `in` and `out` are config.channels buffers of
// config.max_frames samples each, all owned by the chain and stable for the
// processor's life. `key` is empty without a key, and otherwise at least
// config.channels buffers (a narrower key repeated across them), so a
// replica r reads a buffer of its own, key[r]. Returns null with `error`
// set on refusal.
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
