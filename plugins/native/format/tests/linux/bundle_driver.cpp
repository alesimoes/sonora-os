// SPDX-License-Identifier: GPL-3.0-or-later
//
// Loads one of our bundle binaries the way a host does and drives it where
// the RT audit of PR #40 found DPF's wrappers exposed:
//
//   --scenario lazy      (VST3 and LV2) a host that processes an instance
//                        it never activated. DPF then activates it from
//                        inside process()/run(), on the audio thread;
//                        nothing may allocate there.
//   --scenario oversize  (VST3) a block longer than the max_block_size the
//                        host promised in setupProcessing(): refused with
//                        silence, never a write past DPF's dummy buffer
//                        (the ASan job sees one).
//   --scenario threads   (VST3) setParamNormalized() and
//                        getParamNormalized() from a second thread while
//                        process() runs, as Steinberg's validator and a UI
//                        thread do: no data race (the TSan job), and the
//                        last value set is what the plugin holds.
//   --scenario nokey     (VST3, a bundle with a key input) the external
//                        key switched on and the key bus left inactive, or
//                        not handed over at all: no key, so the processor
//                        keys from its own input and sounds as with the
//                        external key off. An active key bus carrying
//                        silence is a silent key, and sounds different.
//   --scenario wide      (VST3) a host that hands over one channel more
//                        than the bundle declares, on the key bus if it has
//                        one, else on the main bus: DPF's channel check
//                        fires inside process() and must print nothing
//                        (SONORA_DPF_QUIET, third_party/dpf/SONORA.md
//                        patch 6), since that is the audio thread.
//   --scenario late      (VST3) when a parameter change at a nonzero
//                        sample offset is heard: with DPF, at the start of
//                        the next block (ADR-0011 Amendment 2). Held as a
//                        test so that a DPF update that changes it, for
//                        better or worse, is seen and the ADR follows.
//   --scenario options   (LV2) options:set with a new block length and
//                        rate between two run() calls: prepared inside
//                        set(), nothing allocated in run(), and the sound of
//                        an instance instantiated with those options. Then
//                        deactivate() and activate() with nothing changed:
//                        no signal state carried over.
//   --scenario state     (LV2, a bundle with a file slot; --state 1 --uri U)
//                        the file the plugin state names: restore() with a
//                        mapped path loads it off the audio thread, save()
//                        writes the abstract path back, patch:Set goes
//                        through the Worker and loads nothing in run(), a
//                        missing file keeps the response, and nothing
//                        allocates in run() after a load.
//   --scenario setup     (VST3) setupProcessing() with a new rate and block
//                        size between two process() calls, on an active
//                        component and on one the host deactivated first,
//                        and again with the same values: the sound of a
//                        fresh instance, nothing allocated in process().
//
// The options and setup scenarios drive a loud burst before the change and
// compare what follows with a fresh instance, and show that the burst leaves
// state at the bundle's defaults (an instance that is not reset sounds
// different), so a missing reset() fails them.
//
// Every scenario reads the bundle's layout (stereo or mono, with or without
// a key input) from the bundle itself: VST3 from its main bus, LV2 from
// --inputs and --outputs, the audio port counts CMake built it with.
//
// Linux only (dlopen); the bundles are built on Linux only. The VST3 ABI
// comes from DPF's own travesty headers and LV2 from the vendored LV2
// headers, so this needs nothing the sanitizer jobs do not have.
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "lv2/atom-forge.h"
#include "lv2/atom.h"
#include "lv2/buf-size.h"
#include "lv2/lv2.h"
#include "lv2/options.h"
#include "lv2/parameters.h"
#include "lv2/patch.h"
#include "lv2/state.h"
#include "lv2/urid.h"
#include "lv2/worker.h"
#include "travesty/audio_processor.h"
#include "travesty/component.h"
#include "travesty/edit_controller.h"
#include "travesty/factory.h"
#include "sonora/plugin/telemetry.h"

// Counts every operator new while armed. The executable exports it
// (ENABLE_EXPORTS), so the bundle, loaded later, binds to this one. It sees
// operator new only: a direct malloc, calloc or realloc in the bundle goes
// past it. "Allocates nothing" here is a claim about operator new; the RTSan
// job, which runs these tests, is the guard for the rest.
namespace {
std::atomic<bool> g_counting{false};
std::atomic<long> g_allocations{0};
}  // namespace

void* operator new(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

constexpr int kBlocks = 64;

// --state 1: the bundle has plugin state (a file slot), which adds an event
// output port beside the event input (DISTRHO_LV2_USE_EVENTS_OUT).
bool g_state_ports = false;
bool g_memoryless = false;  // --memoryless 1, see carries_state()
// --telemetry 1: the processor publishes through native::Telemetry, so the
// host's hand-over (sonora_host_telemetry_take) must land in its block.
bool g_telemetry = false;
void* g_telemetry_offer = nullptr;  // what the host side offers the next instance

// --- VST3, through travesty ---------------------------------------------

// A parameter queue with one point, and a list of them: what a host hands
// process(). The object pointer DPF receives is the address of `vt`.
struct Queue {
    const v3_param_value_queue_cpp* vt;
    v3_param_id id;
    int32_t offset;
    double value;
};

v3_result V3_API no_interface(void*, const v3_tuid, void** obj) {
    *obj = nullptr;
    return V3_NO_INTERFACE;
}
uint32_t V3_API static_ref(void*) { return 1; }
v3_param_id V3_API queue_id(void* self) { return static_cast<Queue*>(self)->id; }
int32_t V3_API queue_count(void*) { return 1; }
v3_result V3_API queue_point(void* self, int32_t idx, int32_t* offset, double* value) {
    if (idx != 0) return V3_INVALID_ARG;
    *offset = static_cast<Queue*>(self)->offset;
    *value = static_cast<Queue*>(self)->value;
    return V3_OK;
}
v3_result V3_API queue_add(void*, int32_t, double, int32_t*) { return V3_NOT_IMPLEMENTED; }

v3_param_value_queue_cpp make_queue_vt() {
    v3_param_value_queue_cpp vt{};
    vt.query_interface = no_interface;
    vt.ref = static_ref;
    vt.unref = static_ref;
    vt.queue.get_param_id = queue_id;
    vt.queue.get_point_count = queue_count;
    vt.queue.get_point = queue_point;
    vt.queue.add_point = queue_add;
    return vt;
}
const v3_param_value_queue_cpp kQueueVt = make_queue_vt();

struct Changes {
    const v3_param_changes_cpp* vt;
    Queue* queues;
    int32_t count;
};

int32_t V3_API changes_count(void* self) { return static_cast<Changes*>(self)->count; }
v3_param_value_queue** V3_API changes_data(void* self, int32_t idx) {
    Changes* c = static_cast<Changes*>(self);
    if (idx < 0 || idx >= c->count) return nullptr;
    return reinterpret_cast<v3_param_value_queue**>(&c->queues[idx]);
}
// Output changes are not collected: DPF only reports latency through them.
v3_param_value_queue** V3_API changes_add(void*, const v3_param_id*, int32_t*) { return nullptr; }

v3_param_changes_cpp make_changes_vt() {
    v3_param_changes_cpp vt{};
    vt.query_interface = no_interface;
    vt.ref = static_ref;
    vt.unref = static_ref;
    vt.changes.get_param_count = changes_count;
    vt.changes.get_param_data = changes_data;
    vt.changes.add_param_data = changes_add;
    return vt;
}
const v3_param_changes_cpp kChangesVt = make_changes_vt();

struct Vst3 {
    void* lib = nullptr;
    v3_plugin_factory** factory = nullptr;
    v3_component** comp = nullptr;
    v3_audio_processor** proc = nullptr;
    v3_edit_controller** ctrl = nullptr;
    int32_t channels = 2;  // of the main buses, read in open()

    bool open(const char* path) {
        lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr) {
            std::fprintf(stderr, "dlopen %s: %s\n", path, dlerror());
            return false;
        }
        using Entry = bool (*)(void*);
        using GetFactory = const void* (*)();
        const auto entry = reinterpret_cast<Entry>(dlsym(lib, "ModuleEntry"));
        const auto get = reinterpret_cast<GetFactory>(dlsym(lib, "GetPluginFactory"));
        if (entry == nullptr || get == nullptr || !entry(lib)) return false;
        factory = static_cast<v3_plugin_factory**>(const_cast<void*>(get()));
        v3_class_info info{};
        if (factory == nullptr || v3_cpp_obj(factory)->get_class_info(factory, 0, &info) != V3_OK) return false;
        if (v3_cpp_obj(factory)->create_instance(factory, info.class_id, v3_component_iid,
                                                 reinterpret_cast<void**>(&comp)) != V3_OK)
            return false;
        if (v3_cpp_obj_initialize(comp, nullptr) != V3_OK) return false;
        if (v3_cpp_obj_query_interface(comp, v3_audio_processor_iid, &proc) != V3_OK) return false;
        if (v3_cpp_obj_query_interface(comp, v3_edit_controller_iid, &ctrl) != V3_OK) return false;
        v3_bus_info bi{};
        if (v3_cpp_obj(comp)->get_bus_info(comp, V3_AUDIO, V3_INPUT, 0, &bi) != V3_OK) return false;
        channels = bi.channel_count;
        v3_cpp_obj(comp)->activate_bus(comp, V3_AUDIO, V3_INPUT, 0, true);
        v3_cpp_obj(comp)->activate_bus(comp, V3_AUDIO, V3_OUTPUT, 0, true);
        return true;
    }

    bool setup(double rate, int32_t max_block) {
        v3_process_setup s{};
        s.process_mode = V3_REALTIME;
        s.symbolic_sample_size = V3_SAMPLE_32;
        s.max_block_size = max_block;
        s.sample_rate = rate;
        return v3_cpp_obj(proc)->setup_processing(proc, &s) == V3_OK;
    }

    // The ParamID of the bypass, and of the first continuous parameter.
    bool find_params(v3_param_id& bypass, v3_param_id& continuous) {
        bool got_bypass = false, got_cont = false;
        for (int32_t i = 0, n = v3_cpp_obj(ctrl)->get_parameter_count(ctrl); i < n; ++i) {
            v3_param_info pi{};
            if (v3_cpp_obj(ctrl)->get_parameter_info(ctrl, i, &pi) != V3_OK) continue;
            if (pi.flags & V3_PARAM_IS_BYPASS) {
                bypass = pi.param_id;
                got_bypass = true;
            } else if (!got_cont && (pi.flags & V3_PARAM_CAN_AUTOMATE) && !(pi.flags & V3_PARAM_READ_ONLY) &&
                       pi.step_count == 0) {
                continuous = pi.param_id;
                got_cont = true;
            }
        }
        return got_bypass && got_cont;
    }

    void close() {
        if (comp != nullptr) v3_cpp_obj(comp)->set_active(comp, false);
        if (ctrl != nullptr) v3_cpp_obj_unref(ctrl);
        if (proc != nullptr) v3_cpp_obj_unref(proc);
        if (comp != nullptr) {
            v3_cpp_obj_terminate(comp);
            v3_cpp_obj_unref(comp);
        }
        if (factory != nullptr) v3_cpp_obj_unref(factory);
        if (lib != nullptr) {
            using Exit = bool (*)();
            if (const auto exit_fn = reinterpret_cast<Exit>(dlsym(lib, "ModuleExit"))) exit_fn();
            dlclose(lib);
        }
    }
};

// `inputs` input and `outputs` output buffers, `frames` long, and a VST3
// process call over the first `vst3_channels` of each (the main buses).
struct Block {
    std::vector<std::vector<float>> in, out;
    std::vector<float*> ins, outs;
    v3_audio_bus_buffers in_bus{};
    v3_audio_bus_buffers out_bus{};
    Changes out_changes{&kChangesVt, nullptr, 0};

    Block(int32_t frames, int32_t inputs, int32_t outputs)
        : in(static_cast<std::size_t>(inputs), std::vector<float>(static_cast<std::size_t>(frames))),
          out(static_cast<std::size_t>(outputs), std::vector<float>(static_cast<std::size_t>(frames), 7.0f)) {
        for (std::size_t c = 0; c < in.size(); ++c) {
            for (int32_t i = 0; i < frames; ++i)
                in[c][static_cast<std::size_t>(i)] =
                    (c & 1 ? -0.25f : 0.25f) * std::sin(0.01f * static_cast<float>(i));
            ins.push_back(in[c].data());
        }
        for (auto& o : out) outs.push_back(o.data());
        in_bus.num_channels = outputs;
        in_bus.channel_buffers_32 = ins.data();
        out_bus.num_channels = outputs;
        out_bus.channel_buffers_32 = outs.data();
    }
    // A VST3 block: main buses only, as wide as the plugin's.
    Block(int32_t frames, const struct Vst3& p);

    v3_result run(Vst3& p, int32_t frames, Changes* params) {
        v3_process_data d{};
        d.process_mode = V3_REALTIME;
        d.symbolic_sample_size = V3_SAMPLE_32;
        d.nframes = frames;
        d.num_input_buses = 1;
        d.num_output_buses = 1;
        d.inputs = &in_bus;
        d.outputs = &out_bus;
        d.input_params = reinterpret_cast<v3_param_changes**>(params);
        d.output_params = reinterpret_cast<v3_param_changes**>(&out_changes);
        return v3_cpp_obj(p.proc)->process(p.proc, &d);
    }

    bool finite() const {
        for (const auto& o : out) {
            for (float v : o) {
                if (!std::isfinite(v)) return false;
            }
        }
        return true;
    }
};

Block::Block(int32_t frames, const Vst3& p) : Block(frames, p.channels, p.channels) {}

// Block's input at another level. kQuiet is Block's own, so an instance fed
// nothing else hears exactly what a fresh one hears; kLoud is the burst whose
// tail (a delay line, a reverb, an envelope, a filter's memory) the options
// and setup scenarios expect a reset to clear.
constexpr float kQuiet = 0.25f;
constexpr float kLoud = 0.9f;
void drive(Block& b, float gain) {
    for (std::size_t c = 0; c < b.in.size(); ++c) {
        for (std::size_t i = 0; i < b.in[c].size(); ++i)
            b.in[c][i] = (c & 1 ? -gain : gain) * std::sin(0.01f * static_cast<float>(i));
    }
}

// Bit for bit, and finite. Says where the first difference is.
bool same_render(const std::vector<float>& got, const std::vector<float>& want, const char* what) {
    if (got.empty() || got.size() != want.size()) {
        std::fprintf(stderr, "%s: %zu values rendered, want %zu\n", what, got.size(), want.size());
        return false;
    }
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (std::isfinite(got[i]) && got[i] == want[i]) continue;
        std::fprintf(stderr, "%s: differs from a fresh instance at value %zu: %.9g, want %.9g\n", what, i,
                     static_cast<double>(got[i]), static_cast<double>(want[i]));
        return false;
    }
    return true;
}

// The burst left state that a fresh instance does not have: without this a
// comparison with a fresh instance proves nothing about reset().
bool carries_state(const std::vector<float>& kept, const std::vector<float>& fresh, const char* what) {
    // --memoryless 1: the bundle's default sound has no signal memory (the
    // cabinet with no response loaded is a unit impulse), so a burst leaves
    // nothing for a reset to clear and the comparison cannot show one. The
    // processor's own suite tests reset() with a response loaded.
    if (g_memoryless) return !kept.empty() && kept.size() == fresh.size();
    std::size_t differ = 0;
    for (std::size_t i = 0; i < kept.size() && i < fresh.size(); ++i) differ += kept[i] != fresh[i];
    std::printf("%s: %zu of %zu value(s) differ from a fresh instance without a reset\n", what, differ,
                fresh.size());
    return !kept.empty() && kept.size() == fresh.size() && differ > 0;
}

int vst3_lazy(const char* path) {
    Vst3 p;
    CHECK(p.open(path));
    // The host's rate and block size reach the instance here, on the main
    // thread; it never calls setActive().
    CHECK(p.setup(48000.0, 256));
    v3_param_id bypass = 0, cont = 0;
    CHECK(p.find_params(bypass, cont));
    Block b(256, p);
    Queue q{&kQueueVt, cont, 0, 0.3};
    Changes params{&kChangesVt, &q, 1};
    bool ok = true;
    g_allocations.store(0);
    g_counting.store(true);
    for (int k = 0; k < kBlocks; ++k) {
        // A parameter change in the first block, then plain blocks.
        ok = ok && b.run(p, 256, k == 0 ? &params : nullptr) == V3_OK;
    }
    g_counting.store(false);
    const long allocations = g_allocations.load();
    p.close();
    if (allocations != 0) std::fprintf(stderr, "%ld allocations inside process()\n", allocations);
    CHECK(ok);
    CHECK(allocations == 0);
    CHECK(b.finite());
    return 0;
}

int vst3_oversize(const char* path) {
    Vst3 p;
    CHECK(p.open(path));
    CHECK(p.setup(48000.0, 128));
    CHECK(v3_cpp_obj(p.comp)->set_active(p.comp, true) == V3_OK);
    Block b(512, p);
    CHECK(b.run(p, 128, nullptr) == V3_OK);
    // Four times the promised block: refused, and the host's buffers are
    // silent rather than stale.
    for (auto& o : b.out) {
        for (float& v : o) v = 7.0f;
    }
    const v3_result r = b.run(p, 512, nullptr);
    p.close();
    CHECK(r != V3_OK);
    for (const auto& o : b.out) {
        for (float v : o) CHECK(v == 0.0f);
    }
    return 0;
}

int vst3_threads(const char* path) {
    Vst3 p;
    CHECK(p.open(path));
    CHECK(p.setup(48000.0, 128));
    CHECK(v3_cpp_obj(p.comp)->set_active(p.comp, true) == V3_OK);
    CHECK(v3_cpp_obj(p.proc)->set_processing(p.proc, true) == V3_OK);
    v3_param_id bypass = 0, cont = 0;
    CHECK(p.find_params(bypass, cont));

    std::atomic<bool> stop{false};
    std::thread controller([&] {
        // What a host's main thread and Steinberg's validator do: set and
        // read back, while the audio thread runs.
        for (int k = 0; !stop.load(std::memory_order_relaxed); ++k) {
            v3_cpp_obj(p.ctrl)->set_parameter_normalised(p.ctrl, cont, (k & 1) ? 0.25 : 0.75);
            v3_cpp_obj(p.ctrl)->set_parameter_normalised(p.ctrl, bypass, (k & 2) ? 1.0 : 0.0);
            (void)v3_cpp_obj(p.ctrl)->get_parameter_normalised(p.ctrl, cont);
            (void)v3_cpp_obj(p.ctrl)->get_parameter_normalised(p.ctrl, bypass);
        }
    });
    Block b(128, p);
    // The same parameters automated from the audio thread: a point at
    // offset 0 (applied before run()) and one later (applied after it).
    Queue qs[2] = {{&kQueueVt, cont, 0, 0.0}, {&kQueueVt, bypass, 64, 0.0}};
    Changes params{&kChangesVt, qs, 2};
    bool ok = true;
    for (int k = 0; k < 2000; ++k) {
        qs[0].value = (k & 1) ? 0.1 : 0.9;
        qs[1].value = (k & 4) ? 1.0 : 0.0;
        ok = ok && b.run(p, 128, &params) == V3_OK;
    }
    stop.store(true);
    controller.join();
    CHECK(ok);
    CHECK(b.finite());

    // Whatever the race left, a value set now is the value held, and
    // process() keeps it.
    CHECK(v3_cpp_obj(p.ctrl)->set_parameter_normalised(p.ctrl, cont, 0.6) == V3_OK);
    CHECK(v3_cpp_obj(p.ctrl)->set_parameter_normalised(p.ctrl, bypass, 1.0) == V3_OK);
    CHECK(b.run(p, 128, nullptr) == V3_OK);
    const double got = v3_cpp_obj(p.ctrl)->get_parameter_normalised(p.ctrl, cont);
    const double got_bypass = v3_cpp_obj(p.ctrl)->get_parameter_normalised(p.ctrl, bypass);
    p.close();
    CHECK(std::fabs(got - 0.6) < 1e-6);
    CHECK(got_bypass == 1.0);
    return 0;
}

// Renders `blocks` blocks of 128 through a fresh instance at 48 kHz, with
// `q` (if any) in block `at`; returns every output channel, end to end.
std::vector<float> vst3_render(const char* path, int blocks, const Queue* q, int at) {
    Vst3 p;
    std::vector<float> all;
    if (!p.open(path) || !p.setup(48000.0, 128) ||
        v3_cpp_obj(p.comp)->set_active(p.comp, true) != V3_OK ||
        v3_cpp_obj(p.proc)->set_processing(p.proc, true) != V3_OK) {
        p.close();
        return all;
    }
    Block b(128 * blocks, p);
    Block one(128, p);
    Queue copy{};
    if (q != nullptr) copy = *q;
    Changes params{&kChangesVt, &copy, 1};
    for (int k = 0; k < blocks; ++k) {
        for (std::size_t c = 0; c < one.in.size(); ++c)
            std::memcpy(one.in[c].data(), b.in[c].data() + 128 * k, 128 * sizeof(float));
        one.run(p, 128, q != nullptr && k == at ? &params : nullptr);
        for (const auto& o : one.out) all.insert(all.end(), o.begin(), o.end());
    }
    p.close();
    return all;
}

// ADR-0011 Amendment 2: DPF applies a queue's point at offset 0 before
// run() and its last point after run(), so a change at offset 64 of block k
// is not heard in block k and is heard from the first sample of block k+1,
// exactly as if it had been sent at offset 0 of block k+1. Shown on the
// first parameter whose change this bundle makes audible at the default
// settings (the bypass, else a continuous one), so it holds for every
// bundle without knowing its parameters.
int vst3_late(const char* path) {
    // Long enough for the slowest default to show a change: the gate's
    // 20 ms hold keeps it open for 960 samples after its threshold moves.
    constexpr int kBlocksLate = 40, kAt = 4;
    Vst3 p;
    CHECK(p.open(path));
    const int32_t chans = p.channels;
    std::vector<Queue> candidates;
    for (int32_t i = 0, n = v3_cpp_obj(p.ctrl)->get_parameter_count(p.ctrl); i < n; ++i) {
        v3_param_info pi{};
        if (v3_cpp_obj(p.ctrl)->get_parameter_info(p.ctrl, i, &pi) != V3_OK) continue;
        if (pi.flags & V3_PARAM_READ_ONLY) continue;
        if (!(pi.flags & V3_PARAM_IS_BYPASS) && (!(pi.flags & V3_PARAM_CAN_AUTOMATE) || pi.step_count != 0))
            continue;
        // The far end of the range from the default (the top from the
        // middle: the gate's -40 dB threshold moved to 0 dB closes it).
        const double to = pi.default_normalised_value <= 0.5 ? 1.0 : 0.0;
        candidates.push_back({&kQueueVt, pi.param_id, 64, to});
    }
    p.close();

    const std::vector<float> plain = vst3_render(path, kBlocksLate, nullptr, 0);
    CHECK(!plain.empty());
    const std::size_t block = 128 * static_cast<std::size_t>(chans);
    for (const Queue& q : candidates) {
        const std::vector<float> late = vst3_render(path, kBlocksLate, &q, kAt);
        CHECK(late.size() == plain.size());
        bool audible = false;
        for (std::size_t i = block * (kAt + 1); i < late.size(); ++i) audible |= late[i] != plain[i];
        if (!audible) continue;
        Queue at_zero = q;
        at_zero.offset = 0;
        const std::vector<float> next = vst3_render(path, kBlocksLate, &at_zero, kAt + 1);
        CHECK(next.size() == plain.size());
        // Block kAt: as if nothing had been sent.
        for (std::size_t i = 0; i < block * (kAt + 1); ++i) CHECK(late[i] == plain[i]);
        // From block kAt + 1: the same as the change sent at its offset 0.
        for (std::size_t i = block * (kAt + 1); i < late.size(); ++i) CHECK(late[i] == next[i]);
        std::printf("ParamID %u at offset 64 of block %d: heard from offset 0 of block %d\n",
                    static_cast<unsigned>(q.id), kAt, kAt + 1);
        return 0;
    }
    std::fprintf(stderr, "no parameter of this bundle changes its output at the defaults\n");
    return 1;
}

// Any number of buses per direction, each with its own channel count: what
// Block cannot express (a key bus, a bus wider than declared).
struct Buses {
    std::vector<std::vector<std::vector<float>>> data[2];  // [dir][bus][channel]
    std::vector<std::vector<float*>> ptrs[2];
    std::vector<v3_audio_bus_buffers> bufs[2];

    Buses(int32_t frames, const std::vector<int32_t>& ins, const std::vector<int32_t>& outs) {
        const std::vector<int32_t>* widths[2] = {&ins, &outs};
        for (int d = 0; d < 2; ++d) {
            const std::size_t n = widths[d]->size();
            data[d].resize(n);
            ptrs[d].resize(n);
            bufs[d].resize(n);
            for (std::size_t b = 0; b < n; ++b) {
                const auto ch = static_cast<std::size_t>((*widths[d])[b]);
                data[d][b].assign(ch, std::vector<float>(static_cast<std::size_t>(frames), 0.0f));
                for (auto& c : data[d][b]) ptrs[d][b].push_back(c.data());
                bufs[d][b].num_channels = (*widths[d])[b];
                bufs[d][b].channel_buffers_32 = ptrs[d][b].data();
            }
        }
    }

    v3_result run(Vst3& p, int32_t frames, Changes* params, int32_t in_buses) {
        Changes out_changes{&kChangesVt, nullptr, 0};
        v3_process_data d{};
        d.process_mode = V3_REALTIME;
        d.symbolic_sample_size = V3_SAMPLE_32;
        d.nframes = frames;
        d.num_input_buses = in_buses;
        d.num_output_buses = static_cast<int32_t>(bufs[1].size());
        d.inputs = bufs[0].data();
        d.outputs = bufs[1].data();
        d.input_params = reinterpret_cast<v3_param_changes**>(params);
        d.output_params = reinterpret_cast<v3_param_changes**>(&out_changes);
        return v3_cpp_obj(p.proc)->process(p.proc, &d);
    }
};

bool title_is(const v3_param_info& pi, const char* want) {
    std::size_t i = 0;
    for (; want[i] != '\0'; ++i) {
        if (i >= 127 || pi.title[i] != static_cast<int16_t>(want[i])) return false;
    }
    return pi.title[i] == 0;
}

// How a host hands over the key, for vst3_nokey.
enum class KeyBus { Inactive, Absent, ActiveSilent };

constexpr v3_param_id kNoParam = ~0u;

// kBlocks blocks of 128 at 48 kHz through a fresh instance, with the
// external key (`external`, a ParamID) switched on in the first block
// unless it is kNoParam. Returns the main outputs, end to end; empty on
// any failure.
std::vector<float> vst3_render_keyed(const char* path, v3_param_id external, KeyBus how) {
    Vst3 p;
    std::vector<float> all;
    v3_bus_info key{};
    if (!p.open(path) || !p.setup(48000.0, 128) ||
        v3_cpp_obj(p.comp)->get_bus_info(p.comp, V3_AUDIO, V3_INPUT, 1, &key) != V3_OK) {
        p.close();
        return all;
    }
    if (how == KeyBus::ActiveSilent) v3_cpp_obj(p.comp)->activate_bus(p.comp, V3_AUDIO, V3_INPUT, 1, true);
    if (v3_cpp_obj(p.comp)->set_active(p.comp, true) != V3_OK ||
        v3_cpp_obj(p.proc)->set_processing(p.proc, true) != V3_OK) {
        p.close();
        return all;
    }
    Buses b(128, {p.channels, key.channel_count}, {p.channels});
    Queue q{&kQueueVt, external, 0, 1.0};
    Changes params{&kChangesVt, &q, 1};
    for (int k = 0; k < kBlocks; ++k) {
        for (std::size_t c = 0; c < b.data[0][0].size(); ++c) {
            for (int i = 0; i < 128; ++i)
                // In phase, at two levels: a stereo-linked processor that
                // joins its channels' side chains (the opto) cancels
                // anti-phase channels and would not act on them at all.
                b.data[0][0][c][static_cast<std::size_t>(i)] =
                    (c & 1 ? 0.2f : 0.25f) * std::sin(0.01f * static_cast<float>(128 * k + i));
        }
        // An inactive bus still gets buffers from most hosts; whatever is in
        // them must not be heard. A silent active bus is zeros.
        for (auto& c : b.data[0][1]) std::fill(c.begin(), c.end(), how == KeyBus::ActiveSilent ? 0.0f : 0.5f);
        const bool first = k == 0 && external != kNoParam;
        if (b.run(p, 128, first ? &params : nullptr, how == KeyBus::Absent ? 1 : 2) != V3_OK) {
            all.clear();
            break;
        }
        for (const auto& o : b.data[1][0]) all.insert(all.end(), o.begin(), o.end());
    }
    p.close();
    return all;
}

int vst3_nokey(const char* path) {
    v3_param_id external = kNoParam;
    {
        Vst3 p;
        CHECK(p.open(path));
        CHECK(v3_cpp_obj(p.comp)->get_bus_count(p.comp, V3_AUDIO, V3_INPUT) == 2);
        for (int32_t i = 0, n = v3_cpp_obj(p.ctrl)->get_parameter_count(p.ctrl); i < n; ++i) {
            v3_param_info pi{};
            if (v3_cpp_obj(p.ctrl)->get_parameter_info(p.ctrl, i, &pi) == V3_OK && title_is(pi, "External key"))
                external = pi.param_id;
        }
        p.close();
    }
    CHECK(external != kNoParam);
    const std::vector<float> off = vst3_render_keyed(path, kNoParam, KeyBus::Inactive);
    const std::vector<float> inactive = vst3_render_keyed(path, external, KeyBus::Inactive);
    const std::vector<float> absent = vst3_render_keyed(path, external, KeyBus::Absent);
    const std::vector<float> silent = vst3_render_keyed(path, external, KeyBus::ActiveSilent);
    CHECK(!off.empty());
    CHECK(inactive.size() == off.size());
    CHECK(absent.size() == off.size());
    CHECK(silent.size() == off.size());
    float apart = 0.0f;
    std::size_t differ_inactive = 0, differ_absent = 0;
    for (std::size_t i = 0; i < off.size(); ++i) {
        differ_inactive += inactive[i] != off[i];
        differ_absent += absent[i] != off[i];
        apart = std::fmax(apart, std::fabs(silent[i] - off[i]));
    }
    std::printf("against the external key off: inactive key bus %zu sample(s) differ, no key bus %zu; "
                "an active silent key: max |difference| %g\n",
                differ_inactive, differ_absent, static_cast<double>(apart));
    CHECK(differ_inactive == 0);
    CHECK(differ_absent == 0);
    CHECK(apart > 1e-3f);
    return 0;
}

// Points fds 1 and 2 at a fresh temporary file until restore(), which says
// how many bytes were written there.
struct Capture {
    std::FILE* file = nullptr;
    int saved[2] = {-1, -1};

    bool start() {
        std::fflush(stdout);
        std::fflush(stderr);
        file = std::tmpfile();
        if (file == nullptr) return false;
        for (int fd = 1; fd <= 2; ++fd) {
            saved[fd - 1] = dup(fd);
            if (saved[fd - 1] < 0 || dup2(fileno(file), fd) < 0) return false;
        }
        return true;
    }
    long restore() {
        std::fflush(stdout);
        std::fflush(stderr);
        for (int fd = 1; fd <= 2; ++fd) {
            if (saved[fd - 1] >= 0) {
                dup2(saved[fd - 1], fd);
                ::close(saved[fd - 1]);
                saved[fd - 1] = -1;
            }
        }
        if (file == nullptr) return -1;
        const long n = static_cast<long>(lseek(fileno(file), 0, SEEK_END));
        std::fclose(file);
        file = nullptr;
        return n;
    }
};

int vst3_wide(const char* path) {
    // The capture itself must see a write, or "nothing printed" proves
    // nothing.
    {
        Capture probe;
        CHECK(probe.start());
        std::fprintf(stderr, "x");
        CHECK(probe.restore() == 1);
    }
    Vst3 p;
    CHECK(p.open(path));
    CHECK(p.setup(48000.0, 128));
    const int32_t declared = p.channels;
    const int32_t buses = v3_cpp_obj(p.comp)->get_bus_count(p.comp, V3_AUDIO, V3_INPUT);
    std::vector<int32_t> ins = {declared};
    if (buses > 1) {
        v3_bus_info key{};
        CHECK(v3_cpp_obj(p.comp)->get_bus_info(p.comp, V3_AUDIO, V3_INPUT, 1, &key) == V3_OK);
        v3_cpp_obj(p.comp)->activate_bus(p.comp, V3_AUDIO, V3_INPUT, 1, true);
        ins.push_back(key.channel_count + 1);
    } else {
        ins[0] += 1;
    }
    CHECK(v3_cpp_obj(p.comp)->set_active(p.comp, true) == V3_OK);
    CHECK(v3_cpp_obj(p.proc)->set_processing(p.proc, true) == V3_OK);
    Buses b(128, ins, {declared + 1});
    for (auto& bus : b.data[0]) {
        for (auto& c : bus) {
            for (std::size_t i = 0; i < c.size(); ++i) c[i] = 0.25f * std::sin(0.01f * static_cast<float>(i));
        }
    }
    Capture cap;
    CHECK(cap.start());
    bool ok = true;
    for (int k = 0; k < 4; ++k) ok = ok && b.run(p, 128, nullptr, static_cast<int32_t>(ins.size())) == V3_OK;
    const long printed = cap.restore();
    p.close();
    std::printf("input buses of %d%s channel(s) and an output of %d, the bundle declaring %d: "
                "%ld byte(s) printed\n",
                ins[0], ins.size() > 1 ? (" and " + std::to_string(ins[1])).c_str() : "", declared + 1,
                declared, printed);
    CHECK(ok);
    CHECK(printed == 0);
    for (const auto& c : b.data[1][0]) {
        for (float v : c) CHECK(std::isfinite(v));
    }
    return 0;
}

// How vst3_setup's instance reaches its second configuration.
enum class Change {
    None,
    // setupProcessing() on an active, processing component. The VST3 spec
    // asks for an inactive one; DPF deactivates and re-activates around it
    // (DistrhoPluginVST3.cpp, setupProcessing), and hosts do call it so.
    SetupActive,
    // What the spec asks: processing off, inactive, setup, active again.
    SetupInactive,
    // setActive(false) and setActive(true) with nothing changed.
    Reactivate,
};

// 128 blocks of 256 at 48 kHz, 0.68 s: longer than the delay's longest
// default time (500 ms), so its line is full of the burst at the change and
// an echo not cleared is heard at once, inside the short window compared
// after it. With 8 blocks the echo came after the window and a missing
// reset passed for the delay.
constexpr int kBurst = 128;
constexpr int kAfterChange = 16;

struct Vst3Render {
    std::vector<float> out;  // every main output, block by block
    uint32_t latency = 0;
    long allocations = 0;  // operator new inside process() after the change
};

// A fresh instance set up at (rate0, max0), active and processing: `burst`
// loud blocks of max0, the change `how` to (rate1, max1), then kAfterChange
// quiet blocks of max1. `out` is empty on any failure.
Vst3Render vst3_render_changed(const char* path, double rate0, int32_t max0, int burst, Change how,
                               double rate1, int32_t max1) {
    Vst3Render r;
    Vst3 p;
    const auto active = [&](bool on) {
        if (!on && v3_cpp_obj(p.proc)->set_processing(p.proc, false) != V3_OK) return false;
        if (v3_cpp_obj(p.comp)->set_active(p.comp, on) != V3_OK) return false;
        return !on || v3_cpp_obj(p.proc)->set_processing(p.proc, true) == V3_OK;
    };
    if (!p.open(path) || !p.setup(rate0, max0) || !active(true)) {
        p.close();
        return r;
    }
    Block b(max0 > max1 ? max0 : max1, p);
    drive(b, kLoud);
    bool ok = true;
    for (int k = 0; k < burst; ++k) ok = ok && b.run(p, max0, nullptr) == V3_OK;
    switch (how) {
    case Change::None: break;
    case Change::SetupActive: ok = ok && p.setup(rate1, max1); break;
    case Change::SetupInactive: ok = ok && active(false) && p.setup(rate1, max1) && active(true); break;
    case Change::Reactivate: ok = ok && active(false) && active(true); break;
    }
    drive(b, kQuiet);
    r.out.reserve(static_cast<std::size_t>(kAfterChange) * b.out.size() * static_cast<std::size_t>(max1));
    g_allocations.store(0);
    g_counting.store(true);
    for (int k = 0; ok && k < kAfterChange; ++k) {
        ok = b.run(p, max1, nullptr) == V3_OK;
        for (const auto& o : b.out) r.out.insert(r.out.end(), o.begin(), o.begin() + max1);
    }
    g_counting.store(false);
    r.allocations = g_allocations.load();
    r.latency = v3_cpp_obj(p.proc)->get_latency_samples(p.proc);
    p.close();
    if (!ok) r.out.clear();
    return r;
}

// --scenario setup (VST3): the counterpart of --scenario options. A host
// moves the rate and the block size with setupProcessing() between two
// process() calls. DPF re-prepares inside it, on the host's main thread
// (sampleRateChanged and bufferSizeChanged in plugin.cpp), so process()
// after it allocates nothing, and the instance sounds exactly like one set
// up with the new values from the start, the burst before the change
// forgotten. The same with the values unchanged, which reaches
// Bridge::activate's no-prepare path, where reset() alone clears the burst.
int vst3_setup(const char* path) {
    struct Case {
        const char* what;
        Change how;
        double rate;
        int32_t max;
    };
    const Case moved[] = {
        {"setupProcessing(44100, 128) while active", Change::SetupActive, 44100.0, 128},
        {"setupProcessing(44100, 128) while inactive", Change::SetupInactive, 44100.0, 128},
        {"setupProcessing(48000, 256) again while active", Change::SetupActive, 48000.0, 256},
        {"setActive(false), setActive(true)", Change::Reactivate, 48000.0, 256},
    };
    // One fresh render per configuration: creating an instance is most of
    // this test's time (about 0.25 s for the tube).
    const Vst3Render fresh44 = vst3_render_changed(path, 44100.0, 128, 0, Change::None, 44100.0, 128);
    const Vst3Render fresh = vst3_render_changed(path, 48000.0, 256, 0, Change::None, 48000.0, 256);
    for (const Case& c : moved) {
        const Vst3Render& want = c.rate == 48000.0 ? fresh : fresh44;
        const Vst3Render got = vst3_render_changed(path, 48000.0, 256, kBurst, c.how, c.rate, c.max);
        CHECK(!want.out.empty());
        if (got.allocations != 0)
            std::fprintf(stderr, "%s: %ld allocations inside process() after it\n", c.what, got.allocations);
        CHECK(got.allocations == 0);
        CHECK(same_render(got.out, want.out, c.what));
        CHECK(got.latency == want.latency);
        std::printf("%s: the sound and latency (%u) of a fresh instance\n", c.what, got.latency);
    }
    const Vst3Render kept = vst3_render_changed(path, 48000.0, 256, kBurst, Change::None, 48000.0, 256);
    CHECK(carries_state(kept.out, fresh.out, "the burst, not reset"));
    return 0;
}

// --- LV2 --------------------------------------------------------------------

// The only feature run() might use is the map, and only at instantiate.
struct UridMap {
    // Transparent, so that looking up a URI already mapped builds no
    // std::string: --scenario options counts the allocations around a
    // set() that calls this.
    std::map<std::string, LV2_URID, std::less<>> ids;
    static LV2_URID map(LV2_URID_Map_Handle h, const char* uri) {
        auto& ids = static_cast<UridMap*>(h)->ids;
        const auto it = ids.find(uri);
        if (it != ids.end()) return it->second;
        const LV2_URID id = static_cast<LV2_URID>(ids.size() + 1);
        ids.emplace(uri, id);
        return id;
    }
};

int lv2_lazy(const char* path, const char* bundle, uint32_t inputs, uint32_t outputs) {
    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
        std::fprintf(stderr, "dlopen %s: %s\n", path, dlerror());
        return 1;
    }
    using Get = const LV2_Descriptor* (*)(uint32_t);
    const auto get = reinterpret_cast<Get>(dlsym(lib, "lv2_descriptor"));
    CHECK(get != nullptr);
    const LV2_Descriptor* desc = get(0);
    CHECK(desc != nullptr);

    UridMap urids;
    LV2_URID_Map map{&urids, UridMap::map};
    const int32_t max_block = 256;
    const LV2_URID atom_int = UridMap::map(&urids, LV2_ATOM__Int);
    const LV2_Options_Option options[] = {
        {LV2_OPTIONS_INSTANCE, 0, UridMap::map(&urids, LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t),
         atom_int, &max_block},
        {LV2_OPTIONS_INSTANCE, 0, UridMap::map(&urids, LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t),
         atom_int, &max_block},
        {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
    };
    const LV2_Feature map_feature{LV2_URID__map, &map};
    const LV2_Feature options_feature{LV2_OPTIONS__options, const_cast<LV2_Options_Option*>(options)};
    // A bundle with plugin state requires the worker feature (DPF
    // instantiates nothing without it); nothing is ever scheduled here.
    const LV2_Worker_Schedule schedule{nullptr, [](LV2_Worker_Schedule_Handle, uint32_t, const void*) {
                                           return LV2_WORKER_SUCCESS;
                                       }};
    const LV2_Feature worker_feature{LV2_WORKER__schedule, const_cast<LV2_Worker_Schedule*>(&schedule)};
    std::vector<const LV2_Feature*> features{&map_feature, &options_feature};
    if (g_state_ports) features.push_back(&worker_feature);
    features.push_back(nullptr);

    // What sonora-plugin-host does around instantiating a plugin (plugins/host
    // loader_linux.cpp): offer the insert's block, which the plugin pulls
    // through sonora_host_telemetry_take() (defined below, exported).
    sonora::plugin::TelemetryBlock tel{};
    g_telemetry_offer = &tel;
    LV2_Handle h = desc->instantiate(desc, 48000.0, bundle, features.data());
    g_telemetry_offer = nullptr;
    // The bundle exports nothing but lv2_descriptor (lv2lint): no side door.
    CHECK(dlsym(lib, "sonora_native_telemetry_bind") == nullptr);
    CHECK(h != nullptr);
    Block b(max_block, static_cast<int32_t>(inputs), static_cast<int32_t>(outputs));
    // DPF's LV2 port order: the audio inputs (the key input's last), the
    // audio outputs, then one port that is the transport input in a bundle
    // that follows the host tempo and the latency output otherwise: an
    // empty atom sequence serves as either. Control ports stay
    // unconnected, which DPF reads as "unchanged".
    alignas(8) std::uint8_t extra[64] = {};
    LV2_Atom_Sequence* seq = reinterpret_cast<LV2_Atom_Sequence*>(extra);
    seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
    seq->atom.type = UridMap::map(&urids, LV2_ATOM__Sequence);
    uint32_t port = 0;
    for (float* in : b.ins) desc->connect_port(h, port++, in);
    for (float* out : b.outs) desc->connect_port(h, port++, out);
    desc->connect_port(h, port, extra);
    // A bundle with plugin state (--state 1) has an event output and then
    // the latency after the event input; DPF reads the input in every run().
    alignas(8) std::uint8_t events_out[1024] = {};
    float latency_out = 0.0f;
    if (g_state_ports) {
        desc->connect_port(h, port + 1, events_out);
        desc->connect_port(h, port + 2, &latency_out);
    }

    g_allocations.store(0);
    g_counting.store(true);
    for (int k = 0; k < kBlocks; ++k) desc->run(h, static_cast<uint32_t>(max_block));
    g_counting.store(false);
    const long allocations = g_allocations.load();
    desc->cleanup(h);
    dlclose(lib);
    if (allocations != 0) std::fprintf(stderr, "%ld allocations inside run()\n", allocations);
    CHECK(allocations == 0);
    CHECK(b.finite());
    sonora::plugin::TelemetrySnapshot snap;
    const bool read = tel.read(snap);
    if (g_telemetry) {
        // Frames reached the engine's side of the block, with at least one
        // meter published, and no value that was not finite.
        CHECK(read && snap.sequence > 0 && snap.meter_mask != 0);
        CHECK(tel.rejected.load() == 0);
    } else {
        CHECK(!read);
    }
    return 0;
}

// The host side of LV2 state with files (ADR-0029 §6): state:mapPath and
// freePath over a made-up session folder, and a worker queue that the test
// runs by hand, on "another thread" (the test's own).
struct StateHost {
    std::string dir;  // the session folder; "files/<name>" abstracts <dir>/<name>
    std::vector<std::vector<std::uint8_t>> queued;

    static char* abstract_path(LV2_State_Map_Path_Handle h, const char* abs) {
        const std::string prefix = static_cast<StateHost*>(h)->dir + "/";
        const std::string a(abs);
        const std::string r = a.compare(0, prefix.size(), prefix) == 0 ? "files/" + a.substr(prefix.size()) : a;
        return strdup(r.c_str());
    }
    static char* absolute_path(LV2_State_Map_Path_Handle h, const char* rel) {
        const std::string r(rel);
        const std::string prefix = "files/";
        return strdup((r.compare(0, prefix.size(), prefix) == 0
                           ? static_cast<StateHost*>(h)->dir + "/" + r.substr(prefix.size())
                           : r).c_str());
    }
    static void free_path(LV2_State_Free_Path_Handle, char* p) { std::free(p); }
    static LV2_Worker_Status schedule_work(LV2_Worker_Schedule_Handle h, uint32_t size, const void* data) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        static_cast<StateHost*>(h)->queued.emplace_back(p, p + size);
        return LV2_WORKER_SUCCESS;
    }

    LV2_State_Map_Path map_path{this, abstract_path, absolute_path};
    LV2_State_Free_Path free_path_feature{this, free_path};
    LV2_Worker_Schedule schedule{this, schedule_work};
    LV2_Feature f_map{LV2_STATE__mapPath, &map_path};
    LV2_Feature f_free{LV2_STATE__freePath, &free_path_feature};
    LV2_Feature f_work{LV2_WORKER__schedule, &schedule};
};

// One LV2 instance, instantiated with a rate, a max block length and a
// nominal one, and activated: what a host has before it starts running.
struct Lv2Instance {
    void* lib = nullptr;
    const LV2_Descriptor* desc = nullptr;
    LV2_Handle h = nullptr;
    const LV2_Options_Interface* opts = nullptr;
    UridMap urids;
    LV2_URID_Map map{&urids, UridMap::map};
    int32_t max_block, nominal;
    Block b;
    // The event input; --scenario state forges patch:Set messages into it.
    alignas(8) std::uint8_t extra[512] = {};
    alignas(8) std::uint8_t events_out[1024] = {};
    float latency_out = 0.0f;
    StateHost own_host;

    Lv2Instance(const char* path, const char* bundle, uint32_t inputs, uint32_t outputs, double rate,
                int32_t max, int32_t nom, StateHost* host = nullptr)
        : max_block(max), nominal(nom), b(max, static_cast<int32_t>(inputs), static_cast<int32_t>(outputs)) {
        lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (lib == nullptr) {
            std::fprintf(stderr, "dlopen %s: %s\n", path, dlerror());
            return;
        }
        using Get = const LV2_Descriptor* (*)(uint32_t);
        const auto get = reinterpret_cast<Get>(dlsym(lib, "lv2_descriptor"));
        desc = get != nullptr ? get(0) : nullptr;
        if (desc == nullptr) return;
        const LV2_URID atom_int = UridMap::map(&urids, LV2_ATOM__Int);
        // Mapped now, so that set() maps nothing new: the allocation count
        // around it is then the plugin's alone.
        UridMap::map(&urids, LV2_ATOM__Float);
        UridMap::map(&urids, LV2_PARAMETERS__sampleRate);
        const LV2_Options_Option options[] = {
            {LV2_OPTIONS_INSTANCE, 0, UridMap::map(&urids, LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t),
             atom_int, &max_block},
            {LV2_OPTIONS_INSTANCE, 0, UridMap::map(&urids, LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t),
             atom_int, &nominal},
            {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
        };
        const LV2_Feature map_feature{LV2_URID__map, &map};
        const LV2_Feature options_feature{LV2_OPTIONS__options, const_cast<LV2_Options_Option*>(options)};
        std::vector<const LV2_Feature*> features{&map_feature, &options_feature};
        if (host == nullptr && g_state_ports) host = &own_host;  // the worker feature is required
        if (host != nullptr) {
            features.push_back(&host->f_map);
            features.push_back(&host->f_free);
            features.push_back(&host->f_work);
        }
        features.push_back(nullptr);
        h = desc->instantiate(desc, rate, bundle, features.data());
        if (h == nullptr) return;
        if (desc->extension_data != nullptr)
            opts = static_cast<const LV2_Options_Interface*>(desc->extension_data(LV2_OPTIONS__interface));
        // The port order of lv2_lazy above.
        LV2_Atom_Sequence* seq = reinterpret_cast<LV2_Atom_Sequence*>(extra);
        seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
        seq->atom.type = UridMap::map(&urids, LV2_ATOM__Sequence);
        uint32_t port = 0;
        for (float* in : b.ins) desc->connect_port(h, port++, in);
        for (float* out : b.outs) desc->connect_port(h, port++, out);
        desc->connect_port(h, port, extra);
        if (g_state_ports) {
            desc->connect_port(h, port + 1, events_out);
            desc->connect_port(h, port + 2, &latency_out);
        }
        if (desc->activate != nullptr) desc->activate(h);
    }
    ~Lv2Instance() {
        if (h != nullptr) {
            if (desc->deactivate != nullptr) desc->deactivate(h);
            desc->cleanup(h);
        }
        if (lib != nullptr) dlclose(lib);
    }
    Lv2Instance(const Lv2Instance&) = delete;
    Lv2Instance& operator=(const Lv2Instance&) = delete;
    bool ok() const {
        return h != nullptr && opts != nullptr && opts->set != nullptr && desc->activate != nullptr &&
               desc->deactivate != nullptr;
    }

    // A host stopping and restarting the instance, nothing changed.
    void reactivate() {
        desc->deactivate(h);
        desc->activate(h);
    }

    // What Ardour (Session::set_block_size) and Carla (bufferSizeChanged,
    // sampleRateChanged) send: the new nominal block length and rate.
    uint32_t set(int32_t new_nominal, float new_rate) {
        nominal = new_nominal;
        rate_ = new_rate;
        const LV2_Options_Option options[] = {
            {LV2_OPTIONS_INSTANCE, 0, UridMap::map(&urids, LV2_BUF_SIZE__nominalBlockLength), sizeof(int32_t),
             UridMap::map(&urids, LV2_ATOM__Int), &nominal},
            {LV2_OPTIONS_INSTANCE, 0, UridMap::map(&urids, LV2_PARAMETERS__sampleRate), sizeof(float),
             UridMap::map(&urids, LV2_ATOM__Float), &rate_},
            {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr},
        };
        return opts->set(h, options);
    }

    // Runs `blocks` full blocks and appends the outputs, channel-interleaved
    // block by block, to `rendered`.
    void run(int blocks, std::vector<float>* rendered) {
        for (int k = 0; k < blocks; ++k) {
            desc->run(h, static_cast<uint32_t>(max_block));
            if (rendered == nullptr) continue;
            for (const auto& c : b.out) rendered->insert(rendered->end(), c.begin(), c.end());
        }
    }

private:
    float rate_ = 0.0f;
};

// --scenario options (LV2): the host changes the block length and the rate
// through the options interface (options:set) while the instance is
// active, between two run() calls. The LV2 options interface puts set() in
// the "instantiation" threading class (lv2/options.h): never concurrent
// with run(). Ardour calls it from Session::set_block_size, which its
// engine guarantees is never inside process(); Carla from its engine's
// buffer-size and rate callbacks (JACK delivers those on its non-RT
// notification thread); our host never calls it. So DPF re-preparing
// there, at once (DistrhoPluginLV2.cpp lv2_set_options, setBufferSize(n,
// true)), is on a thread that may allocate, and deferring the change to the
// next activate() would be worse: DPF reaches activate() lazily from inside
// run() (--scenario lazy). This holds what that leaves to check:
// - options that repeat the current ones cost nothing (no allocation);
// - run() after a real change allocates nothing: the change was prepared
//   inside set();
// - the instance then sounds exactly like one instantiated with the new
//   options, even with a nominal length under the blocks the host runs
//   (nominal is only "typical", buf-size.ttl; the max still holds), the
//   loud burst before set() forgotten;
// - deactivate() and activate() with nothing changed forget it too.
int lv2_options(const char* path, const char* bundle, uint32_t inputs, uint32_t outputs) {
    constexpr int32_t kMax = 256;
    constexpr int32_t kNominalAfter = 64;
    constexpr float kRateAfter = 44100.0f;
    constexpr int kAfter = 16;

    Lv2Instance moved(path, bundle, inputs, outputs, 48000.0, kMax, kMax);
    CHECK(moved.ok());
    drive(moved.b, kLoud);
    moved.run(kBurst, nullptr);

    g_allocations.store(0);
    g_counting.store(true);
    const uint32_t same = moved.set(kMax, 48000.0f);
    g_counting.store(false);
    CHECK(same == LV2_OPTIONS_SUCCESS);
    if (g_allocations.load() != 0)
        std::fprintf(stderr, "%ld allocations in a set() that changes nothing\n", g_allocations.load());
    CHECK(g_allocations.load() == 0);

    CHECK(moved.set(kNominalAfter, kRateAfter) == LV2_OPTIONS_SUCCESS);
    drive(moved.b, kQuiet);
    std::vector<float> after;
    after.reserve(static_cast<std::size_t>(kAfter) * outputs * kMax);
    g_allocations.store(0);
    g_counting.store(true);
    moved.run(kAfter, &after);
    g_counting.store(false);
    if (g_allocations.load() != 0)
        std::fprintf(stderr, "%ld allocations inside run() after set()\n", g_allocations.load());
    CHECK(g_allocations.load() == 0);

    Lv2Instance fresh(path, bundle, inputs, outputs, kRateAfter, kMax, kNominalAfter);
    CHECK(fresh.ok());
    std::vector<float> want;
    fresh.run(kAfter, &want);

    CHECK(same_render(after, want, "options:set"));
    // The port after the audio: the latency output in a bundle that does not
    // follow the host tempo, which must then match too.
    CHECK(std::memcmp(moved.extra, fresh.extra, sizeof(float)) == 0);

    // deactivate() and activate() with the options unchanged: DPF calls
    // neither callback, so Bridge::activate takes its no-prepare path and
    // its reset() is all that clears the burst. `kept`, the burst without
    // the re-activation, shows the burst leaves state to clear.
    Lv2Instance again(path, bundle, inputs, outputs, 48000.0, kMax, kMax);
    Lv2Instance kept(path, bundle, inputs, outputs, 48000.0, kMax, kMax);
    Lv2Instance fresh48(path, bundle, inputs, outputs, 48000.0, kMax, kMax);
    CHECK(again.ok() && kept.ok() && fresh48.ok());
    drive(again.b, kLoud);
    drive(kept.b, kLoud);
    again.run(kBurst, nullptr);
    kept.run(kBurst, nullptr);
    g_allocations.store(0);
    g_counting.store(true);
    again.reactivate();
    g_counting.store(false);
    if (g_allocations.load() != 0)
        std::fprintf(stderr, "%ld allocations in an unchanged re-activation\n", g_allocations.load());
    CHECK(g_allocations.load() == 0);
    drive(again.b, kQuiet);
    drive(kept.b, kQuiet);
    std::vector<float> reactivated, carried, want48;
    again.run(kAfter, &reactivated);
    kept.run(kAfter, &carried);
    fresh48.run(kAfter, &want48);
    CHECK(same_render(reactivated, want48, "deactivate(), activate()"));
    CHECK(carries_state(carried, want48, "the burst, not reset"));
    return 0;
}

// --- --scenario state (LV2): a file slot carried in plugin state ----------

// What the host keeps of a saved state, and hands back on restore.
struct StateStore {
    struct Entry {
        std::string value;
        uint32_t type;
    };
    std::map<uint32_t, Entry> entries;
    static LV2_State_Status store(LV2_State_Handle h, uint32_t key, const void* value, size_t size,
                                  uint32_t type, uint32_t) {
        auto* s = static_cast<StateStore*>(h);
        s->entries[key] = {std::string(static_cast<const char*>(value), size), type};
        return LV2_STATE_SUCCESS;
    }
    static const void* retrieve(LV2_State_Handle h, uint32_t key, size_t* size, uint32_t* type, uint32_t* flags) {
        auto* s = static_cast<StateStore*>(h);
        const auto it = s->entries.find(key);
        if (it == s->entries.end()) return nullptr;
        *size = it->second.value.size();
        *type = it->second.type;
        *flags = LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE;
        return it->second.value.c_str();
    }
};

// A mono PCM16 WAV with one nonzero sample: an impulse response whose
// response (index, gain) the test can read back.
bool write_ir(const std::string& path, uint32_t rate, int index, int16_t value, int frames) {
    std::vector<std::uint8_t> w;
    auto p32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) w.push_back(static_cast<std::uint8_t>(v >> (8 * i))); };
    auto p16 = [&](uint16_t v) { w.push_back(static_cast<std::uint8_t>(v)); w.push_back(static_cast<std::uint8_t>(v >> 8)); };
    auto tag = [&](const char* t) { w.insert(w.end(), t, t + 4); };
    tag("RIFF"); p32(36 + static_cast<uint32_t>(frames) * 2); tag("WAVE");
    tag("fmt "); p32(16); p16(1); p16(1); p32(rate); p32(rate * 2); p16(2); p16(16);
    tag("data"); p32(static_cast<uint32_t>(frames) * 2);
    for (int i = 0; i < frames; ++i) p16(static_cast<uint16_t>(i == index ? value : 0));
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return false;
    const bool ok = std::fwrite(w.data(), 1, w.size(), f) == w.size();
    return std::fclose(f) == 0 && ok;
}

constexpr int kIrLen = 48;

// The first kIrLen samples of channel 0 for a unit impulse, after enough
// silent blocks for a response loaded before them to have been taken over
// (the audio thread takes it at a block boundary, then dips for 2 x 4 ms +
// one partition). With `count` the allocations in run() are checked.
bool measure_ir(Lv2Instance& in, std::vector<float>& ir, bool count) {
    for (auto& ch : in.b.in) std::fill(ch.begin(), ch.end(), 0.0f);
    const uint32_t frames = static_cast<uint32_t>(in.max_block);
    if (count) {
        g_allocations.store(0);
        g_counting.store(true);
    }
    for (int k = 0; k < 8; ++k) in.desc->run(in.h, frames);
    for (auto& ch : in.b.in) ch[0] = 1.0f;
    in.desc->run(in.h, frames);
    if (count) g_counting.store(false);
    for (auto& ch : in.b.in) ch[0] = 0.0f;
    if (count && g_allocations.load() != 0) {
        std::fprintf(stderr, "%ld allocations inside run() after a file was loaded\n", g_allocations.load());
        return false;
    }
    ir.assign(in.b.out[0].begin(), in.b.out[0].begin() + kIrLen);
    return in.b.finite();
}

bool is_response(const std::vector<float>& ir, int index, float gain, const char* what) {
    for (int i = 0; i < kIrLen; ++i) {
        const float want = i == index ? gain : 0.0f;
        if (std::fabs(ir[static_cast<std::size_t>(i)] - want) > 1e-3f) {
            std::fprintf(stderr, "%s: sample %d is %.6f, want %.6f\n", what, i,
                         static_cast<double>(ir[static_cast<std::size_t>(i)]), static_cast<double>(want));
            return false;
        }
    }
    return true;
}

// The patch:Set a host sends to write a filename-path property, as the first
// event of the input sequence.
void forge_patch_set(Lv2Instance& in, LV2_URID property, const std::string& path) {
    LV2_Atom_Forge forge;
    lv2_atom_forge_init(&forge, &in.map);
    lv2_atom_forge_set_buffer(&forge, in.extra, sizeof in.extra);
    LV2_Atom_Forge_Frame seq_frame, obj_frame;
    lv2_atom_forge_sequence_head(&forge, &seq_frame, 0);
    lv2_atom_forge_frame_time(&forge, 0);
    lv2_atom_forge_object(&forge, &obj_frame, 0, UridMap::map(&in.urids, LV2_PATCH__Set));
    lv2_atom_forge_key(&forge, UridMap::map(&in.urids, LV2_PATCH__property));
    lv2_atom_forge_urid(&forge, property);
    lv2_atom_forge_key(&forge, UridMap::map(&in.urids, LV2_PATCH__value));
    lv2_atom_forge_path(&forge, path.c_str(), static_cast<uint32_t>(path.size() + 1));
    lv2_atom_forge_pop(&forge, &obj_frame);
    lv2_atom_forge_pop(&forge, &seq_frame);
}

void empty_events(Lv2Instance& in) {
    auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(in.extra);
    seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
    seq->atom.type = UridMap::map(&in.urids, LV2_ATOM__Sequence);
}

// The bundle's response is the file its state names: restore() puts it in
// (through state:mapPath, off the audio thread), save() writes the path back
// abstracted, a patch:Set reaches setState() through the Worker and never
// loads a file inside run(), and a file that cannot be read leaves the
// previous response. Mapped path and file are the host's (StateHost).
int lv2_state(const char* path, const char* bundle, uint32_t inputs, uint32_t outputs, const char* uri) {
    CHECK(uri != nullptr && g_state_ports);
    char tmpl[] = "/tmp/sonora_state_XXXXXX";
    CHECK(mkdtemp(tmpl) != nullptr);
    StateHost host;
    host.dir = tmpl;
    CHECK(write_ir(host.dir + "/a.wav", 48000, 10, 16384, 40));    // 0.5 at 10
    CHECK(write_ir(host.dir + "/b.wav", 48000, 20, 8192, 40));     // 0.25 at 20

    constexpr int32_t kMax = 256;
    Lv2Instance a(path, bundle, inputs, outputs, 48000.0, kMax, kMax, &host);
    CHECK(a.h != nullptr && a.desc->extension_data != nullptr);
    const auto* state_if = static_cast<const LV2_State_Interface*>(a.desc->extension_data(LV2_STATE__interface));
    const auto* worker_if = static_cast<const LV2_Worker_Interface*>(a.desc->extension_data(LV2_WORKER__interface));
    CHECK(state_if != nullptr && state_if->save != nullptr && state_if->restore != nullptr);
    CHECK(worker_if != nullptr && worker_if->work != nullptr);
    const LV2_Feature* const state_features[] = {&host.f_map, &host.f_free, nullptr};
    const LV2_URID key = UridMap::map(&a.urids, (std::string(uri) + "#ir").c_str());
    const LV2_URID atom_path = UridMap::map(&a.urids, LV2_ATOM__Path);
    drive(a.b, 0.0f);

    std::vector<float> ir;
    // Nothing loaded: the unit impulse.
    CHECK(measure_ir(a, ir, false));
    CHECK(is_response(ir, 0, 1.0f, "no file"));

    // restore(): the host hands back the abstract path of a.wav.
    StateStore saved;
    saved.entries[key] = {"files/a.wav", atom_path};
    CHECK(state_if->restore(a.h, StateStore::retrieve, &saved, 0, state_features) == LV2_STATE_SUCCESS);
    CHECK(measure_ir(a, ir, true));
    CHECK(is_response(ir, 10, 0.5f, "restored a.wav"));

    // save(): the same abstract path, typed atom:Path, not the absolute one.
    StateStore out;
    CHECK(state_if->save(a.h, StateStore::store, &out, 0, state_features) == LV2_STATE_SUCCESS);
    CHECK(out.entries.count(key) == 1);
    CHECK(out.entries[key].type == atom_path);
    CHECK(std::string(out.entries[key].value.c_str()) == "files/a.wav");

    // A host that writes the property: patch:Set in the event input is only
    // queued to the Worker by run(), and the file is read in work().
    forge_patch_set(a, key, host.dir + "/b.wav");
    a.desc->run(a.h, static_cast<uint32_t>(kMax));
    empty_events(a);
    CHECK(host.queued.size() == 1);
    CHECK(measure_ir(a, ir, false));
    CHECK(is_response(ir, 10, 0.5f, "before the worker ran"));
    const auto respond = [](LV2_Worker_Respond_Handle, uint32_t, const void*) { return LV2_WORKER_SUCCESS; };
    CHECK(worker_if->work(a.h, respond, nullptr, static_cast<uint32_t>(host.queued[0].size()),
                          host.queued[0].data()) == LV2_WORKER_SUCCESS);
    CHECK(measure_ir(a, ir, true));
    CHECK(is_response(ir, 20, 0.25f, "patch:Set b.wav"));

    // The save after a patch:Set names the new file, abstracted.
    StateStore out2;
    CHECK(state_if->save(a.h, StateStore::store, &out2, 0, state_features) == LV2_STATE_SUCCESS);
    CHECK(std::string(out2.entries[key].value.c_str()) == "files/b.wav");

    // A file that is gone: the previous response stays.
    StateStore gone;
    gone.entries[key] = {"files/missing.wav", atom_path};
    CHECK(state_if->restore(a.h, StateStore::retrieve, &gone, 0, state_features) == LV2_STATE_SUCCESS);
    CHECK(measure_ir(a, ir, true));
    CHECK(is_response(ir, 20, 0.25f, "missing file"));

    // A second instance restored from the saved state sounds the same.
    Lv2Instance c(path, bundle, inputs, outputs, 48000.0, kMax, kMax, &host);
    CHECK(c.h != nullptr);
    drive(c.b, 0.0f);
    CHECK(state_if->restore(c.h, StateStore::retrieve, &out2, 0, state_features) == LV2_STATE_SUCCESS);
    std::vector<float> ir_c;
    CHECK(measure_ir(c, ir_c, true));
    CHECK(same_render(ir_c, ir, "second instance from saved state"));

    for (const char* f : {"/a.wav", "/b.wav"}) std::remove((host.dir + f).c_str());
    rmdir(host.dir.c_str());
    return 0;
}

}  // namespace

// The host's half of the telemetry hand-over (plugins/host loader_linux.cpp). The
// driver is linked with ENABLE_EXPORTS so the bundle's dlsym(RTLD_DEFAULT) finds it.
extern "C" __attribute__((visibility("default"))) void* sonora_host_telemetry_take() {
    void* block = g_telemetry_offer;
    g_telemetry_offer = nullptr;
    return block;
}

int main(int argc, char** argv) {
    const char* vst3 = nullptr;
    const char* lv2 = nullptr;
    const char* bundle = nullptr;
    const char* uri = nullptr;
    std::string scenario;
    uint32_t inputs = 2, outputs = 2;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--state") == 0) g_state_ports = std::atoi(argv[i + 1]) != 0;
        else if (std::strcmp(argv[i], "--memoryless") == 0) g_memoryless = std::atoi(argv[i + 1]) != 0;
        else if (std::strcmp(argv[i], "--telemetry") == 0) g_telemetry = std::atoi(argv[i + 1]) != 0;
        else if (std::strcmp(argv[i], "--uri") == 0) uri = argv[i + 1];
        else if (std::strcmp(argv[i], "--vst3") == 0) vst3 = argv[i + 1];
        else if (std::strcmp(argv[i], "--lv2") == 0) lv2 = argv[i + 1];
        else if (std::strcmp(argv[i], "--bundle") == 0) bundle = argv[i + 1];
        else if (std::strcmp(argv[i], "--scenario") == 0) scenario = argv[i + 1];
        else if (std::strcmp(argv[i], "--inputs") == 0) inputs = static_cast<uint32_t>(std::atoi(argv[i + 1]));
        else if (std::strcmp(argv[i], "--outputs") == 0) outputs = static_cast<uint32_t>(std::atoi(argv[i + 1]));
    }
    int r = 2;
    if (vst3 != nullptr && scenario == "lazy") r = vst3_lazy(vst3);
    else if (vst3 != nullptr && scenario == "oversize") r = vst3_oversize(vst3);
    else if (vst3 != nullptr && scenario == "threads") r = vst3_threads(vst3);
    else if (vst3 != nullptr && scenario == "late") r = vst3_late(vst3);
    else if (vst3 != nullptr && scenario == "nokey") r = vst3_nokey(vst3);
    else if (vst3 != nullptr && scenario == "wide") r = vst3_wide(vst3);
    else if (vst3 != nullptr && scenario == "setup") r = vst3_setup(vst3);
    else if (lv2 != nullptr && bundle != nullptr && scenario == "lazy") r = lv2_lazy(lv2, bundle, inputs, outputs);
    else if (lv2 != nullptr && bundle != nullptr && scenario == "options")
        r = lv2_options(lv2, bundle, inputs, outputs);
    else if (lv2 != nullptr && bundle != nullptr && scenario == "state")
        r = lv2_state(lv2, bundle, inputs, outputs, uri);
    else
        std::fprintf(stderr, "usage: %s (--vst3 BINARY --scenario lazy|oversize|threads|late|nokey|wide|setup |"
                             " --lv2 BINARY --bundle DIR [--inputs N --outputs N] --scenario lazy|options|state [--state 1 --uri URI])\n",
                     argv[0]);
    if (r == 0) std::printf("bundle_driver %s: pass\n", scenario.c_str());
    return r;
}
