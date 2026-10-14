// SPDX-License-Identifier: GPL-3.0-or-later
//
// Session::process() running real plugin hosts (ADR-0012 §7.2, checklist
// 4): PluginChainRunner is installed in the session, each InsertChain node
// runs through PluginClient into a sonora-plugin-host process the
// supervisor started, and the plan carries each chain's latency as the host
// reported it at Ready. The plan is built as engine/control's builder
// builds it (layout_track_graph, then compensate); the ChainResolver's part
// is done here by hand from SupervisedChain (checklist 5 moves it there).
//
//   aligned_lv2   the in-tree 100-frame look-ahead and Debian's eg-amp, and
//                 a dry track: at the bus all three are the same signal on
//                 the same frame, bit for bit
//   aligned_2048  a chain of four Debian LV2 limiters whose own latencies
//                 add up to 2048 frames (two LSP Limiter Mono at 20 ms
//                 look-ahead, two x42-dpl) beside a dry track: they land on
//                 the same frame, bit for bit (the DAW 5 exit check,
//                 against real plugins): at -20 dBFS both limiters sit
//                 below threshold and pass the signal unchanged
//   aligned_vst3  AGain (unity) and the LSP limiter as VST3, and a mixed
//                 LV2+VST3 chain in one host, beside a dry track, bit for
//                 bit
//
// Each aligned case also searches the lag in +-2 blocks at which the bus
// matches best, and requires 0: a misaligned track is reported as a lag,
// not only as an error.
//   stall         a host that takes 20 ms per block: its track fades out
//                 and goes silent, the slot is removed after 8 misses, the
//                 dry track plays on bit for bit, and no callback waits
//
// Between callbacks the aligned cases wait for every host to return its
// block, as the plugin_client host tests do: a harness may wait, the
// callback never does. Every process() call is checked for allocation.
// Exit 77: a Debian plugin this needs is not installed (a failure in CI
// unless SONORA_PLUGIN_TESTS_ALLOW_SKIP).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "sonora/plan_graph.h"
#include "sonora/platform/process.h"
#include "sonora/platform/wake.h"
#include "sonora/plugin_chain_runner.h"
#include "sonora/plugin_client/host_supervisor.h"
#include "sonora/session.h"

// operator new and delete counted on the thread inside process(), as
// test_plan_pdc does; malloc is left alone here because this test also
// spawns processes and lets lilv-free code allocate on its own threads.
namespace {
thread_local bool t_counting = false;
std::atomic<long> g_heap{0};
void* counted(std::size_t n) {
    if (t_counting) g_heap.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n > 0 ? n : 1);
}
void counted_free(void* p) {
    if (t_counting && p) g_heap.fetch_add(1, std::memory_order_relaxed);
    std::free(p);
}
}  // namespace

void* operator new(std::size_t n) {
    if (void* p = counted(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::size_t) noexcept { counted_free(p); }

using namespace sonora;
using namespace sonora::plugin_client;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

constexpr int kSkip = 77;
constexpr uint32_t kBlock = 128;
constexpr uint32_t kRate = 48000;
constexpr int64_t kLeadIn = 4 * kBlock;

const char* const kLookahead = "urn:sonora:test#lookahead";
const char* const kSleep = "urn:sonora:test#sleep";
const char* const kEgAmp = "http://lv2plug.in/plugins/eg-amp";
const char* const kLspLimiter = "http://lsp-plug.in/plugins/lv2/limiter_mono";
const char* const kDpl = "http://gareus.org/oss/lv2/dpl#mono";
const char* const kAGain = "vst3:84E8DE5F92554F5396FAE4133C935A18";
const char* const kLspLimiterVst3 = "vst3:6473702062316D20202020206A7A357A";

uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}
uint64_t now_ms() { return now_ns() / 1000000; }

// Deterministic noise, 0 before the lead-in, |x| <= amplitude: a one-frame
// misalignment is an error of the signal's own size, not of its slope.
float signal(int64_t t, float amplitude) {
    if (t < kLeadIn) return 0.f;
    uint32_t x = uint32_t(t) * 2654435761u + 0x9E3779B9u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return amplitude * (float(int32_t(x >> 8) % 4096) / 4096.f);
}

struct Args {
    std::string host, scenario;
    std::vector<std::string> bundles;
};

// What the scan knows of a plugin: `--probe` prints its dense parameters.
struct Probe {
    bool ok = false;
    std::vector<std::string> symbols;
    std::vector<float> defaults;
};

Probe probe(const Args& a, const std::string& id) {
    Probe p;
    const std::string out = "/tmp/sonora-session-plugins-probe-" + std::to_string(platform::current_process_id());
    std::vector<std::string> argv = {a.host, "--probe", id};
    for (const std::string& b : a.bundles) {
        argv.push_back("--bundle");
        argv.push_back(b);
    }
    platform::ChildProcess::Options o;
    o.stdout_path = out;
    std::string error;
    auto child = platform::ChildProcess::spawn(argv, o, error);
    if (!child) return p;
    child->wait(60000);
    std::string all;
    {
        std::ifstream f(out);
        std::getline(f, all);
    }
    std::remove(out.c_str());
    p.ok = all.find("\"ok\":true") != std::string::npos;
    for (std::size_t at = all.find("{\"key\":\""); at != std::string::npos; at = all.find("{\"key\":\"", at + 1)) {
        const std::size_t s = all.find("\"symbol\":\"", at);
        const std::size_t d = all.find("\"default\":", at);
        if (s == std::string::npos || d == std::string::npos) break;
        p.symbols.push_back(all.substr(s + 10, all.find('"', s + 10) - (s + 10)));
        p.defaults.push_back(std::strtof(all.c_str() + d + 10, nullptr));
    }
    return p;
}

struct Plugin {
    std::string id;
    // Overrides of the plugin's defaults, by symbol, in the format's own
    // values (LV2 plain, VST3 normalised).
    std::vector<std::pair<std::string, float>> set;
};

// Raised when a plugin is missing, so the scenario returns kSkip.
bool g_missing = false;

// A mono chain's spec from the scan: dense counts and the defaults, then
// the overrides, seeded into the store before the host starts (§5.1).
ChainSpec chain_spec(const Args& a, const std::vector<Plugin>& plugins) {
    ChainSpec s;
    s.channels = 1;
    for (const Plugin& pl : plugins) {
        const Probe p = probe(a, pl.id);
        if (!p.ok) {
            std::printf("  SKIP: %s is not installed or refused its probe (tools/Dockerfile.plugins)\n",
                        pl.id.c_str());
            g_missing = true;
        }
        std::vector<float> v = p.defaults;
        for (const auto& kv : pl.set) {
            const auto it = std::find(p.symbols.begin(), p.symbols.end(), kv.first);
            if (it == p.symbols.end()) {
                std::printf("  %s has no parameter %s\n", pl.id.c_str(), kv.first.c_str());
                g_missing = true;
                continue;
            }
            v[std::size_t(it - p.symbols.begin())] = kv.second;
        }
        s.plugins.push_back(pl.id);
        s.param_counts.push_back(uint32_t(v.size()));
        s.values.insert(s.values.end(), v.begin(), v.end());
    }
    s.bypass.assign(plugins.size(), 0.f);
    return s;
}

// One session, its tracks fed from the inputs, every track monitored, and
// the plugin side: client, supervisor, the runner the session calls.
struct Rig {
    ClientConfig ccfg;
    PluginClient client;
    SupervisorConfig scfg;
    HostSupervisor sup;
    PluginChainRunner runner;
    std::unique_ptr<Session> session;
    std::vector<std::unique_ptr<SupervisedChain>> chains;  // by track; null: dry
    DelayLinePool pool;
    Compensation comp;
    std::vector<std::vector<float>> in;
    std::vector<const float*> in_ptrs;
    std::vector<float> out[2];
    float* out_ptrs[2];
    int64_t frame = 0;
    float amplitude = 0.1f;
    uint64_t slowest_process_ns = 0;
    bool ready = false;

    static ClientConfig client_config() {
        ClientConfig c;
        c.block_frames = kBlock;
        c.max_channels = 16;
        c.max_stores = 8;
        c.max_nodes = 8;
        c.store_slots = 1024;
        return c;
    }
    static SupervisorConfig sup_config(const Args& a) {
        SupervisorConfig s;
        s.host_path = a.host;
        s.bundles = a.bundles;
        s.sample_rate = kRate;
        s.block_frames = kBlock;
        return s;
    }
    static SessionConfig session_config(int tracks) {
        SessionConfig cfg;
        cfg.dir = "test_session_plugins_unused";
        cfg.track_count = tracks;
        cfg.ring_seconds = 0.1;
        cfg.playback.chunk_frames = 256;
        cfg.playback.window_seconds = 0.02;
        cfg.playback.pool_chunks = 64;
        cfg.playback.max_block_frames = 256;
        cfg.playback.max_delay_frames = 16384;
        cfg.playback.max_delayed_edges = 16;
        return cfg;
    }

    // `specs[t]` empty: track t is dry. Pans: -1 left, +1 right.
    Rig(const Args& a, const std::vector<ChainSpec>& specs, const std::vector<float>& pans)
        : client(client_config()), scfg(sup_config(a)), sup(scfg, client), runner(client),
          session(std::make_unique<Session>(session_config(int(specs.size())))),
          pool(session->delay_lines()) {
        check(sup.ok(), "the host speaks this engine's versions and the clock is sealed");
        if (!sup.ok()) {
            std::printf("  %s\n", sup.why().c_str());
            return;
        }
        const auto tracks = uint32_t(specs.size());
        in.assign(tracks, std::vector<float>(kBlock, 0.f));
        for (auto& v : in) in_ptrs.push_back(v.data());
        for (int o = 0; o < 2; ++o) {
            out[o].assign(kBlock, 0.f);
            out_ptrs[o] = out[o].data();
        }
        for (uint32_t t = 0; t < tracks; ++t) {
            Track& tr = *session->tracks()[t];
            tr.set_monitor(Track::Monitor::Input);
            tr.set_pan(pans[t]);
        }
        chains.resize(tracks);
        for (uint32_t t = 0; t < tracks; ++t) {
            if (specs[t].plugins.empty()) continue;
            // Store and node state by track: one chain per track here.
            chains[t] = std::make_unique<SupervisedChain>(sup, specs[t], t, t);
            std::string error;
            if (!chains[t]->start(error)) {
                std::printf("  track %u: cannot start the chain: %s\n", t, error.c_str());
                check(false, "every host came up Ready");
                return;
            }
        }
        check(sup.region_lock_failures() == 0, "every channel region is locked");
        session->set_insert_chains(&runner);
        ready = publish();
        check(ready, "the plan with every chain is published");
    }

    ~Rig() {
        // The session first: its callbacks are over (process() is called
        // here, on this thread), and the hosts go before the supervisor.
        session.reset();
        chains.clear();
    }

    std::vector<ChainBinding> bindings() const {
        std::vector<ChainBinding> b(chains.size());
        for (std::size_t t = 0; t < chains.size(); ++t) {
            if (!chains[t]) continue;
            const InsertChainRef r = chains[t]->node();
            b[t].stable_key = uint32_t(t + 1);
            b[t].active = true;
            b[t].chain_id = 1000 + t;
            b[t].channel_slot = r.channel_slot;
            b[t].channel_generation = r.channel_generation;
            b[t].param_store = r.param_store;
            b[t].node_state = r.node_state;
            b[t].latency_frames = chains[t]->latency_frames();
        }
        return b;
    }

    bool publish() {
        const std::vector<ChainBinding> chain_bindings = bindings();
        const auto tracks = uint32_t(chains.size());
        uint32_t active = 0;
        for (const ChainBinding& c : chain_bindings) active += c.active ? 1 : 0;
        PlanSizes sizes;
        sizes.tracks = tracks;
        sizes.nodes = graph_nodes(tracks, active);
        sizes.edges = graph_edges(tracks, active);
        PlaybackPlan* p = create_plan(sizes);
        p->epoch = session->plans().next_epoch();
        p->chunk_frames = 256;
        p->pan_exponent = 0.5f;
        for (uint32_t t = 0; t < tracks; ++t) {
            p->tracks[t].engine_track = t;
            p->tracks[t].channels = 1;
        }
        layout_track_graph(*p, chain_bindings.data());
        comp = compensate(*p, session->max_delay_frames(), pool, nullptr);
        DiskReader& r = session->reader();
        const uint64_t gen = r.serve(session->plans().current(), p);
        r.run_pass();
        if (r.resident() != gen) return false;
        if (!session->plans().publish(p)) return false;
        r.serve(p, nullptr);
        r.run_pass();
        return true;
    }

    // One callback with the signal on every input; nothing in process()
    // may allocate, and its duration is kept.
    void block() {
        for (auto& v : in)
            for (uint32_t i = 0; i < kBlock; ++i) v[i] = signal(frame + i, amplitude);
        backend::AudioBlock b{};
        b.inputs = in_ptrs.data();
        b.input_count = uint32_t(in_ptrs.size());
        b.outputs = out_ptrs;
        b.output_count = 2;
        b.nframes = kBlock;
        const uint64_t t0 = now_ns();
        t_counting = true;
        session->process(b);
        t_counting = false;
        slowest_process_ns = std::max(slowest_process_ns, now_ns() - t0);
        frame += kBlock;
    }

    // The harness's wait: every host has returned what it was given.
    void settle() {
        for (auto& c : chains) {
            if (!c || !c->host()) continue;
            HostProcess* h = c->host();
            const uint64_t end = now_ms() + 2000;
            for (;;) {
                plugin::ChannelHeader& hd = h->header();
                const uint32_t seen = hd.done_word.load(std::memory_order_acquire);
                if (hd.completed_block.load(std::memory_order_acquire) ==
                        hd.submitted_block.load(std::memory_order_acquire) ||
                    now_ms() > end || !h->running())
                    break;
                platform::wait_for_change(hd.done_word, seen, 5'000'000);
            }
        }
    }
};

// Left and right compared over `blocks` waited callbacks: left must be
// `left_tracks` times right, delayed by the session's compensation, with
// at most `tolerance` of error (0: bit for bit). Also reports the lag in
// [-2 blocks, 2 blocks] at which left best matches right: misalignment
// shows as a nonzero best lag, not as a slightly larger error.
bool aligned(Rig& rig, uint32_t blocks, float left_tracks, float tolerance, const char* what) {
    const uint32_t comp = rig.comp.compensation_frames;
    std::vector<float> l, r;
    for (uint32_t k = 0; k < blocks; ++k) {
        rig.block();
        rig.settle();
        l.insert(l.end(), rig.out[0].begin(), rig.out[0].end());
        r.insert(r.end(), rig.out[1].begin(), rig.out[1].end());
    }
    // The first callbacks hold the hosts' ramp-ins and the faders' start;
    // the lead-in keeps the signal out of them. Compared from where the
    // signal is at the bus, plus a block of margin.
    const std::size_t from = std::size_t(kLeadIn + comp + kBlock);
    const std::size_t to = l.size() - 2 * kBlock;
    if (from + 8 * kBlock >= to) {
        check(false, "enough callbacks to compare");
        return false;
    }
    bool want_ok = true;
    float worst = 0.f;
    uint64_t nonzero = 0;
    for (std::size_t i = from; i < to; ++i) {
        const float want = signal(int64_t(i) - int64_t(comp), rig.amplitude);
        if (want != 0.f) ++nonzero;
        if (r[i] != want) want_ok = false;
        worst = std::max(worst, std::fabs(l[i] - left_tracks * want));
    }
    int best_lag = 0;
    float best_err = 1e30f;
    for (int lag = -int(2 * kBlock); lag <= int(2 * kBlock); ++lag) {
        float e = 0.f;
        for (std::size_t i = from; i < to; ++i) e += std::fabs(l[i] - left_tracks * r[std::size_t(int64_t(i) - lag)]);
        if (e < best_err) {
            best_err = e;
            best_lag = lag;
        }
    }
    char line[256];
    std::snprintf(line, sizeof line, "%s: compensation %u frames, %zu frames compared, worst error %.3g, best lag %d",
                  what, comp, to - from, double(worst), best_lag);
    const bool ok = want_ok && nonzero > (to - from) / 2 && worst <= tolerance && best_lag == 0;
    check(want_ok, "right (the dry track or a unity chain) is the signal, delayed by the compensation, bit for bit");
    check(ok, line);
    return ok;
}

void report_runner(const Rig& rig, uint64_t callbacks, uint32_t chains) {
    char line[200];
    std::snprintf(line, sizeof line, "every chain collected in every callback but its first (%llu of %llu)",
                  (unsigned long long)rig.runner.collected(), (unsigned long long)(callbacks * chains));
    check(rig.runner.collected() == (callbacks - 1) * chains && rig.runner.missed() == chains &&
              rig.runner.removed() == 0 && rig.runner.passed_through() == 0 && rig.runner.unsupported() == 0,
          line);
    check(g_heap.load() == 0, "process() allocated and freed nothing");
}

int scenario_aligned_lv2(const Args& a) {
    std::printf("aligned_lv2: %s (100 + 128), %s (0 + 128), and a dry track\n", kLookahead, kEgAmp);
    std::vector<ChainSpec> specs = {chain_spec(a, {{kLookahead, {}}}), chain_spec(a, {{kEgAmp, {}}}), ChainSpec{}};
    if (g_missing) return kSkip;
    Rig rig(a, specs, {-1.f, 1.f, -1.f});
    if (!rig.ready) return 1;
    check(rig.chains[0]->latency_frames() == 100 + kBlock, "the look-ahead chain reports 228 frames");
    check(rig.chains[1]->latency_frames() == kBlock, "eg-amp reports the IPC block, 128 frames");
    check(rig.comp.compensation_frames == 100 + kBlock && rig.comp.shortfall_frames == 0,
          "the session compensates 228 frames, none clamped");
    // Right: eg-amp's track. Left: the look-ahead's and the dry one, so
    // aligned and unity, left is exactly twice right.
    const uint32_t blocks = 80;
    aligned(rig, blocks, 2.f, 0.f, "bit for bit at the bus");
    report_runner(rig, blocks, 2);
    std::printf("  slowest process(): %.1f us\n", double(rig.slowest_process_ns) / 1000.0);
    return failures ? 1 : 0;
}

int scenario_aligned_2048(const Args& a) {
    std::printf("aligned_2048: %s x2 (lk 20 ms) and %s x2, own latency 2048, beside a dry track\n", kLspLimiter,
                kDpl);
    const Plugin lsp{kLspLimiter, {{"lk", 20.f}}};
    const Plugin dpl{kDpl, {}};
    std::vector<ChainSpec> specs = {chain_spec(a, {lsp, dpl, lsp, dpl}), ChainSpec{}};
    if (g_missing) return kSkip;
    Rig rig(a, specs, {-1.f, 1.f});
    if (!rig.ready) return 1;
    char line[160];
    std::snprintf(line, sizeof line, "the chain reports %u frames: its plugins' 2048 + the IPC block",
                  rig.chains[0]->latency_frames());
    check(rig.chains[0]->latency_frames() == 2048 + kBlock, line);
    check(rig.comp.compensation_frames == 2048 + kBlock && rig.comp.shortfall_frames == 0,
          "the dry track is delayed by all of it, none clamped");
    // -20 dBFS peaks: below both limiters' thresholds (0 dB and -1 dB).
    const uint32_t blocks = 120;
    aligned(rig, blocks, 1.f, 0.f, "the 2048-frame track lands on the dry track's frame, bit for bit");
    report_runner(rig, blocks, 1);
    std::printf("  slowest process(): %.1f us\n", double(rig.slowest_process_ns) / 1000.0);
    return failures ? 1 : 0;
}

int scenario_aligned_vst3(const Args& a) {
    std::printf("aligned_vst3: AGain at unity, the LSP limiter VST3, %s + LSP VST3 + AGain in one host, dry\n",
                kDpl);
    const Plugin again{kAGain, {{"Gain", 1.f}}};
    const Plugin lsp3{kLspLimiterVst3, {}};
    std::vector<ChainSpec> specs = {chain_spec(a, {again}), chain_spec(a, {lsp3}),
                                    chain_spec(a, {{kDpl, {}}, lsp3, again}), ChainSpec{}};
    if (g_missing) return kSkip;
    // Right: AGain's track. Left: the two limiter chains and the dry
    // track: three times right.
    Rig rig(a, specs, {1.f, -1.f, -1.f, -1.f});
    if (!rig.ready) return 1;
    check(rig.chains[0]->latency_frames() == kBlock, "AGain reports the IPC block");
    check(rig.chains[1]->latency_frames() == 240 + kBlock, "the LSP limiter VST3 reports 240 + 128");
    check(rig.chains[2]->latency_frames() == 64 + 240 + kBlock, "the mixed chain reports 64 + 240 + 128");
    check(rig.comp.compensation_frames == 64 + 240 + kBlock, "the session compensates the slowest chain");
    const uint32_t blocks = 100;
    aligned(rig, blocks, 3.f, 0.f, "LV2 and VST3 chains land on the dry track's frame, bit for bit");
    report_runner(rig, blocks, 3);
    std::printf("  slowest process(): %.1f us\n", double(rig.slowest_process_ns) / 1000.0);
    return failures ? 1 : 0;
}

int scenario_stall(const Args& a) {
    std::printf("stall: %s at 20 ms per block beside a dry track, callbacks paced at the period\n", kSleep);
    std::vector<ChainSpec> specs = {chain_spec(a, {{kSleep, {{"sleep_ms", 20.f}}}}), ChainSpec{}};
    if (g_missing) return kSkip;
    Rig rig(a, specs, {-1.f, 1.f});
    if (!rig.ready) return 1;
    const uint32_t comp = rig.comp.compensation_frames;
    check(comp == kBlock, "the sleeping chain reports the IPC block");
    const uint32_t slot = rig.chains[0]->node().channel_slot;
    bool dry_exact = true;
    bool silent_after = true;
    uint64_t removed_at = 0;
    const uint32_t blocks = 60;
    for (uint32_t k = 0; k < blocks; ++k) {
        const int64_t at = rig.frame;
        rig.block();
        for (uint32_t i = 0; i < kBlock; ++i)
            dry_exact = dry_exact && rig.out[1][i] == signal(at + i - comp, rig.amplitude);
        if (!removed_at && rig.runner.removed() > 0) removed_at = k;
        if (removed_at && k > removed_at)
            for (uint32_t i = 0; i < kBlock; ++i) silent_after = silent_after && rig.out[0][i] == 0.f;
        std::this_thread::sleep_for(std::chrono::microseconds(2667));
    }
    check(dry_exact, "the dry track played every block, bit for bit");
    check(removed_at > 0 && rig.client.removed(slot), "the slot was removed after its miss streak");
    check(rig.client.slot_status(slot).misses >= kMaxConsecutiveMisses, "8 consecutive misses counted");
    check(silent_after, "after the removal its track is silence, never the dry signal");
    char line[160];
    std::snprintf(line, sizeof line, "no callback waited on the host: slowest process() %.1f us, budget 2667 us",
                  double(rig.slowest_process_ns) / 1000.0);
    // A callback that waited would take the host's 20 ms; this bound is far
    // under it and still loose enough for a loaded container.
    check(rig.slowest_process_ns < 1'000'000, line);
    check(g_heap.load() == 0, "process() allocated and freed nothing");
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--host") a.host = argv[i + 1];
        else if (k == "--scenario") a.scenario = argv[i + 1];
        else if (k == "--bundle") a.bundles.push_back(argv[i + 1]);
    }
    if (a.host.empty() || a.scenario.empty()) {
        std::fprintf(stderr, "usage: %s --host PATH --scenario NAME [--bundle DIR]...\n", argv[0]);
        return 2;
    }
    int rc = 2;
    if (a.scenario == "aligned_lv2") rc = scenario_aligned_lv2(a);
    else if (a.scenario == "aligned_2048") rc = scenario_aligned_2048(a);
    else if (a.scenario == "aligned_vst3") rc = scenario_aligned_vst3(a);
    else if (a.scenario == "stall") rc = scenario_stall(a);
    else std::fprintf(stderr, "unknown scenario %s\n", a.scenario.c_str());
    if (rc == 0 && failures) rc = 1;
    std::printf("%s: %s\n", a.scenario.c_str(), rc == 0 ? "ok" : rc == kSkip ? "skipped" : "FAILED");
    return rc;
}
