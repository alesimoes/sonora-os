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
//   --scenario late      (VST3) when a parameter change at a nonzero
//                        sample offset is heard: with DPF, at the start of
//                        the next block (ADR-0011 Amendment 2). Held as a
//                        test so that a DPF update that changes it, for
//                        better or worse, is seen and the ADR follows.
//
// Every scenario reads the bundle's layout (stereo or mono, with or without
// a key input) from the bundle itself: VST3 from its main bus, LV2 from
// --inputs and --outputs, the audio port counts CMake built it with.
//
// Linux only (dlopen); the bundles are built on Linux only. The VST3 ABI
// comes from DPF's own travesty headers and LV2 from the vendored LV2
// headers, so this needs nothing the sanitizer jobs do not have.
#include <dlfcn.h>

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

#include "lv2/atom.h"
#include "lv2/buf-size.h"
#include "lv2/lv2.h"
#include "lv2/options.h"
#include "lv2/urid.h"
#include "travesty/audio_processor.h"
#include "travesty/component.h"
#include "travesty/edit_controller.h"
#include "travesty/factory.h"

// Counts every operator new while armed. The executable exports it
// (ENABLE_EXPORTS), so the bundle, loaded later, binds to this one.
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

// --- LV2 --------------------------------------------------------------------

// The only feature run() might use is the map, and only at instantiate.
struct UridMap {
    std::map<std::string, LV2_URID> ids;
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
    const LV2_Feature* features[] = {&map_feature, &options_feature, nullptr};

    LV2_Handle h = desc->instantiate(desc, 48000.0, bundle, features);
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
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const char* vst3 = nullptr;
    const char* lv2 = nullptr;
    const char* bundle = nullptr;
    std::string scenario;
    uint32_t inputs = 2, outputs = 2;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--vst3") == 0) vst3 = argv[i + 1];
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
    else if (lv2 != nullptr && bundle != nullptr && scenario == "lazy") r = lv2_lazy(lv2, bundle, inputs, outputs);
    else
        std::fprintf(stderr, "usage: %s (--vst3 BINARY --scenario lazy|oversize|threads|late |"
                             " --lv2 BINARY --bundle DIR [--inputs N --outputs N] --scenario lazy)\n",
                     argv[0]);
    if (r == 0) std::printf("bundle_driver %s: pass\n", scenario.c_str());
    return r;
}
