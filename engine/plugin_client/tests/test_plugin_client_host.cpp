// SPDX-License-Identifier: GPL-3.0-or-later
//
// engine/plugin_client end to end: drivers/fake runs the callback, which runs
// PluginClient's InsertChain step; HostSupervisor starts the real
// sonora-plugin-host process, timer-paced (the futex wake is gated off).
// Built where lilv is (tools/build-plugins.sh, CI's plugins job).
//
//   passthrough  output = the previous callback's input, bit for bit, after
//                the first block's ramp-in; latency reported = one block
//   params       a gain moved in callback k is heard from callback k + 1,
//                exactly; the look-ahead plugin's latency + one block is
//                what PDC is told
//   miss         a host that stops returning blocks: fade-out of the last
//                sample, silence, 8 counted misses remove the channel with
//                the culprit named, the supervisor restarts it
//                make-before-break, and the output ramps back in
//   crash        a segfaulting plugin: restarted, parameters from the store
//   restarts     a plugin that crashes every time: 3 restarts, then down
//   nan          the host's scrub, and the engine's own guard on its copy
//   native       Sonora's EQ bundle against its N0 reference render, one
//                block late (--args-file from sonora-dsp-measure)
//
// Between callbacks the test waits for the host where the case needs every
// block back (a harness may wait; the client never does). The miss, crash
// and restarts cases run paced instead, at the period, without waiting.
#include "sonora/fake_backend.h"
#include "sonora/platform/process.h"
#include "sonora/platform/wake.h"
#include "sonora/plugin_client/host_supervisor.h"
#include "sonora/plugin_client/plugin_client.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace sonora;
using namespace sonora::plugin_client;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

constexpr uint32_t kBlock = 128;
constexpr uint32_t kRate = 48000;
const char* const kCrash = "urn:sonora:test#crash";
const char* const kSleep = "urn:sonora:test#sleep";
const char* const kNan = "urn:sonora:test#nan";
const char* const kLookahead = "urn:sonora:test#lookahead";

uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}
uint64_t now_ms() { return now_ns() / 1000000; }

struct Args {
    std::string host, scenario, args_file;
    std::vector<std::string> bundles, chain;
};

// What the scan knows of a plugin: `--probe` prints its dense parameters.
struct Probe {
    std::vector<std::string> symbols;
    std::vector<float> defaults;
};

Probe probe(const Args& a, const std::string& id) {
    Probe p;
    const std::string out = "/tmp/sonora-plugin-client-probe-" + std::to_string(platform::current_process_id());
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
    child->wait(30000);
    std::string all;
    {
        std::ifstream f(out);
        std::getline(f, all);
    }
    std::remove(out.c_str());
    for (std::size_t at = all.find("{\"key\":\""); at != std::string::npos; at = all.find("{\"key\":\"", at + 1)) {
        const std::size_t s = all.find("\"symbol\":\"", at);
        const std::size_t d = all.find("\"default\":", at);
        if (s == std::string::npos || d == std::string::npos) break;
        p.symbols.push_back(all.substr(s + 10, all.find('"', s + 10) - (s + 10)));
        p.defaults.push_back(std::strtof(all.c_str() + d + 10, nullptr));
    }
    return p;
}

// The engine's callback in miniature: one InsertChain node, its input from
// a stream the test wrote, its output into a stream the test reads. No
// allocation here: every buffer is sized before the backend is activated.
class ChainSession final : public backend::ProcessCallback {
public:
    ChainSession(PluginClient& client, uint32_t channels, uint64_t blocks)
        : client_(client), channels_(channels), blocks_(blocks), in(channels), out(channels), outcomes(blocks) {
        for (uint32_t c = 0; c < channels; ++c) {
            in[c].assign(blocks * kBlock, 0.f);
            out[c].assign(blocks * kBlock, 0.f);
        }
    }

    void process(const backend::AudioBlock& b) noexcept SONORA_RT override {
        if (callback < blocks_ && b.nframes == kBlock) {
            const float* ip[4] = {};
            float* op[4] = {};
            for (uint32_t c = 0; c < channels_; ++c) {
                ip[c] = in[c].data() + callback * kBlock;
                op[c] = out[c].data() + callback * kBlock;
            }
            client_.begin_callback(now_ns());
            outcomes[callback] = client_.process_chain(ref, ip, op, b.nframes, events, event_count);
            client_.end_callback();
            event_count = 0;
            for (uint32_t o = 0; o < b.output_count; ++o)
                std::memcpy(b.outputs[o], op[o < channels_ ? o : 0], kBlock * sizeof(float));
            ++callback;
            return;
        }
        for (uint32_t o = 0; o < b.output_count; ++o) std::memset(b.outputs[o], 0, b.nframes * sizeof(float));
    }

    void send(uint32_t param, float value, uint32_t offset = 0) {
        events[event_count++] = ParamEvent{0, param, offset, value};
    }
    const float* out_block(uint32_t c, uint64_t k) const { return out[c].data() + k * kBlock; }
    const float* in_block(uint32_t c, uint64_t k) const { return in[c].data() + k * kBlock; }

    InsertChainRef ref;
    uint64_t callback = 0;
    ParamEvent events[8];
    uint32_t event_count = 0;

private:
    PluginClient& client_;
    uint32_t channels_;
    uint64_t blocks_;

public:
    std::vector<std::vector<float>> in, out;
    std::vector<Outcome> outcomes;
};

// Client, supervisor, backend and one supervised chain.
struct Rig {
    ClientConfig ccfg;
    PluginClient client;
    SupervisorConfig scfg;
    HostSupervisor sup;
    FakeBackend fake;
    ChainSession session;
    std::unique_ptr<SupervisedChain> chain;
    bool ready = false;

    static ClientConfig client_config() {
        ClientConfig c;
        c.max_channels = 16;
        c.max_stores = 4;
        c.max_nodes = 4;
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

    Rig(const Args& a, ChainSpec spec, uint64_t blocks)
        : client(client_config()), scfg(sup_config(a)), sup(scfg, client),
          fake(FakeBackend::onboard_config()), session(client, spec.channels, blocks) {
        check(sup.ok(), "the host speaks this engine's versions and the clock is sealed");
        if (!sup.ok()) {
            std::printf("  %s\n", sup.why().c_str());
            return;
        }
        chain = std::make_unique<SupervisedChain>(sup, std::move(spec), 0, 0);
        std::string error;
        if (!chain->start(error)) {
            std::printf("  cannot start the chain: %s\n", error.c_str());
            check(false, "the host came up Ready");
            return;
        }
        session.ref = chain->node();
        const uint32_t ch = session.ref.channels;
        if (!fake.open({"plugin-client-test", ch, ch, kRate, FakeBackend::onboard().name}, session, error) ||
            !fake.activate(error)) {
            std::printf("  fake backend: %s\n", error.c_str());
            check(false, "the fake backend runs");
            return;
        }
        ready = true;
    }
    ~Rig() {
        fake.deactivate();
        chain.reset();
    }

    // The harness's wait: until the host has returned what it was given.
    void settle() {
        HostProcess* h = chain->host();
        if (!h) return;
        const uint64_t end = now_ms() + 2000;
        for (;;) {
            plugin::ChannelHeader& hd = h->header();
            const uint32_t seen = hd.done_word.load(std::memory_order_acquire);
            if (hd.completed_block.load(std::memory_order_acquire) ==
                    hd.submitted_block.load(std::memory_order_acquire) ||
                now_ms() > end || !h->running())
                return;
            platform::wait_for_change(hd.done_word, seen, 5'000'000);
        }
    }
    void run_waited(uint64_t n) {
        for (uint64_t i = 0; i < n; ++i) {
            fake.run_blocks(1);
            settle();
        }
    }
    void run_paced(uint64_t n) {
        for (uint64_t i = 0; i < n; ++i) {
            fake.run_blocks(1);
            std::this_thread::sleep_for(std::chrono::microseconds(2667));
        }
    }
    // Paced callbacks, polling the supervisor as the control thread does,
    // until it swaps or gives up. The swap is a plan swap: the next
    // callback runs the new node; once it has, the old plan is retired.
    SupervisedChain::Event until_event(uint64_t max_ms) {
        const uint64_t end = now_ms() + max_ms;
        while (now_ms() < end) {
            run_paced(1);
            const SupervisedChain::Event e = chain->poll(now_ms());
            if (e == SupervisedChain::Event::Swapped) {
                session.ref = chain->node();
                fake.run_blocks(1);
                settle();
                chain->retired();
                return e;
            }
            if (e == SupervisedChain::Event::Down) return e;
        }
        return SupervisedChain::Event::None;
    }
};

ChainSpec mono_spec(const Args& a, const std::string& id) {
    const Probe p = probe(a, id);
    ChainSpec s;
    s.plugins = {id};
    s.param_counts = {uint32_t(p.defaults.size())};
    s.values = p.defaults;
    s.channels = 1;
    return s;
}

void fill_noise(ChainSession& s, uint32_t seed) {
    uint32_t x = seed;
    for (auto& plane : s.in)
        for (float& v : plane) {
            x = x * 1664525u + 1013904223u;
            v = float(int32_t(x >> 8) % 1000) / 2000.f;
        }
}

// Block k of the output is block k-1 of the input, bit for bit, from `from`
// to `to`; the first block after silence is that block ramped in.
bool plays_previous_block(const ChainSession& s, uint64_t from, uint64_t to, bool ramped_first) {
    for (uint64_t k = from; k < to; ++k) {
        const float* o = s.out_block(0, k);
        const float* i = s.in_block(0, k - 1);
        for (uint32_t f = 0; f < kBlock; ++f) {
            const float want = (ramped_first && k == from) ? i[f] * (float(f) / float(kBlock)) : i[f];
            if (o[f] != want) {
                std::printf("  block %llu frame %u: %.9g, want %.9g\n", (unsigned long long)k, f, double(o[f]),
                            double(want));
                return false;
            }
        }
    }
    return true;
}

int scenario_passthrough(const Args& a) {
    std::printf("passthrough: %s, timer-paced host\n", kCrash);
    Rig rig(a, mono_spec(a, kCrash), 64);
    if (!rig.ready) return 1;
    fill_noise(rig.session, 7);
    check(rig.chain->latency_frames() == kBlock, "latency for PDC: the IPC block, 128 frames");
    rig.run_waited(64);
    check(rig.session.outcomes[0] == Outcome::Missed, "callback 0 plays silence: nothing was submitted before");
    bool all = true;
    for (uint64_t k = 1; k < 64; ++k) all = all && rig.session.outcomes[k] == Outcome::Collected;
    check(all, "every later callback collects the previous block");
    check(plays_previous_block(rig.session, 1, 64, true),
          "bit-exact, one block late, the first block ramped in from 0");
    const SlotStatus st = rig.client.slot_status(rig.session.ref.channel_slot);
    check(st.submitted == 64 && st.collected == 63 && st.misses == 0, "64 submitted, 63 collected, no miss");
    check(rig.chain->host()->header().submitted_block.load() == 64, "blocks numbered from 1: the 64th is 64");
    return 0;
}

int scenario_params(const Args& a) {
    std::printf("params: %s, a gain in dB moved through the ring\n", kLookahead);
    Rig rig(a, mono_spec(a, kLookahead), 64);
    if (!rig.ready) return 1;
    fill_noise(rig.session, 11);
    char what[160];
    std::snprintf(what, sizeof what, "latency for PDC: the plugin's 100 frames + the IPC block = %u",
                  rig.chain->latency_frames());
    check(rig.chain->latency_frames() == 100 + kBlock, what);
    rig.run_waited(20);
    rig.session.send(0, -6.f);
    rig.run_waited(20);
    rig.session.send(0, 3.f);
    rig.run_waited(24);
    // Block b of the host's output is gain(b) * input delayed 100 frames;
    // the client plays it in callback b + 1.
    const std::vector<float>& x = rig.session.in[0];
    auto gain_of = [](uint64_t b) { return b < 20 ? 0.f : b < 40 ? -6.f : 3.f; };
    bool exact = true;
    for (uint64_t c = 2; c < 64 && exact; ++c) {
        const uint64_t b = c - 1;
        const float g = std::pow(10.f, gain_of(b) / 20.f);
        for (uint32_t f = 0; f < kBlock; ++f) {
            const int64_t n = int64_t(b * kBlock + f) - 100;
            const float want = g * (n >= 0 ? x[std::size_t(n)] : 0.f);
            if (rig.session.out_block(0, c)[f] != want) {
                std::printf("  callback %llu frame %u: %.9g, want %.9g\n", (unsigned long long)c, f,
                            double(rig.session.out_block(0, c)[f]), double(want));
                exact = false;
                break;
            }
        }
    }
    check(exact, "a change sent in callback k applies to block k, heard in callback k + 1, exactly");
    check(rig.client.slot_status(rig.session.ref.channel_slot).params_refused == 0, "no change refused");
    check(rig.chain->host()->header().params_late.load() == 0, "no change arrived late");
    return 0;
}

int scenario_miss(const Args& a) {
    std::printf("miss: %s, the host stops returning blocks\n", kSleep);
    Rig rig(a, mono_spec(a, kSleep), 200);
    if (!rig.ready) return 1;
    fill_noise(rig.session, 3);
    rig.run_waited(10);
    const uint32_t first_slot = rig.session.ref.channel_slot;
    const uint64_t k0 = rig.session.callback;
    rig.session.send(0, 300.f);  // sleep_ms: the host holds this block for 300 ms
    uint64_t removed_at = 0;
    for (int i = 0; i < 40 && !removed_at; ++i) {
        rig.run_paced(1);
        if (rig.session.outcomes[rig.session.callback - 1] == Outcome::Removed) removed_at = rig.session.callback - 1;
    }
    check(rig.session.outcomes[k0] == Outcome::Collected, "the callback that sent the change still played");
    // The first miss: the last sample played, faded to 0 over 64 frames.
    const float last = rig.session.out_block(0, k0)[kBlock - 1];
    bool fade = true;
    for (uint32_t f = 0; f < kBlock; ++f) {
        const uint32_t k = f + 1;
        const float want = k < kFadeFrames ? last * (float(kFadeFrames - k) / float(kFadeFrames)) : 0.f;
        fade = fade && rig.session.out_block(0, k0 + 1)[f] == want;
    }
    check(rig.session.outcomes[k0 + 1] == Outcome::Missed && fade, "the first miss fades the last sample out over 64 frames");
    bool silent = true;
    for (uint64_t k = k0 + 2; k <= removed_at; ++k)
        for (uint32_t f = 0; f < kBlock; ++f) silent = silent && rig.session.out_block(0, k)[f] == 0.f;
    check(silent, "then silence, never the dry signal");
    char what[160];
    std::snprintf(what, sizeof what, "removed on the 8th counted miss (callback %llu after the change)",
                  (unsigned long long)(removed_at - k0));
    check(removed_at == k0 + kMaxConsecutiveMisses, what);
    const SlotStatus st = rig.client.slot_status(first_slot);
    check(st.removed && rig.client.removed(first_slot), "the removal flag the control thread reads is set");
    check(st.culprit == 0, "the culprit is named from the streak: instance 0");
    check(st.misses == kMaxConsecutiveMisses, "8 misses counted, the late host's second block included");

    rig.session.send(0, 0.f);  // into the store: the new host gets it on its first submit
    rig.run_paced(1);
    check(rig.until_event(10000) == SupervisedChain::Event::Swapped, "the supervisor restarted the chain");
    check(rig.session.ref.channel_slot != first_slot, "on a new slot, the old one retired after the swap");
    check(rig.chain->restarts() == 1, "one restart");
    const uint64_t back = rig.session.callback;
    rig.run_waited(20);
    // The first callback on the new channel submits and plays silence.
    check(rig.session.outcomes[back] == Outcome::Collected, "the new host's blocks come back");
    check(plays_previous_block(rig.session, back, back + 20, true), "and ramp in, then bit-exact");
    check(rig.client.slot_status(rig.session.ref.channel_slot).misses == 0, "the swap's silence is not a miss");
    return 0;
}

int scenario_crash(const Args& a) {
    std::printf("crash: %s segfaults in a block\n", kCrash);
    Rig rig(a, mono_spec(a, kCrash), 400);
    if (!rig.ready) return 1;
    fill_noise(rig.session, 5);
    rig.run_waited(10);
    rig.session.send(0, 1.f);  // crash
    rig.run_paced(1);
    rig.session.send(0, 0.f);  // held in the store while the channel is busy or gone
    rig.run_paced(1);
    check(rig.until_event(10000) == SupervisedChain::Event::Swapped, "restarted after the crash");
    check(rig.chain->why().find("signal 11") != std::string::npos, "the crash is named: killed by SIGSEGV");
    std::printf("  why: %s\n", rig.chain->why().c_str());
    const uint64_t back = rig.session.callback;
    rig.run_waited(30);
    check(plays_previous_block(rig.session, back, back + 30, true),
          "the restarted host plays again, bit-exact, with the store's crash=0");
    return 0;
}

int scenario_restarts(const Args& a) {
    std::printf("restarts: %s crashes on every block it is given\n", kCrash);
    Rig rig(a, mono_spec(a, kCrash), 4000);
    if (!rig.ready) return 1;
    rig.run_waited(5);
    rig.session.send(0, 1.f);
    SupervisedChain::Event e = SupervisedChain::Event::None;
    for (int i = 0; i < 6 && e != SupervisedChain::Event::Down; ++i) e = rig.until_event(10000);
    check(e == SupervisedChain::Event::Down, "the chain goes down");
    check(rig.chain->restarts() == 3, "after 3 restarts within 60 s (§5.3)");
    std::printf("  why: %s\n", rig.chain->why().c_str());
    rig.run_paced(5);
    const uint64_t k = rig.session.callback - 1;
    bool silent = true;
    for (uint32_t f = 0; f < kBlock; ++f) silent = silent && rig.session.out_block(0, k)[f] == 0.f;
    check(silent, "and its path stays silent");
    return 0;
}

int scenario_nan(const Args& a) {
    std::printf("nan: %s writes NaN and Inf\n", kNan);
    Rig rig(a, mono_spec(a, kNan), 80);
    if (!rig.ready) return 1;
    fill_noise(rig.session, 13);
    rig.run_waited(10);
    rig.session.send(0, 1.f);
    rig.run_waited(10);
    bool finite = true;
    for (float v : rig.session.out[0]) finite = finite && std::isfinite(v);
    HostProcess* h = rig.chain->host();
    check(h->header().instances[0].nonfinite_samples.load() > 0, "the host scrubbed and counted the plugin's NaN/Inf");
    check(finite, "nothing non-finite reached the engine's output");

    // A plugin thread can write after the host's scrub, so the engine
    // checks its own copy (rt-engineer M4): a NaN written into the output
    // plane once the block is complete makes that block a miss and a fault.
    rig.session.send(0, 0.f);
    rig.run_waited(5);
    const uint32_t slot = rig.session.ref.channel_slot;
    const SlotStatus before = rig.client.slot_status(slot);
    rig.run_waited(1);
    plugin::output_plane(h->region(), h->layout(), 0)[3] = std::numeric_limits<float>::quiet_NaN();
    const uint64_t k = rig.session.callback;
    rig.run_waited(1);
    const SlotStatus after = rig.client.slot_status(slot);
    check(rig.session.outcomes[k] == Outcome::Missed, "the poisoned block is a miss");
    check(after.nonfinite_samples == before.nonfinite_samples + 1 && after.nonfinite_blocks == before.nonfinite_blocks + 1,
          "counted as one non-finite sample and one faulted block");
    finite = true;
    for (float v : rig.session.out[0]) finite = finite && std::isfinite(v);
    check(finite, "and never played");
    const uint64_t back = rig.session.callback;
    rig.run_waited(10);
    check(plays_previous_block(rig.session, back, back + 10, true), "the next block ramps back in");
    return 0;
}

bool read_planar(const std::string& path, uint32_t channels, std::vector<std::vector<float>>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f || channels == 0) return false;
    std::vector<float> all(std::size_t(f.tellg()) / sizeof(float));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(all.data()), std::streamsize(all.size() * sizeof(float)));
    if (all.size() % channels) return false;
    const std::size_t frames = all.size() / channels;
    out.assign(channels, {});
    for (uint32_t c = 0; c < channels; ++c)
        out[c].assign(all.begin() + std::ptrdiff_t(c * frames), all.begin() + std::ptrdiff_t((c + 1) * frames));
    return true;
}

int scenario_native(const Args& a) {
    // The fixture: one argument per line (sonora-dsp-measure --host-fixture).
    std::vector<std::string> lines;
    {
        std::ifstream f(a.args_file);
        for (std::string l; std::getline(f, l);) lines.push_back(l);
    }
    uint32_t channels = 0;
    int64_t expect_latency = -1;
    double tolerance = 0.0;
    std::string input, expected;
    std::vector<std::string> sets;
    for (std::size_t i = 0; i + 1 < lines.size(); i += 2) {
        const std::string& k = lines[i];
        const std::string& v = lines[i + 1];
        if (k == "--channels") channels = uint32_t(std::atoi(v.c_str()));
        else if (k == "--input") input = v;
        else if (k == "--expected") expected = v;
        else if (k == "--tolerance") tolerance = std::atof(v.c_str());
        else if (k == "--expect-latency") expect_latency = std::atoll(v.c_str());
        else if (k == "--set") sets.push_back(v);
    }
    std::printf("native: %s, %u channel(s), %zu value(s) from the model, tolerance %.3g\n", a.chain[0].c_str(),
                channels, sets.size(), tolerance);
    std::vector<std::vector<float>> in, want;
    if (a.chain.size() != 1 || !read_planar(input, channels, in) || !read_planar(expected, channels, want) ||
        in[0].size() != want[0].size()) {
        check(false, "the fixture's input and reference render read");
        return 1;
    }
    // The golden values go where the engine puts a session's values: the
    // store and the seeded table, by dense index from the scan.
    const Probe p = probe(a, a.chain[0]);
    ChainSpec spec;
    spec.plugins = a.chain;
    spec.param_counts = {uint32_t(p.defaults.size())};
    spec.values = p.defaults;
    spec.channels = channels;
    for (const std::string& s : sets) {
        const std::size_t colon = s.find(':'), eq = s.rfind('=');
        const std::string symbol = s.substr(colon + 1, eq - colon - 1);
        bool found = false;
        for (std::size_t d = 0; d < p.symbols.size(); ++d)
            if (p.symbols[d] == symbol) {
                spec.values[d] = std::strtof(s.c_str() + eq + 1, nullptr);
                found = true;
            }
        if (!found) std::printf("  --set %s: no such parameter in the probe\n", s.c_str());
    }
    const uint64_t blocks = std::min<uint64_t>(in[0].size() / kBlock, 200);
    Rig rig(a, spec, blocks + 1);
    if (!rig.ready) return 1;
    for (uint32_t c = 0; c < channels; ++c)
        std::copy(in[c].begin(), in[c].begin() + std::ptrdiff_t(blocks * kBlock), rig.session.in[c].begin());
    char what[160];
    std::snprintf(what, sizeof what, "latency for PDC: the plugin's %lld + the IPC block = %u",
                  (long long)expect_latency, rig.chain->latency_frames());
    check(int64_t(rig.chain->latency_frames()) == expect_latency + kBlock, what);
    rig.run_waited(blocks + 1);
    double worst = 0.0;
    bool all_collected = true;
    for (uint64_t k = 0; k < blocks; ++k) {
        all_collected = all_collected && rig.session.outcomes[k + 1] == Outcome::Collected;
        for (uint32_t c = 0; c < channels; ++c)
            for (uint32_t f = 0; f < kBlock; ++f) {
                const float ref = want[c][k * kBlock + f] * (k == 0 ? float(f) / float(kBlock) : 1.f);
                worst = std::max(worst, std::fabs(double(rig.session.out_block(c, k + 1)[f]) - double(ref)));
            }
    }
    check(all_collected, "every block came back in time");
    std::snprintf(what, sizeof what, "%llu blocks one block late against the N0 render: max |diff| %.3g <= %.3g",
                  (unsigned long long)blocks, worst, tolerance);
    check(worst <= tolerance, what);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--host") a.host = v;
        else if (k == "--bundle") a.bundles.push_back(v);
        else if (k == "--scenario") a.scenario = v;
        else if (k == "--chain") a.chain.push_back(v);
        else if (k == "--args-file") a.args_file = v;
        else {
            std::fprintf(stderr, "unknown argument %s\n", k.c_str());
            return 2;
        }
    }
    int r = 2;
    if (a.scenario == "passthrough") r = scenario_passthrough(a);
    else if (a.scenario == "params") r = scenario_params(a);
    else if (a.scenario == "miss") r = scenario_miss(a);
    else if (a.scenario == "crash") r = scenario_crash(a);
    else if (a.scenario == "restarts") r = scenario_restarts(a);
    else if (a.scenario == "nan") r = scenario_nan(a);
    else if (a.scenario == "native") r = scenario_native(a);
    else std::fprintf(stderr, "unknown scenario %s\n", a.scenario.c_str());
    if (r != 0 || failures) {
        std::printf("plugin_client_host %s: %d check(s) failed\n", a.scenario.c_str(), failures);
        return 1;
    }
    std::printf("plugin_client_host %s: ok\n", a.scenario.c_str());
    return 0;
}
