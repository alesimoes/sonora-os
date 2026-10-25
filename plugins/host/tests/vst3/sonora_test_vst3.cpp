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
//   vst3:534F4E4F524156535433544553540005  turncoat     takes stereo, then answers 8 channels
//                                                       once its main bus is activated
//   vst3:534F4E4F524156535433544553540006  params       256 parameters; writes how many
//                                                       changed in a block, their sum, and
//                                                       the frame offset of param 0's change
//   vst3:534F4E4F524156535433544553540007  latency      param 0 > 0.5: latency 0 -> 64, told
//                                                       from another thread; segfaults if
//                                                       asked its latency on the thread
//                                                       that runs process()
//   vst3:534F4E4F524156535433544553540008  once         mono pass-through; activates
//                                                       once, and refuses setActive(true)
//                                                       after a setActive(false)
//   vst3:534F4E4F524156535433544553540009  state        mono pass-through; counts its blocks
//                                                       and keeps the count in its state
//                                                       ("SNTS" + int64); refuses another
//                                                       magic in setState
//   vst3:534F4E4F52415653543354455354000A  touch        gain on ParamID 7 "level" (and a
//                                                       read-only ParamID 9); 300 ms after
//                                                       it is given a component handler,
//                                                       its "editor" thread touches: a
//                                                       gesture 0.25 -> 0.75, a lone edit
//                                                       to 0.5, an edit of the read-only
//                                                       one, and a gesture left open at
//                                                       0.125
//
// SONORA_TEST_VST3_CRASH_ON_SCAN=1 makes GetPluginFactory segfault, for the
// test that a crashing scan costs one --list entry and not the list.
// SONORA_TEST_VST3_HANG_ON_SCAN=1 makes the latency class hang when it is
// instantiated: the scan is killed after the classes before it (params
// among them) were described and written.

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

enum class Kind {
    Crash = 1, StereoOnly = 2, YesMan = 3, Liar = 4, Turncoat = 5, Params = 6, Latency = 7, Once = 8,
    State = 9, Touch = 10
};
constexpr int kKinds = 10;
constexpr ParamID kTouchLevel = 7, kTouchMeter = 9;
constexpr char kStateMagic[4] = {'S', 'N', 'T', 'S'};
// More than the 128 one block once carried; the host's ring holds 256.
constexpr int32 kParamsCount = 256;
constexpr uint32 kLatencyAfter = 64;
constexpr int32 kDelayRing = 4096;  // power of two, more than a block plus kLatencyAfter

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
        const bool stereo = kind != Kind::Crash && kind != Kind::Params && kind != Kind::Latency &&
                            kind != Kind::Once && kind != Kind::State && kind != Kind::Touch;
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
    tresult PLUGIN_API activateBus(MediaType type, BusDirection, int32 index, TBool state) override {
        // Agreed to stereo; the main bus "becomes" 8 channels when activated.
        if (kind_ == Kind::Turncoat && type == kAudio && index == 0 && state)
            arrangement_ = SpeakerArr::k71Cine;
        return kResultOk;
    }
    tresult PLUGIN_API setActive(TBool state) override {
        if (kind_ != Kind::Once || !state) return kResultOk;
        return activations_++ == 0 ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API setState(IBStream* s) override {
        if (kind_ != Kind::State) return kResultOk;
        char magic[4];
        int64 n = 0;
        int32 got = 0;
        if (!s || s->read(magic, 4, &got) != kResultOk || got != 4 || std::memcmp(magic, kStateMagic, 4) != 0)
            return kResultFalse;
        if (s->read(&n, sizeof n, &got) != kResultOk || got != int32(sizeof n)) return kResultFalse;
        blocks_.store(n, std::memory_order_relaxed);
        return kResultOk;
    }
    tresult PLUGIN_API getState(IBStream* s) override {
        if (kind_ != Kind::State) return kResultOk;
        int64 n = blocks_.load(std::memory_order_relaxed);
        int32 put = 0;
        if (!s || s->write(const_cast<char*>(kStateMagic), 4, &put) != kResultOk || put != 4) return kResultFalse;
        return s->write(&n, sizeof n, &put) == kResultOk && put == int32(sizeof n) ? kResultOk : kResultFalse;
    }

    // IAudioProcessor
    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* ins, int32 n_in, SpeakerArrangement* outs,
                                          int32 n_out) override {
        if (n_in != 1 || n_out != 1) return kResultFalse;
        switch (kind_) {
        case Kind::Crash:
        case Kind::Params:
        case Kind::Latency:
        case Kind::Once:
        case Kind::State:
        case Kind::Touch:
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
    uint32 PLUGIN_API getLatencySamples() override {
        // VST3 asks for this on the main thread. A host that asks on the
        // thread running process() is what this class exists to catch.
        if (armed_.load(std::memory_order_acquire) && std::this_thread::get_id() == process_thread_) {
            volatile int* nowhere = nullptr;
            *nowhere = 1;
        }
        return latency_.load(std::memory_order_acquire);
    }
    tresult PLUGIN_API setupProcessing(ProcessSetup&) override { return kResultOk; }
    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    tresult PLUGIN_API process(ProcessData& data) override {
        int32 changed = 0;
        double sum = 0;
        int32 offset0 = -1;  // where parameter 0's last change landed in this block
        if (IParameterChanges* changes = data.inputParameterChanges) {
            changed = changes->getParameterCount();
            for (int32 i = 0; i < changed; ++i) {
                IParamValueQueue* q = changes->getParameterData(i);
                int32 offset;
                ParamValue v;
                if (!q || q->getPointCount() < 1 ||
                    q->getPoint(q->getPointCount() - 1, offset, v) != kResultOk)
                    continue;
                sum += v;
                if (q->getParameterId() == 0) {
                    trigger_ = v;
                    offset0 = offset;
                }
                if (kind_ == Kind::Touch && q->getParameterId() == kTouchLevel) level_ = float(v);
            }
        }
        if (kind_ == Kind::Crash && trigger_ > 0.5) {
            volatile float* nowhere = nullptr;
            *nowhere = 1.f;  // the point of this plugin
        }
        if (kind_ == Kind::Latency && trigger_ > 0.5 && !armed_.load(std::memory_order_relaxed)) {
            // As LSP does: the new latency applies at once, and the
            // controller hears of it from another thread a little later.
            process_thread_ = std::this_thread::get_id();
            latency_.store(kLatencyAfter, std::memory_order_release);
            armed_.store(true, std::memory_order_release);
            notify_requested_.store(true, std::memory_order_release);
        }
        if (data.numInputs < 1 || data.numOutputs < 1) return kResultOk;
        const AudioBusBuffers& in = data.inputs[0];
        AudioBusBuffers& out = data.outputs[0];
        const int32 n = data.numSamples;
        if (kind_ == Kind::Params) {
            for (int32 c = 0; c < out.numChannels; ++c) {
                float* o = out.channelBuffers32[c];
                for (int32 s = 0; s < n; ++s) o[s] = 0.f;
                if (n > 2) {
                    o[0] = static_cast<float>(changed);
                    o[1] = static_cast<float>(sum);
                    o[2] = static_cast<float>(offset0);
                }
            }
            return kResultOk;
        }
        if (kind_ == Kind::Latency) {
            const auto d = static_cast<int32>(latency_.load(std::memory_order_relaxed));
            for (int32 c = 0; c < out.numChannels && c < 2; ++c) {
                const float* i = in.channelBuffers32[c];
                float* o = out.channelBuffers32[c];
                int32 w = write_;
                for (int32 s = 0; s < n; ++s, w = (w + 1) & (kDelayRing - 1)) {
                    delay_[c][w] = i[s];
                    o[s] = delay_[c][(w - d) & (kDelayRing - 1)];
                }
            }
            write_ = (write_ + n) & (kDelayRing - 1);
            return kResultOk;
        }
        if (kind_ == Kind::State) blocks_.fetch_add(1, std::memory_order_relaxed);
        const float gain = kind_ == Kind::Touch ? level_ : 1.f;
        // The liar writes eight channels into a bus it declared as two.
        const int32 channels = kind_ == Kind::Liar ? 8 : out.numChannels;
        for (int32 c = 0; c < channels; ++c) {
            float* o = out.channelBuffers32[c];
            const float* i = c < in.numChannels ? in.channelBuffers32[c] : nullptr;
            for (int32 s = 0; s < n; ++s) o[s] = i ? gain * i[s] : 0.f;
        }
        return kResultOk;
    }
    uint32 PLUGIN_API getTailSamples() override { return 0; }

    // IEditController: one parameter, id 0, "crash", "latency" or "unused";
    // the params class has kParamsCount of them, ids 0 up, titled "#id".
    tresult PLUGIN_API setComponentState(IBStream*) override { return kResultOk; }
    int32 PLUGIN_API getParameterCount() override {
        return kind_ == Kind::Params ? kParamsCount : kind_ == Kind::Touch ? 2 : 1;
    }
    tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo& info) override {
        if (index < 0 || index >= getParameterCount()) return kInvalidArgument;
        std::memset(&info, 0, sizeof info);
        if (kind_ == Kind::Touch) {
            info.id = index == 0 ? kTouchLevel : kTouchMeter;
            copy16(info.title, index == 0 ? "level" : "meter");
            copy16(info.shortTitle, index == 0 ? "level" : "meter");
            copy16(info.units, "%");
            info.defaultNormalizedValue = index == 0 ? 1.0 : 0.0;
            info.flags = index == 0 ? ParameterInfo::kCanAutomate : ParameterInfo::kIsReadOnly;
            return kResultOk;
        }
        info.id = static_cast<ParamID>(index);
        const char* title = kind_ == Kind::Crash     ? "crash"
                            : kind_ == Kind::Latency ? "latency"
                            : kind_ == Kind::Params  ? "param"
                                                     : "unused";
        copy16(info.title, title);
        copy16(info.shortTitle, title);
        info.stepCount = 1;
        info.defaultNormalizedValue = 0;
        info.flags = ParameterInfo::kCanAutomate;
        return kResultOk;
    }
    tresult PLUGIN_API getParamStringByValue(ParamID, ParamValue v, String128 s) override {
        if (kind_ == Kind::Touch) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.1f", v * 100.0);
            copy16(s, buf);
            return kResultOk;
        }
        copy16(s, "");
        return kResultOk;
    }
    tresult PLUGIN_API getParamValueByString(ParamID, TChar*, ParamValue&) override { return kResultFalse; }
    ParamValue PLUGIN_API normalizedParamToPlain(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API plainParamToNormalized(ParamID, ParamValue v) override { return v; }
    ParamValue PLUGIN_API getParamNormalized(ParamID id) override {
        if (kind_ == Kind::Touch) return id == kTouchLevel ? edited_.load() : 0.0;
        return trigger_;
    }
    tresult PLUGIN_API setParamNormalized(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API setComponentHandler(IComponentHandler* handler) override {
        if (kind_ != Kind::Latency && kind_ != Kind::Touch) return kResultOk;
        stop_notifier();
        handler_ = handler;
        if (handler_) notifier_ = std::thread([this] { kind_ == Kind::Touch ? touch_script() : notify_loop(); });
        return kResultOk;
    }
    IPlugView* PLUGIN_API createView(FIDString) override { return nullptr; }

private:
    ~TestEffect() { stop_notifier(); }

    // The latency class's own thread: it tells the host outside process(),
    // as VST3 asks, and a few milliseconds late, as LSP's sync thread does.
    void notify_loop() {
        while (!quit_.load(std::memory_order_acquire)) {
            if (notify_requested_.exchange(false, std::memory_order_acq_rel)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                handler_->restartComponent(kLatencyChanged);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // What an editor does when the operator drags the level knob, then
    // clicks it once, then pokes a meter, then grabs the knob and lets the
    // window close under the mouse. From a thread of the plugin's own, as an
    // editor's toolkit would.
    void touch_script() {
        for (int i = 0; i < 300 && !quit_.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (quit_.load(std::memory_order_acquire)) return;
        const auto edit = [&](ParamValue v) {
            edited_.store(v);
            handler_->performEdit(kTouchLevel, v);
        };
        handler_->beginEdit(kTouchLevel);
        edit(0.25);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        edit(0.5);
        edit(0.75);  // inside the coalescing interval: only the gesture end carries it
        handler_->endEdit(kTouchLevel);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        edit(0.5);  // no gesture: the host wraps it in one
        handler_->performEdit(kTouchMeter, 1.0);  // read-only: no dense index
        handler_->beginEdit(kTouchLevel);
        edit(0.125);
        // ... and never an endEdit.
    }

    void stop_notifier() {
        if (!notifier_.joinable()) return;
        quit_.store(true, std::memory_order_release);
        notifier_.join();
        quit_.store(false, std::memory_order_release);
    }

    Kind kind_;
    SpeakerArrangement arrangement_;
    double trigger_ = 0;
    std::atomic<uint32> refs_{1};
    // The latency class.
    std::atomic<uint32> latency_{0};
    std::atomic<bool> armed_{false}, notify_requested_{false}, quit_{false};
    std::thread::id process_thread_;
    IComponentHandler* handler_ = nullptr;
    std::thread notifier_;
    float delay_[2][kDelayRing]{};
    int32 write_ = 0;
    int activations_ = 0;  // the once class
    std::atomic<int64> blocks_{0};  // the state class
    float level_ = 1.f;             // the touch class: the processor's
    std::atomic<double> edited_{1.0};  // the touch class: the controller's
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
#ifdef SONORA_TEST_VST3_ONLY_KIND
    // One class per bundle, for the Windows builds the yabridge test runs:
    // a class that hangs a scan under Wine costs every class after it in
    // the same bundle (ADR-0017 Y4), so each one the test needs ships alone.
    int32 PLUGIN_API countClasses() override { return 1; }
#else
    int32 PLUGIN_API countClasses() override { return kKinds; }
#endif
    tresult PLUGIN_API getClassInfo(int32 index, PClassInfo* info) override {
        static const char* const names[kKinds] = {
            "Sonora Test Crash",    "Sonora Test Stereo Only", "Sonora Test Yes-Man", "Sonora Test Liar",
            "Sonora Test Turncoat", "Sonora Test Params",      "Sonora Test Latency",
            "Sonora Test Once",     "Sonora Test State",       "Sonora Test Touch"};
#ifdef SONORA_TEST_VST3_ONLY_KIND
        if (index != 0) return kInvalidArgument;
        index = SONORA_TEST_VST3_ONLY_KIND - 1;
#endif
        if (index < 0 || index >= kKinds) return kInvalidArgument;
        *info = PClassInfo();
        make_cid(static_cast<Kind>(index + 1), info->cid);
        info->cardinality = PClassInfo::kManyInstances;
        std::strcpy(info->category, kVstAudioEffectClass);
        std::strcpy(info->name, names[index]);
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(FIDString cid, FIDString iid, void** obj) override {
        *obj = nullptr;
        for (int k = 1; k <= kKinds; ++k) {
            TUID c;
            make_cid(static_cast<Kind>(k), c);
            if (!same(c, cid)) continue;
            if (const char* e = std::getenv("SONORA_TEST_VST3_HANG_ON_SCAN");
                e && *e == '1' && static_cast<Kind>(k) == Kind::Latency)
                for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
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

#ifdef SONORA_TEST_VST3_FAKE_CHAINLOADER
// The name yabridge's VST3 chainloader carries, so this build stands in for
// a bundle yabridgectl made (ADR-0017 Y2) without Wine on the test machine.
// It is not yabridge: it runs the plugin natively.
__attribute__((used)) const char kFakeChainloaderMark[] = "libyabridge-vst3.so";
#endif

// The same source also builds as a Windows .vst3 (mingw-w64), the plugin
// the real yabridge test runs through Wine (ADR-0017 Y5).
#ifdef _WIN32
#define SONORA_TEST_EXPORT __declspec(dllexport)
#else
#define SONORA_TEST_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

#ifdef _WIN32
SONORA_TEST_EXPORT bool InitDll() { return true; }
SONORA_TEST_EXPORT bool ExitDll() { return true; }
#else
SONORA_TEST_EXPORT bool ModuleEntry(void*) { return true; }
SONORA_TEST_EXPORT bool ModuleExit() { return true; }
#endif

SONORA_TEST_EXPORT IPluginFactory* PLUGIN_API GetPluginFactory() {
    if (const char* e = std::getenv("SONORA_TEST_VST3_CRASH_ON_SCAN"); e && *e == '1') {
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    }
    return &g_factory;
}

}  // extern "C"
