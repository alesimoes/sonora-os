// SPDX-License-Identifier: GPL-3.0-or-later
//
// The DPF glue (ADR-0011 §1.2): one DISTRHO::Plugin that wraps whichever
// Processor the bundle names (entry.h). Compiled once per bundle and format,
// because DPF describes a plugin with per-bundle macros
// (DistrhoPluginInfo.h, generated). Everything that is not translation
// between DPF's calls and ours lives in the neutral Bridge, where it is
// tested without DPF.
//
// What maps to what:
// - Parameter table (ParamInfo) -> DPF Parameter: the id is the LV2 symbol
//   and the VST3 state key of every parameter but index 0, the index is the
//   VST3 ParamID (plus DPF's internal offset), so ids and indices stay
//   append-only (§2.4) in every format.
// - Index 0, our latency-compensated bypass (§1.5), carries DPF's bypass
//   designation, so a host's own bypass button drives it instead of
//   cutting the plugin out with no delay. In LV2 that port is the standard
//   lv2:enabled: DPF's exporter forces the symbol "lv2_enabled", not our
//   id "bypass", and inverts the value (1 = processing). The bundle's
//   descriptor.json carries that mapping (bundle_files.cpp), so a
//   descriptor-driven editor finds the port.
// - Latency is reported from activate(), the only place it changes.
#include <cstring>
#include <memory>

#include "DistrhoPlugin.hpp"
#include "sonora/native/format/bridge.h"
#include "sonora/native/format/entry.h"

START_NAMESPACE_DISTRHO

namespace {

using sonora::native::Descriptor;
using sonora::native::ParamInfo;
using sonora::native::Taper;
using sonora::native::Unit;

const Descriptor& bundle_descriptor() {
    // DPF asks for the parameter count before the instance exists (the
    // Plugin base constructor), so the table is read from a probe instance
    // once. The table is static data; the probe is never prepared.
    static const std::unique_ptr<sonora::native::Processor> probe =
        sonora::native::format::make_bundle_processor();
    return probe->descriptor();
}

const char* unit_text(Unit u) {
    // DPF maps "dB", "Hz", "ms" and "%" to LV2's unit vocabulary; anything
    // else is written as a custom unit with this text.
    switch (u) {
    case Unit::Decibel: return "dB";
    case Unit::Hertz: return "Hz";
    case Unit::Milliseconds: return "ms";
    case Unit::Percent: return "%";
    case Unit::Bpm: return "BPM";
    default: return "";
    }
}

}  // namespace

class SonoraPlugin final : public Plugin {
public:
    SonoraPlugin()
        : Plugin(bundle_descriptor().param_count, 0, 0),
          bridge_(sonora::native::format::make_bundle_processor(), DISTRHO_PLUGIN_NUM_INPUTS) {
        // Prepared once here so the latency is known before the host
        // activates (a VST3 host may ask getLatencySamples() first); the
        // host's own rate and block size arrive with activate().
        bridge_.activate(getSampleRate(), getBufferSize());
        setLatency(bridge_.latency());
    }

protected:
    const char* getLabel() const override { return bridge_.descriptor().id; }
    const char* getDescription() const override { return bridge_.descriptor().category; }
    const char* getMaker() const override { return "Sonora"; }
    const char* getLicense() const override { return "GPL-3.0-or-later"; }
    // The suite's version, not the state version (that one travels in the
    // state itself, ADR-0011 §2.4). Odd LV2 minor numbers mark development
    // builds, which these are until N1 ships in the image.
    uint32_t getVersion() const override { return d_version(0, 1, 0); }
    int64_t getUniqueId() const override { return d_cconst(SONORA_PLUGIN_UNIQUE_ID); }

    void initAudioPort(bool input, uint32_t index, AudioPort& port) override {
        port.groupId = kPortGroupStereo;
        Plugin::initAudioPort(input, index, port);
    }

    void initParameter(uint32_t index, Parameter& p) override {
        const Descriptor& d = bridge_.descriptor();
        if (index >= d.param_count) return;
        const ParamInfo& info = d.params[index];
        if (index == 0) {
            // ParamInfo index 0 is always kBypassParam (param.h).
            p.initDesignation(kParameterDesignationBypass);
            // The VST3 state key. The LV2 exporter writes "lv2_enabled"
            // whatever this says (see the top of the file).
            p.symbol = info.id;
            return;
        }
        p.name = info.name;
        p.symbol = info.id;
        p.unit = unit_text(info.unit);
        p.ranges.min = info.min;
        p.ranges.max = info.max;
        p.ranges.def = info.def;
        uint32_t hints = 0;
        if (info.automatable) hints |= kParameterIsAutomatable;
        if (info.taper == Taper::Stepped) hints |= kParameterIsInteger;
        if (info.unit == Unit::Toggle) hints |= kParameterIsBoolean | kParameterIsInteger;
        if (info.taper == Taper::Log) hints |= kParameterIsLogarithmic;
        p.hints = hints;
        if (info.unit == Unit::Choice && info.choices != nullptr && info.choice_count > 0) {
            // Choice values are min, min + 1, ...: the index into `choices`
            // offset by the range's start (param.h).
            ParameterEnumerationValue* const values = new ParameterEnumerationValue[info.choice_count];
            for (uint32_t i = 0; i < info.choice_count; ++i) {
                values[i].value = info.min + static_cast<float>(i);
                values[i].label = info.choices[i];
            }
            p.enumValues.count = static_cast<uint8_t>(info.choice_count);
            p.enumValues.restrictedMode = true;
            p.enumValues.values = values;
        }
    }

    float getParameterValue(uint32_t index) const override { return bridge_.value(index); }
    void setParameterValue(uint32_t index, float value) override { bridge_.set_value(index, value); }

    // From the host's activate (LV2 activate, VST3 setActive), from DPF
    // around a rate or block-size change, and from DPF's own lazy
    // activation inside run() when a host processes an instance it never
    // activated: that last one is on the audio thread. Never while run()
    // is running. The rate and block size were prepared for in the
    // constructor or in the two callbacks below, so unless a reconfiguring
    // value is pending this does not allocate (Bridge::activate).
    void activate() override {
        bridge_.activate(getSampleRate(), getBufferSize());
        setLatency(bridge_.latency());
    }

    // DPF calls these on the host's main thread (VST3 setupProcessing, LV2
    // instantiate and options), whether or not the instance is active.
    // Preparing here, rather than at the next activate(), keeps a lazy
    // activation on the audio thread free.
    void bufferSizeChanged(uint32_t) override {
        bridge_.activate(getSampleRate(), getBufferSize());
        setLatency(bridge_.latency());
    }
    void sampleRateChanged(double) override {
        bridge_.activate(getSampleRate(), getBufferSize());
        setLatency(bridge_.latency());
    }

    void run(const float** inputs, float** outputs, uint32_t frames) override {
        sonora::native::Transport transport;
#if DISTRHO_PLUGIN_WANT_TIMEPOS
        const TimePosition& pos = getTimePosition();
        if (pos.bbt.valid && pos.bbt.beatsPerMinute > 0.0) {
            transport.tempo_bpm = pos.bbt.beatsPerMinute;
            transport.tempo_valid = true;
        }
#endif
        bridge_.run(inputs, outputs, frames, transport);
    }

private:
    sonora::native::format::Bridge bridge_;

    DISTRHO_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SonoraPlugin)
};

Plugin* createPlugin() { return new SonoraPlugin(); }

END_NAMESPACE_DISTRHO
