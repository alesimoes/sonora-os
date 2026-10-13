// SPDX-License-Identifier: GPL-3.0-or-later
//
// Misbehaving VST3 plugins for the host's tests, built as a bundle in the
// build tree and never installed. Written against the interface headers
// only, as single-component effects (the component is its own controller).
//
//   vst3:534F4E4F524156535433544553540001  crash        param 0 > 0.5: segfault
//   vst3:534F4E4F524156535433544553540002  stereo-only  refuses any other arrangement
//   vst3:534F4E4F524156535433544553540003  yes-man      says yes to mono, stays stereo
//   vst3:534F4E4F524156535433544553540004  liar         declares stereo, writes 8 channels
//
// SONORA_TEST_VST3_CRASH_ON_SCAN=1 makes GetPluginFactory segfault, for the
// test that a crashing scan costs one --list entry and not the list.

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

enum class Kind { Crash = 1, StereoOnly = 2, YesMan = 3, Liar = 4 };

void make_cid(Kind k, TUID out) {
    const TUID base = INLINE_UID(0x534F4E4F, 0x52415653, 0x54335445, 0x53540000);
    std::memcpy(out, base, sizeof(TUID));
    out[15] = static_cast<char>(static_cast<int>(k));
}

bool same(const TUID a, const TUID b) { return std::memcmp(a, b, sizeof(TUID)) == 0; }

void copy16(String128 dst, const char* src) {
    int i = 0;
    for (; src[i] && i < 127; ++i) dst[i] = static_cast<char16>(src[i]);
    dst[i] = 0;
}

class TestEffect final : public IComponent, public IAudioProcessor, public IEditController {
public:
    explicit TestEffect(Kind kind) : kind_(kind) {
        const bool stereo = kind != Kind::Crash;
        arrangement_ = stereo ? SpeakerArr::kStereo : SpeakerArr::kMono;
    }

    // FUnknown
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (same(iid, FUnknown_iid) || same(iid, IPluginBase_iid) || same(iid, IComponent_iid))
            *obj = static_cast<IComponent*>(this);
        else if (same(iid, IAudioProcessor_iid))
            *obj = static_cast<IAudioProcessor*>(this);
        else if (same(iid, IEditController_iid))
            *obj = static_cast<IEditController*>(this);
        else {
            *obj = nullptr;
            return kNoInterface;
        }
        addRef();
        return kResultOk;
    }
    uint32 PLUGIN_API addRef() override { return ++refs_; }
    uint32 PLUGIN_API release() override {
        const uint32 r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    // IPluginBase
    tresult PLUGIN_API initialize(FUnknown*) override { return kResultOk; }
    tresult PLUGIN_API terminate() override { return kResultOk; }

    // IComponent
    tresult PLUGIN_API getControllerClassId(TUID) override { return kNotImplemented; }
    tresult PLUGIN_API setIoMode(IoMode) override { return kResultOk; }
    int32 PLUGIN_API getBusCount(MediaType type, BusDirection) override { return type == kAudio ? 1 : 0; }
    tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index, BusInfo& bus) override {
        if (type != kAudio || index != 0) return kInvalidArgument;
        bus.mediaType = kAudio;
        bus.direction = dir;
        bus.channelCount = SpeakerArr::getChannelCount(arrangement_);
        copy16(bus.name, dir == kInput ? "In" : "Out");
        bus.busType = kMain;
        bus.flags = BusInfo::kDefaultActive;
        return kResultOk;
    }
    tresult PLUGIN_API getRoutingInfo(RoutingInfo&, RoutingInfo&) override { return kNotImplemented; }
    tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override { return kResultOk; }
    tresult PLUGIN_API setActive(TBool) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream*) override { return kResultOk; }

    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* ins, int32 n_in, SpeakerArrangement* outs,
                                          int32 n_out) override {
        if (n_in != 1 || n_out != 1) return kResultFalse;
        switch (kind_) {
        case Kind::Crash:
            if (ins[0] != outs[0] || SpeakerArr::getChannelCount(ins[0]) > 2) return kResultFalse;
            arrangement_ = ins[0];
            return kResultTrue;
        case Kind::YesMan:
            return kResultTrue;  // and changes nothing
        default:
            return ins[0] == SpeakerArr::kStereo && outs[0] == SpeakerArr::kStereo ? kResultTrue
                                                                                    : kResultFalse;
        }
    }
    tresult PLUGIN_API getBusArrangement(BusDirection, int32 index, SpeakerArrangement& arr) override {
        if (index != 0) return kInvalidArgument;
        arr = arrangement_;
        return kResultOk;
    }
    tresult PLUGIN_API canProcessSampleSize(int32 size) override {
        return size == kSample32 ? kResultTrue : kResultFalse;
    }
    uint32 PLUGIN_API getLatencySamples() override { return 0; }
    tresult PLUGIN_API setupProcessing(ProcessSetup&) override { return kResultOk; }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    tresult PLUGIN_API process(ProcessData& data) override {
        if (IParameterChanges* changes = data.inputParameterChanges) {
            for (int32 i = 0; i < changes->getParameterCount(); ++i) {
                IParamValueQueue* q = changes->getParameterData(i);
                int32 offset;
                ParamValue v;
                if (q && q->getParameterId() == 0 && q->getPointCount() > 0 &&
                    q->getPoint(q->getPointCount() - 1, offset, v) == kResultOk)
                    trigger_ = v;
            }
        }
        if (kind_ == Kind::Crash && trigger_ > 0.5) {
            volatile float* nowhere = nullptr;
            *nowhere = 1.f;  // the point of this plugin
        }
        if (data.numInputs < 1 || data.numOutputs < 1) return kResultOk;
        const AudioBusBuffers& in = data.inputs[0];
        AudioBusBuffers& out = data.outputs[0];
        const int32 n = data.numSamples;
        // The liar writes eight channels into a bus it declared as two.
        const int32 channels = kind_ == Kind::Liar ? 8 : out.numChannels;
        for (int32 c = 0; c < channels; ++c) {
            float* o = out.channelBuffers32[c];
            const float* i = c < in.numChannels ? in.channelBuffers32[c] : nullptr;
            for (int32 s = 0; s < n; ++s) o[s] = i ? i[s] : 0.f;
        }
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return 0; }

    // IEditController: one parameter, id 0, "crash" (or "unused").
    tresult PLUGIN_API setComponentState(IBStream*) override { return kResultOk; }
    int32 PLUGIN_API getParameterCount() override { return 1; }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
        if (index != 0) return kInvalidArgument;
        std::memset(&info, 0, sizeof info);
        info.id = 0;
        copy16(info.title, kind_ == Kind::Crash ? "crash" : "unused");
        copy16(info.shortTitle, info.title[0] == 'c' ? "crash" : "unused");
        info.stepCount = 1;
        info.defaultNormalizedValue = 0;
        info.flags = ParameterInfo::kCanAutomate;
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue, String128 s) override {
        copy16(s, "");
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kResultFalse; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API getParamNormalized(ParamID) override { return trigger_; }
    tresult PLUGIN_API setParamNormalized(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API setComponentHandler(IComponentHandler*) override { return kResultOk; }
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

private:
    ~TestEffect() = default;

    Kind kind_;
    SpeakerArrangement arrangement_;
    double trigger_ = 0;
    std::atomic<uint32> refs_{1};
};

class Factory final : public IPluginFactory {
public:
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (same(iid, FUnknown_iid) || same(iid, IPluginFactory_iid)) {
            *obj = this;
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API getFactoryInfo(PFactoryInfo* info) override {
        *info = PFactoryInfo();
        std::strcpy(info->vendor, "SonoraOS tests");
        info->flags = PFactoryInfo::kUnicode;
        return kResultOk;
    }
    int32 PLUGIN_API countClasses() override { return 4; }
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        static const char* const names[] = {"Sonora Test Crash", "Sonora Test Stereo Only",
                                            "Sonora Test Yes-Man", "Sonora Test Liar"};
        if (index < 0 || index >= 4) return kInvalidArgument;
        *info = PClassInfo();
        make_cid(static_cast<Kind>(index + 1), info->cid);
        info->cardinality = PClassInfo::kManyInstances;
        std::strcpy(info->category, kVstAudioEffectClass);
        std::strcpy(info->name, names[index]);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
        *obj = nullptr;
        for (int k = 1; k <= 4; ++k) {
            TUID c;
            make_cid(static_cast<Kind>(k), c);
            if (!same(c, cid)) continue;
            auto* e = new TestEffect(static_cast<Kind>(k));
            const tresult r = e->queryInterface(iid, obj);
            e->release();
            return r;
        }
        return kNoInterface;
    }
};

Factory g_factory;

}  // namespace

extern "C" {

__attribute__((visibility("default"))) bool ModuleEntry(void*) { return true; }
__attribute__((visibility("default"))) bool ModuleExit() { return true; }

__attribute__((visibility("default"))) IPluginFactory* PLUGIN_API GetPluginFactory() {
    if (const char* e = std::getenv("SONORA_TEST_VST3_CRASH_ON_SCAN"); e && *e == '1') {
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    }
    return &g_factory;
}

}  // extern "C"
