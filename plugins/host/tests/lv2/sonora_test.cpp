// SPDX-License-Identifier: GPL-3.0-or-later
//
// Misbehaving LV2 plugins for testing the host (ADR-0007 §7). Each one does
// on purpose what real plugins do by accident. Never installed; the test
// driver loads the bundle from the build tree.
//
//   urn:sonora:test#crash  mono pass-through; segfaults when `crash` > 0.5
//   urn:sonora:test#sleep  mono pass-through; sleeps `sleep_ms` per block
//   urn:sonora:test#liar   declares stereo, writes 8 channels into output 0
//   urn:sonora:test#nan    mono pass-through; writes NaN and Inf when `nan` > 0.5
//   urn:sonora:test#lookahead  mono, 100 frames of reported latency and a
//                          `gain` in dB, with no bypass port: what the host
//                          bypasses with its own delay line (ADR-0012 §5.1)
//   urn:sonora:test#key    mono, with a key input: outputs its key, then
//                          scribbles over the key's `n` samples
//   urn:sonora:test#keyliar  the same, and writes 64 samples past its key
//   urn:sonora:test#state  mono pass-through that counts its run() calls and
//                          keeps the count in its state (state:interface), so
//                          a restored instance is told apart from a fresh one
//   urn:sonora:test#worker  mono, `gain` (linear) applied only once its work
//                          comes back: run() schedules each new value
//                          (worker:schedule), work() sleeps 2 ms and responds,
//                          work_response() applies it. Silent for good if
//                          work() ever runs on the run() thread, a response
//                          arrives anywhere else, only one of work()'s two
//                          responses arrives, or a run() follows one without
//                          end_run(). Its state is that gain, restored
//                          through a schedule from restore(). A gain of
//                          exactly kHangGain makes work() never return, as
//                          a plugin stuck on a network share does; at
//                          kSlowGain it returns after kSlowWorkMs and then
//                          maps a URID, as a load finishing late does. At
//                          kFloodGain every run() also schedules
//                          kFloodMessages requests, more than the host's
//                          request ring holds, which work() takes 1 ms
//                          each over and leaves unanswered

#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>
#include <lv2/worker/worker.h>

#include <pthread.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>

#if defined(__has_feature)
#if __has_feature(realtime_sanitizer)
#include <sanitizer/rtsan_interface.h>
#define SONORA_TEST_RTSAN 1
#endif
#endif

namespace {

enum Kind { kCrash, kSleep, kLiar, kNan, kLookahead, kKey, kKeyLiar, kState, kWorker };

constexpr const char* kBlocksUri = "urn:sonora:test#blocks";
constexpr const char* kLoadedUri = "urn:sonora:test#loaded";

constexpr uint32_t kLookaheadFrames = 100;
constexpr uint32_t kLookaheadRing = 256;  // a power of two above the delay

struct Plugin {
    Kind kind;
    // SONORA_TEST_RTSAN_SLEEP set: the sleep is left to RTSan (see run()).
    bool rtsan_checks_sleep = false;
    const float* control = nullptr;
    const float* in[2] = {nullptr, nullptr};
    float* out[2] = {nullptr, nullptr};
    float* latency = nullptr;
    float ring[kLookaheadRing] = {};
    uint32_t write = 0;
    // Written through on purpose: what a plugin that uses its inputs as
    // scratch space does.
    float* key = nullptr;
    // The state plugin: run() counts, save() may read beside it.
    std::atomic<int64_t> blocks{0};
    LV2_URID blocks_urid = 0, long_urid = 0;
    // The worker plugin. `requested` and the pending response are run()'s
    // alone; `applied` and `restored` are also read by save().
    const LV2_Worker_Schedule* schedule = nullptr;
    float requested = 1.f;
    std::atomic<float> applied{1.f}, restored{1.f};
    uint32_t pending_kind = 0;
    float pending_value = 0.f;
    bool ended = true;  // end_run() since the last run()
    LV2_URID loaded_urid = 0, float_urid = 0;
    const LV2_URID_Map* map = nullptr;
    std::atomic<pthread_t> run_thread{};
    std::atomic<bool> run_seen{false}, broken{false};
};

// Read through a volatile so the compiler cannot prove the address is null
// and replace the store with a trap: the test wants a genuine SIGSEGV.
volatile std::uintptr_t g_nowhere = 0;

template <Kind K>
LV2_Handle instantiate(const LV2_Descriptor*, double, const char*, const LV2_Feature* const* features) {
    auto* p = new Plugin;
    p->kind = K;
    if (K == kState) {
        const LV2_URID_Map* map = nullptr;
        for (int i = 0; features && features[i]; ++i)
            if (std::strcmp(features[i]->URI, LV2_URID__map) == 0)
                map = static_cast<const LV2_URID_Map*>(features[i]->data);
        if (!map) {
            delete p;
            return nullptr;
        }
        p->blocks_urid = map->map(map->handle, kBlocksUri);
        p->long_urid = map->map(map->handle, LV2_ATOM__Long);
    }
    if (K == kWorker) {
        const LV2_URID_Map* map = nullptr;
        for (int i = 0; features && features[i]; ++i) {
            if (std::strcmp(features[i]->URI, LV2_WORKER__schedule) == 0)
                p->schedule = static_cast<const LV2_Worker_Schedule*>(features[i]->data);
            if (std::strcmp(features[i]->URI, LV2_URID__map) == 0)
                map = static_cast<const LV2_URID_Map*>(features[i]->data);
        }
        if (!p->schedule || !map) {
            delete p;
            return nullptr;
        }
        p->map = map;
        p->loaded_urid = map->map(map->handle, kLoadedUri);
        p->float_urid = map->map(map->handle, LV2_ATOM__Float);
    }
    p->rtsan_checks_sleep = std::getenv("SONORA_TEST_RTSAN_SLEEP") != nullptr;
    return p;
}

LV2_State_Status save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle handle, uint32_t,
                      const LV2_Feature* const*) {
    auto* p = static_cast<Plugin*>(h);
    const int64_t n = p->blocks.load(std::memory_order_relaxed);
    return store(handle, p->blocks_urid, &n, sizeof n, p->long_urid, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

LV2_State_Status restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle,
                         uint32_t, const LV2_Feature* const*) {
    auto* p = static_cast<Plugin*>(h);
    size_t size = 0;
    uint32_t type = 0, flags = 0;
    const void* v = retrieve(handle, p->blocks_urid, &size, &type, &flags);
    if (!v || size != sizeof(int64_t) || type != p->long_urid) return LV2_STATE_ERR_NO_PROPERTY;
    int64_t n;
    std::memcpy(&n, v, sizeof n);
    p->blocks.store(n, std::memory_order_relaxed);
    return LV2_STATE_SUCCESS;
}

const LV2_State_Interface kStateInterface = {save, restore};

const void* extension_data(const char* uri) {
    return std::strcmp(uri, LV2_STATE__interface) == 0 ? &kStateInterface : nullptr;
}

bool on_run_thread(const Plugin* p) {
    return p->run_seen.load(std::memory_order_acquire) &&
           pthread_equal(p->run_thread.load(std::memory_order_relaxed), pthread_self());
}

// What run() and restore() ask of the worker: a gain from the port ('G'),
// or the one a restored state names ('R'). The answer is two responses, the
// value and then a commit, so a host that keeps only one per work() call
// never applies anything.
struct WorkMsg {
    uint32_t kind;
    float value;
};
constexpr uint32_t kCommit = 'C';
constexpr float kHangGain = 0.25f;
constexpr float kSlowGain = 0.125f;
constexpr long kSlowWorkMs = 2600;
constexpr float kFloodGain = 0.0625f;
constexpr uint32_t kFlood = 'F';
constexpr uint32_t kFloodMessages = 1536;  // 18 KiB of requests a block, past a 16 KiB ring

// The slow part a real plugin does here: reading a sample or an IR.
LV2_Worker_Status work(LV2_Handle h, LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle rh,
                       uint32_t size, const void* data) {
    auto* p = static_cast<Plugin*>(h);
    if (size != sizeof(WorkMsg) || on_run_thread(p)) p->broken.store(true, std::memory_order_relaxed);
    WorkMsg m{};
    if (size == sizeof m) std::memcpy(&m, data, sizeof m);
    if (m.kind == kFlood) {
        // Slow enough that the ring stays full: a worker that drained it as
        // fast as run() fills it would refuse nothing.
        timespec f{0, 1'000'000};
        nanosleep(&f, nullptr);
        return LV2_WORKER_SUCCESS;
    }
    timespec ts{0, 2'000'000};
    if (m.value == kSlowGain) ts = {kSlowWorkMs / 1000, (kSlowWorkMs % 1000) * 1'000'000};
    do nanosleep(&ts, nullptr);
    while (m.value == kHangGain);
    if (m.value == kSlowGain) p->map->map(p->map->handle, kLoadedUri);
    if (LV2_Worker_Status s = respond(rh, size, data)) return s;
    return respond(rh, sizeof kCommit, &kCommit);
}

LV2_Worker_Status work_response(LV2_Handle h, uint32_t size, const void* data) {
    auto* p = static_cast<Plugin*>(h);
    bool ok = on_run_thread(p);
    if (ok && size == sizeof(WorkMsg) && p->pending_kind == 0) {
        WorkMsg m;
        std::memcpy(&m, data, sizeof m);
        p->pending_kind = m.kind;
        p->pending_value = m.value;
    } else if (ok && size == sizeof kCommit && p->pending_kind != 0) {
        (p->pending_kind == 'R' ? p->restored : p->applied).store(p->pending_value, std::memory_order_relaxed);
        p->pending_kind = 0;
    } else {
        ok = false;
    }
    if (!ok) p->broken.store(true, std::memory_order_relaxed);
    return ok ? LV2_WORKER_SUCCESS : LV2_WORKER_ERR_UNKNOWN;
}

LV2_Worker_Status end_run(LV2_Handle h) {
    static_cast<Plugin*>(h)->ended = true;
    return LV2_WORKER_SUCCESS;
}

const LV2_Worker_Interface kWorkerInterface = {work, work_response, end_run};

// The worker plugin's state is the gain its worker last applied. Restoring
// it goes through the worker too, as a sampler re-loads its sample: from
// the schedule feature restore() is handed (LV2 worker 1.2), answered in
// the first run().
LV2_State_Status worker_save(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle handle, uint32_t,
                             const LV2_Feature* const*) {
    auto* p = static_cast<Plugin*>(h);
    const float g = p->applied.load(std::memory_order_relaxed) * p->restored.load(std::memory_order_relaxed);
    return store(handle, p->loaded_urid, &g, sizeof g, p->float_urid, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

LV2_State_Status worker_restore(LV2_Handle h, LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle,
                                uint32_t, const LV2_Feature* const* features) {
    auto* p = static_cast<Plugin*>(h);
    size_t size = 0;
    uint32_t type = 0, flags = 0;
    const void* v = retrieve(handle, p->loaded_urid, &size, &type, &flags);
    if (!v || size != sizeof(float) || type != p->float_urid) return LV2_STATE_ERR_NO_PROPERTY;
    const LV2_Worker_Schedule* schedule = nullptr;
    for (int i = 0; features && features[i]; ++i)
        if (std::strcmp(features[i]->URI, LV2_WORKER__schedule) == 0)
            schedule = static_cast<const LV2_Worker_Schedule*>(features[i]->data);
    if (!schedule) return LV2_STATE_ERR_NO_FEATURE;
    WorkMsg m{'R', 0.f};
    std::memcpy(&m.value, v, sizeof m.value);
    return schedule->schedule_work(schedule->handle, sizeof m, &m) == LV2_WORKER_SUCCESS ? LV2_STATE_SUCCESS
                                                                                         : LV2_STATE_ERR_UNKNOWN;
}

const LV2_State_Interface kWorkerStateInterface = {worker_save, worker_restore};

const void* worker_extension_data(const char* uri) {
    if (std::strcmp(uri, LV2_WORKER__interface) == 0) return &kWorkerInterface;
    if (std::strcmp(uri, LV2_STATE__interface) == 0) return &kWorkerStateInterface;
    return nullptr;
}

void connect(LV2_Handle h, uint32_t port, void* data) {
    auto* p = static_cast<Plugin*>(h);
    if (p->kind == kLiar) {
        // 0, 1: in L/R. 2, 3: out L/R.
        if (port < 2) p->in[port] = static_cast<const float*>(data);
        else if (port < 4) p->out[port - 2] = static_cast<float*>(data);
        return;
    }
    if (p->kind == kState) {
        if (port == 0) p->in[0] = static_cast<const float*>(data);
        else if (port == 1) p->out[0] = static_cast<float*>(data);
        return;
    }
    if (p->kind == kKey || p->kind == kKeyLiar) {
        // 0: in. 1: key (lv2:isSideChain). 2: out.
        if (port == 0) p->in[0] = static_cast<const float*>(data);
        else if (port == 1) p->key = static_cast<float*>(data);
        else if (port == 2) p->out[0] = static_cast<float*>(data);
        return;
    }
    // 0: control. 1: in. 2: out. 3: latency (lookahead only).
    if (port == 0) p->control = static_cast<const float*>(data);
    else if (port == 1) p->in[0] = static_cast<const float*>(data);
    else if (port == 2) p->out[0] = static_cast<float*>(data);
    else if (port == 3) p->latency = static_cast<float*>(data);
}

void run(LV2_Handle h, uint32_t n) {
    auto* p = static_cast<Plugin*>(h);
    switch (p->kind) {
    case kCrash:
        if (p->control && *p->control > 0.5f)
            *reinterpret_cast<volatile int*>(g_nowhere) = 1;
        std::memcpy(p->out[0], p->in[0], n * sizeof(float));
        break;
    case kSleep:
        if (p->control && *p->control > 0.f) {
            const long ns = static_cast<long>(*p->control * 1e6f);
            timespec ts{ns / 1000000000L, ns % 1000000000L};
#ifdef SONORA_TEST_RTSAN
            // The overrun the deadline tests are about, made on purpose. The
            // host's block is a realtime context under RTSan, which would
            // halt the host here first and test RTSan instead of the
            // deadline; the host code around this run() stays checked.
            // plugin_host_rtsan_block turns the exemption off, and RTSan
            // must then report this sleep from the host's run_block.
            if (!p->rtsan_checks_sleep) {
                const __rtsan::ScopedDisabler the_overrun_is_the_test;
                nanosleep(&ts, nullptr);
            } else {
                nanosleep(&ts, nullptr);
            }
#else
            nanosleep(&ts, nullptr);
#endif
        }
        std::memcpy(p->out[0], p->in[0], n * sizeof(float));
        break;
    case kNan:
        std::memcpy(p->out[0], p->in[0], n * sizeof(float));
        // What a filter does when its state blows up: NaN, then Inf, on every
        // fourth sample, so the rest of the block is still the input.
        if (p->control && *p->control > 0.5f)
            for (uint32_t i = 0; i < n; i += 4)
                p->out[0][i] = (i & 4) ? std::numeric_limits<float>::infinity()
                                       : std::numeric_limits<float>::quiet_NaN();
        break;
    case kLookahead: {
        const float g = p->control ? std::pow(10.f, *p->control / 20.f) : 1.f;
        for (uint32_t i = 0; i < n; ++i) {
            p->ring[p->write & (kLookaheadRing - 1)] = p->in[0][i];
            p->out[0][i] = g * p->ring[(p->write - kLookaheadFrames) & (kLookaheadRing - 1)];
            ++p->write;
        }
        if (p->latency) *p->latency = float(kLookaheadFrames);
        break;
    }
    case kWorker: {
        // Every run(): a host may validate on one thread and process on
        // another; the audio class is whichever thread runs it now.
        p->run_thread.store(pthread_self(), std::memory_order_relaxed);
        p->run_seen.store(true, std::memory_order_release);
        // A host that offers end_run calls it after every run().
        if (!p->ended) p->broken.store(true, std::memory_order_relaxed);
        p->ended = false;
        const WorkMsg m{'G', p->control ? *p->control : 1.f};
        if (m.value != p->requested &&
            p->schedule->schedule_work(p->schedule->handle, sizeof m, &m) == LV2_WORKER_SUCCESS)
            p->requested = m.value;
        if (m.value == kFloodGain)
            for (uint32_t k = 0; k < kFloodMessages; ++k) {
                const WorkMsg f{kFlood, 0.f};
                p->schedule->schedule_work(p->schedule->handle, sizeof f, &f);
            }
        const float g = p->broken.load(std::memory_order_relaxed)
                            ? 0.f
                            : p->applied.load(std::memory_order_relaxed) * p->restored.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i) p->out[0][i] = g * p->in[0][i];
        break;
    }
    case kState:
        std::memcpy(p->out[0], p->in[0], n * sizeof(float));
        p->blocks.fetch_add(1, std::memory_order_relaxed);
        break;
    case kKey:
    case kKeyLiar:
        std::memcpy(p->out[0], p->key, n * sizeof(float));
        for (uint32_t i = 0; i < n; ++i) p->key[i] = 7.0f;
        if (p->kind == kKeyLiar)
            for (uint32_t i = n; i < n + 64; ++i) p->key[i] = 7.0f;
        break;
    case kLiar:
        // Treats output 0 as eight interleaved-by-block channels. Real
        // plugins do this when their channel count is a compile-time
        // constant and the manifest says something else.
        for (int c = 0; c < 8; ++c)
            std::memcpy(p->out[0] + std::size_t(c) * n, p->in[c & 1], n * sizeof(float));
        break;
    }
}

void cleanup(LV2_Handle h) { delete static_cast<Plugin*>(h); }

const LV2_Descriptor kDescriptors[] = {
    {"urn:sonora:test#crash", instantiate<kCrash>, connect, nullptr, run, nullptr, cleanup, nullptr},
    {"urn:sonora:test#sleep", instantiate<kSleep>, connect, nullptr, run, nullptr, cleanup, nullptr},
    {"urn:sonora:test#liar", instantiate<kLiar>, connect, nullptr, run, nullptr, cleanup, nullptr},
    {"urn:sonora:test#nan", instantiate<kNan>, connect, nullptr, run, nullptr, cleanup, nullptr},
    {"urn:sonora:test#lookahead", instantiate<kLookahead>, connect, nullptr, run, nullptr, cleanup, nullptr},
    {"urn:sonora:test#key", instantiate<kKey>, connect, nullptr, run, nullptr, cleanup, nullptr},
    {"urn:sonora:test#keyliar", instantiate<kKeyLiar>, connect, nullptr, run, nullptr, cleanup, nullptr},
    {"urn:sonora:test#state", instantiate<kState>, connect, nullptr, run, nullptr, cleanup, extension_data},
    {"urn:sonora:test#worker", instantiate<kWorker>, connect, nullptr, run, nullptr, cleanup,
     worker_extension_data},
};

}  // namespace

extern "C" LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index < sizeof kDescriptors / sizeof kDescriptors[0] ? &kDescriptors[index] : nullptr;
}
