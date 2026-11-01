// SPDX-License-Identifier: GPL-3.0-or-later
//
// engine/plugin_client end to end: drivers/fake runs the callback, which runs
// PluginClient's InsertChain step; HostSupervisor starts the real
// sonora-plugin-host process, timer-paced (the futex wake is gated off).
// Built where lilv is (tools/build-plugins.sh, CI's plugins job).
//
//   async_start  begin() returns at once and poll() reports the first host
//                Ready after its 400 ms validation; no call waits
//   ready_timeout  a host not Ready within the bound is killed and the
//                chain is down, noticed from poll()
//   async_refused  a plugin refused at validation: down, named, not retried
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
#include "sonora/platform/ipc.h"
#include "sonora/platform/process.h"
#include "sonora/platform/wait_set.h"
#include "sonora/platform/wake.h"
#include "sonora/plugin/host_control.h"
#include "sonora/plugin_client/host_supervisor.h"
#include "sonora/plugin_client/plugin_client.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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
const char* const kKey = "urn:sonora:test#key";

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
    ChainSession(PluginClient& client, uint32_t channels, uint64_t blocks, uint32_t keys = 0)
        : client_(client), channels_(channels), keys_(keys), blocks_(blocks), in(channels), out(channels),
          key(keys), outcomes(blocks) {
        for (uint32_t c = 0; c < channels; ++c) {
            in[c].assign(blocks * kBlock, 0.f);
            out[c].assign(blocks * kBlock, 0.f);
        }
        for (auto& k : key) k.assign(blocks * kBlock, 0.f);
    }

    void process(const backend::AudioBlock& b) noexcept SONORA_RT override {
        if (callback < blocks_ && b.nframes == kBlock) {
            const float* ip[4] = {};
            float* op[4] = {};
            const float* kp[4] = {};
            for (uint32_t c = 0; c < channels_; ++c) {
                ip[c] = in[c].data() + callback * kBlock;
                op[c] = out[c].data() + callback * kBlock;
            }
            for (uint32_t k = 0; k < keys_; ++k) kp[k] = key[k].data() + callback * kBlock;
            client_.begin_callback(now_ns());
            outcomes[callback] = client_.process_chain(ref, ip, op, b.nframes, events, event_count, kp);
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
    uint32_t keys_;
    uint64_t blocks_;

public:
    std::vector<std::vector<float>> in, out, key;
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
          fake(FakeBackend::onboard_config()), session(client, spec.channels, blocks, spec.key_channels) {
        check(sup.ok(), "the host speaks this engine's versions and the clock is sealed");
        // The container grants memlock (tools/build-plugins.sh), so a
        // refusal here is a bug, not the environment (auditor M2, #49).
        check(sup.clock_locked() && sup.region_lock_failures() == 0, "the clock and channel regions are locked");
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
            const uint64_t t0 = now_ms();
            const SupervisedChain::Event e = chain->poll(t0);
            slowest_poll_ms = std::max(slowest_poll_ms, now_ms() - t0);
            // The replacement is spawned, not waited for (L6 on #49): its
            // Ready is a later poll.
            if (e == SupervisedChain::Event::Restarting) {
                ++restarting;
                old_slot_stayed = old_slot_stayed && chain->node().channel_slot == session.ref.channel_slot;
            }
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
    uint64_t slowest_poll_ms = 0;
    int restarting = 0;
    bool old_slot_stayed = true;  // node() keeps the old slot until Swapped
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

// ---- the supervisor never blocks its caller (code-reviewer L6 on PR #49)
//
// The control server's thread runs the supervisor, and it is the document
// thread: a start that waited for Ready would freeze every client for up to
// 10 s. begin() spawns and returns; poll() notices Ready, the refusal or the
// timeout. Each call is timed.

// Far above a poll's own work (a spawn, on a loaded shared machine), far
// below the 400 ms validation and the 10 s bound a wait would take.
constexpr uint64_t kPollBudgetMs = 150;

struct AsyncRig {
    PluginClient client;
    SupervisorConfig scfg;
    HostSupervisor sup;
    uint64_t slowest_call_ms = 0;

    AsyncRig(const Args& a, uint64_t ready_timeout_ms)
        : client(Rig::client_config()), scfg(Rig::sup_config(a)), sup(with_timeout(scfg, ready_timeout_ms), client) {}
    static SupervisorConfig& with_timeout(SupervisorConfig& s, uint64_t ms) {
        s.ready_timeout_ms = ms;
        return s;
    }
    // poll() every 5 ms until an event or `max_ms`; every call timed.
    SupervisedChain::Event until_event(SupervisedChain& c, uint64_t max_ms, uint64_t& polls_before) {
        const uint64_t end = now_ms() + max_ms;
        polls_before = 0;
        while (now_ms() < end) {
            const uint64_t t0 = now_ms();
            const SupervisedChain::Event e = c.poll(t0);
            slowest_call_ms = std::max(slowest_call_ms, now_ms() - t0);
            if (e != SupervisedChain::Event::None) return e;
            ++polls_before;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return SupervisedChain::Event::None;
    }
};

ChainSpec sleep_spec(const Args& a, float sleep_ms) {
    ChainSpec s = mono_spec(a, kSleep);
    if (!s.values.empty()) s.values[0] = sleep_ms;  // validation runs a block: Ready waits for it
    return s;
}

int scenario_async_start(const Args& a) {
    std::printf("async_start: %s takes 400 ms per block, so Ready comes after its validation\n", kSleep);
    AsyncRig rig(a, 10000);
    if (!rig.sup.ok()) {
        check(false, rig.sup.why().c_str());
        return 1;
    }
    SupervisedChain chain(rig.sup, sleep_spec(a, 400.f), 0, 0);
    std::string error;
    const uint64_t t0 = now_ms();
    check(chain.begin(error), "begin() spawned the host");
    const uint64_t began = now_ms() - t0;
    char line[160];
    std::snprintf(line, sizeof line, "begin() returned in %llu ms, without waiting for Ready",
                  (unsigned long long)began);
    check(began < kPollBudgetMs, line);
    check(chain.starting() && chain.node().channel_slot == kNoSlot, "starting, bound to no slot yet");
    uint64_t polls = 0;
    const SupervisedChain::Event e = rig.until_event(chain, 15000, polls);
    const uint64_t ready_after = now_ms() - t0;
    check(e == SupervisedChain::Event::Swapped, "poll() reports the first host Ready: put node() in a plan");
    std::snprintf(line, sizeof line, "Ready after %llu ms, %llu polls returned None before it",
                  (unsigned long long)ready_after, (unsigned long long)polls);
    check(ready_after >= 400 && polls > 10, line);
    std::snprintf(line, sizeof line, "no poll() took over %llu ms (slowest %llu)", (unsigned long long)kPollBudgetMs,
                  (unsigned long long)rig.slowest_call_ms);
    check(rig.slowest_call_ms < kPollBudgetMs, line);
    check(!chain.starting() && chain.node().channel_slot != kNoSlot, "bound to a slot");
    check(chain.latency_frames() == kBlock, "the latency is read at Ready: the IPC block");
    check(chain.idle(), "idle: nothing submitted");
    chain.retired();
    return 0;
}

int scenario_ready_timeout(const Args& a) {
    std::printf("ready_timeout: %s at 3 s per block against a 300 ms Ready bound\n", kSleep);
    AsyncRig rig(a, 300);
    if (!rig.sup.ok()) {
        check(false, rig.sup.why().c_str());
        return 1;
    }
    SupervisedChain chain(rig.sup, sleep_spec(a, 3000.f), 0, 0);
    std::string error;
    check(chain.begin(error), "begin() spawned the host");
    uint64_t polls = 0;
    const uint64_t t0 = now_ms();
    const SupervisedChain::Event e = rig.until_event(chain, 5000, polls);
    const uint64_t after = now_ms() - t0;
    check(e == SupervisedChain::Event::Down, "the chain is down");
    std::printf("  why: %s (after %llu ms)\n", chain.why().c_str(), (unsigned long long)after);
    check(chain.why().find("not ready within 300 ms") != std::string::npos, "the timeout is named");
    check(after >= 250 && after < 2000, "noticed at the bound, from poll(), not after the plugin's block");
    check(rig.slowest_call_ms < kPollBudgetMs, "no poll() waited");
    check(chain.restarts() == 0, "a failed first start is not a restart");
    check(chain.node().channel_slot == kNoSlot, "no slot was taken");
    return 0;
}

int scenario_async_refused(const Args& a) {
    std::printf("async_refused: urn:sonora:test#liar writes outside its buffers in validation\n");
    AsyncRig rig(a, 10000);
    if (!rig.sup.ok()) {
        check(false, rig.sup.why().c_str());
        return 1;
    }
    SupervisedChain chain(rig.sup, mono_spec(a, "urn:sonora:test#liar"), 0, 0);
    std::string error;
    check(chain.begin(error), "begin() spawned the host");
    uint64_t polls = 0;
    const SupervisedChain::Event e = rig.until_event(chain, 10000, polls);
    check(e == SupervisedChain::Event::Down, "the refusal makes the chain down, without a restart");
    std::printf("  why: %s\n", chain.why().c_str());
    check(chain.why().find("urn:sonora:test#liar") != std::string::npos, "the refused plugin is named");
    check(chain.restarts() == 0, "a refusal is not retried");
    check(rig.slowest_call_ms < kPollBudgetMs, "no poll() waited");
    return 0;
}

// ---- HostControl (host_control.h) between the supervisor and its hosts
//
// Real hosts for what a plugin does (its key table, its refusal); this
// binary itself, run as a scripted fake host (SONORA_FAKE_HOST=<mode> in its
// environment), for what a host must not do: a Hello with another pid, a
// message only the engine sends, a GestureEnd flood (M2), a message half
// sent and never finished (M1).

namespace pc = sonora::plugin::control;

uint64_t steady_ns() { return now_ns(); }

// The fake host: --version as a real one answers it; otherwise descriptor 3
// is its socket and 4 its channel region. Ready with the IPC block as its
// latency, a Loaded with keys p0, p1, ..., then `mode`. It beats its
// heartbeat until it is told to stop.
int fake_host(int argc, char** argv, const std::string& mode) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--version") {
            char line[128];
            pc::format_host_version_line(line, sizeof line);
            std::printf("%s\n", line);
            return 0;
        }
    }
    std::string err;
    auto ctl = platform::ControlChannel::adopt(3, err);
    struct stat st {};
    if (!ctl || ::fstat(4, &st) != 0) return 2;
    void* base = ::mmap(nullptr, std::size_t(st.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, 4, 0);
    if (base == MAP_FAILED) return 2;
    plugin::ChannelHeader& h = *static_cast<plugin::ChannelHeader*>(base);
    pc::Reassembler in(pc::kMaxMessageBytes);
    const auto receive = [&](std::string& message) {
        std::string datagram;
        for (;;) {
            if (ctl->receive(datagram, pc::kMaxFrameBytes, nullptr, 5000, err) != platform::IoStatus::Ok) return false;
            switch (in.add(datagram, message, err)) {
                case pc::Reassembler::Result::Complete: return true;
                case pc::Reassembler::Result::Error: return false;
                case pc::Reassembler::Result::Partial: break;
            }
        }
    };
    const auto send = [&](const std::string& message) {
        std::vector<std::string> frames;
        if (!pc::split_frames(message, frames, err)) return;
        for (const std::string& f : frames) ctl->send(f, 1000, err);
    };
    const auto msg = [&](const auto& m) {
        std::string out;
        pc::encode(m, out);
        send(out);
    };
    // n of the same GestureEnd, or ParamTouched, at once.
    const auto burst_ends = [&](uint32_t n, uint32_t params) {
        msg(pc::GestureBegin{0, 0});
        for (uint32_t k = 0; k < n; ++k) msg(pc::GestureEnd{0, params ? k % params : 0, 0.5f});
    };
    const auto burst_touches = [&](uint32_t n) {
        msg(pc::GestureBegin{0, 0});
        for (uint32_t k = 0; k < n; ++k) msg(pc::ParamTouched{0, 0, float(k % 2)});
    };
    const auto big_state = [&](uint32_t request) {
        pc::State s;
        s.request = request;
        s.status = pc::StateStatus::Ok;
        s.bytes.assign(pc::kMaxStateBytes - 64, 'x');
        return s;
    };
    std::string m;
    pc::EngineHello ehello;
    pc::Load load;
    if (!receive(m) || !pc::decode(m, ehello, err) || !receive(m) || !pc::decode(m, load, err)) return 2;
    // #72 review item 3: a host that exits before Ready; one that never gets
    // there; one Ready the first time and then a crash, whose replacements
    // never get there (a marker file tells the runs apart).
    if (mode == "exit-early") return 4;
    bool never_ready = mode == "never-ready";
    if (mode == "ready-once") {
        const char* marker = std::getenv("SONORA_FAKE_MARKER");
        struct stat ms {};
        if (marker && ::stat(marker, &ms) == 0) {
            never_ready = true;
        } else if (marker) {
            std::ofstream(marker) << "ran\n";
        }
    }
    if (never_ready) {
        pc::Hello hello;
        hello.pid = uint64_t(::getpid());
        msg(hello);
        std::string datagram;
        while (h.shutdown.load(std::memory_order_acquire) == 0) {
            if (ctl->receive(datagram, pc::kMaxFrameBytes, nullptr, 20, err) == platform::IoStatus::Closed) break;
        }
        return 0;
    }
    // Before Hello.
    if (mode == "before-hello") msg(pc::GestureBegin{0, 0});
    pc::Hello hello;
    hello.pid = mode == "bad-pid" ? 1 : uint64_t(::getpid());
    if (mode == "bad-version") hello.wire_version = 99;
    msg(hello);
    if (mode == "two-hellos") msg(hello);
    // After Hello, before Loaded: an unsolicited 16 MiB State, three times
    // (H1 on #78); the first is the fault.
    if (mode == "state-before")
        for (int i = 0; i < 3; ++i) msg(big_state(7));
    h.latency_frames.store(h.block_frames, std::memory_order_relaxed);
    h.latency_generation.store(1, std::memory_order_release);
    h.state.store(uint32_t(plugin::HostState::Ready), std::memory_order_release);
    pc::Loaded loaded;
    loaded.latency_frames = h.block_frames;
    loaded.latency_generation = 1;
    for (const pc::LoadInstance& li : load.instances) {
        pc::LoadedInstance i;
        for (uint32_t p = 0; p < li.params; ++p) i.params.push_back({"p" + std::to_string(p), 0.f, 1.f, 0.f});
        loaded.instances.push_back(std::move(i));
    }
    if (mode == "bad-count") loaded.instances.push_back(loaded.instances.front());
    msg(loaded);
    const uint32_t params = load.instances.empty() ? 0 : load.instances.front().params;
    const uint64_t misbehave_at = steady_ns() + 300'000'000;
    bool misbehaved = false;
    uint64_t next_ms = 0;
    uint32_t sent = 0;
    // A host that never hears its Shutdown (a plugin's thread holds its main
    // loop): it beats on until it is killed.
    if (mode == "deaf") {
        for (;;) {
            h.heartbeat.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    while (h.shutdown.load(std::memory_order_acquire) == 0) {
        h.heartbeat.fetch_add(1, std::memory_order_relaxed);
        const uint64_t t = steady_ns();
        if (!misbehaved && t >= misbehave_at) {
            misbehaved = true;
            next_ms = t / 1'000'000;
            if (mode == "ready-once") {
                std::_Exit(1);  // a crash, once Ready
            } else if (mode == "bogus") {
                msg(pc::EngineHello{});  // a type only the engine sends
            } else if (mode == "flood") {
                burst_ends(10 * pc::kMaxGestureEndsPerSecond + 100, 0);
            } else if (mode == "gesture-at-limit") {
                burst_ends(10 * pc::kMaxGestureEndsPerSecond, 0);
            } else if (mode == "gesture-over") {
                burst_ends(10 * pc::kMaxGestureEndsPerSecond + 1, 0);
            } else if (mode == "preset") {
                // A preset change in the plugin's own editor: every
                // parameter ends a gesture at once.
                burst_ends(params, params);
            } else if (mode == "coalesce-newer") {
                // #78 note 1: a burst past the human rate coalesces ends of
                // parameter 0 at 0.5, then a newer gesture of the same
                // parameter touches 0.75, all in one batch.
                burst_ends(3 * pc::kMaxGestureEndsPerSecond, 0);
                msg(pc::GestureBegin{0, 0});
                msg(pc::ParamTouched{0, 0, 0.75f});
            } else if (mode == "touch-at-limit") {
                burst_touches(pc::kMaxTouchesPerSecondPerParam);
            } else if (mode == "touch-over") {
                burst_touches(pc::kMaxTouchesPerSecondPerParam + 1);
            } else if (mode == "half") {
                // The first frame of a two-frame message, and never the second.
                std::string out;
                pc::State s;
                s.bytes.assign(pc::kMaxFrameBytes * 2, 'x');
                pc::encode(s, out);
                std::vector<std::string> frames;
                pc::split_frames(out, frames, err);
                if (!frames.empty()) ctl->send(frames.front(), 1000, err);
            } else if (mode == "state-after") {
                for (int i = 0; i < 3; ++i) msg(big_state(7));
            } else if (mode == "two-loaded") {
                msg(loaded);
            } else if (mode == "refused-after-loaded") {
                msg(pc::Refused{plugin::Refusal::InstantiateFailed, 0, "late"});
            } else if (mode == "out-of-chain") {
                msg(pc::GestureBegin{5, 0});
            } else if (mode == "fd") {
                auto region = platform::SharedRegion::create("sonora-fake-fd", 4096, err);
                std::string out;
                pc::encode(pc::LatencyChanged{2, h.block_frames}, out);
                std::vector<std::string> frames;
                pc::split_frames(out, frames, err);
                if (region && !frames.empty()) ctl->send_with_region(frames.front(), *region, 1000, err);
            } else if (mode == "big-frame") {
                ctl->send(std::string(pc::kMaxFrameBytes + 4096, 'x'), 1000, err);
            }
        }
        // The legitimate rates, paced (M2 must not kill them): a knob
        // dragged at 100 touches a second for 2 s; 50 GestureEnd spread over
        // a second.
        if (misbehaved && mode == "touch-legit" && sent < 200 && t / 1'000'000 >= next_ms) {
            if (sent == 0) msg(pc::GestureBegin{0, 0});
            msg(pc::ParamTouched{0, 0, float(sent % 2)});
            ++sent;
            next_ms += 10;
        }
        if (misbehaved && mode == "gesture-legit" && sent < pc::kMaxGestureEndsPerSecond &&
            t / 1'000'000 >= next_ms) {
            msg(pc::GestureBegin{0, 0});
            msg(pc::GestureEnd{0, 0, float(sent % 2)});
            ++sent;
            next_ms += 1000 / pc::kMaxGestureEndsPerSecond;
        }
        std::string datagram;
        if (ctl->receive(datagram, pc::kMaxFrameBytes, nullptr, 2, err) == platform::IoStatus::Closed) break;
    }
    return 0;
}

struct ControlRig {
    PluginClient client;
    SupervisorConfig scfg;
    std::unique_ptr<HostSupervisor> sup;
    ControlRig(const std::string& host, const std::vector<std::string>& bundles, uint64_t ready_timeout_ms = 10000)
        : client(Rig::client_config()) {
        scfg.host_path = host;
        scfg.bundles = bundles;
        scfg.sample_rate = kRate;
        scfg.block_frames = kBlock;
        scfg.ready_timeout_ms = ready_timeout_ms;
        sup = std::make_unique<HostSupervisor>(scfg, client);
    }
    // poll() every 5 ms until an event other than None, or `max_ms`.
    static SupervisedChain::Event next_event(SupervisedChain& c, uint64_t max_ms) {
        const uint64_t end = now_ms() + max_ms;
        while (now_ms() < end) {
            const SupervisedChain::Event e = c.poll(now_ms());
            if (e != SupervisedChain::Event::None) return e;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return SupervisedChain::Event::None;
    }
};

std::string self_path() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, std::size_t(n)) : std::string();
}

ChainSpec fake_spec() {
    ChainSpec s;
    s.plugins = {"urn:sonora:fake#one"};
    s.param_counts = {2};
    s.channels = 1;
    return s;
}

// A real host's Loaded: one entry per instance, each with the stable keys
// the scan saw, in dense order (host_control.h, "Keys").
int scenario_control_loaded(const Args& a) {
    std::printf("control_loaded: %s + %s, the key table from Loaded\n", kLookahead, kCrash);
    ControlRig rig(a.host, a.bundles);
    check(rig.sup->ok(), "the host speaks layout, clock and wire versions");
    if (!rig.sup->ok()) {
        std::printf("  %s\n", rig.sup->why().c_str());
        return 1;
    }
    const Probe look = probe(a, kLookahead), crash = probe(a, kCrash);
    ChainSpec s;
    s.plugins = {kLookahead, kCrash};
    s.param_counts = {uint32_t(look.symbols.size()), uint32_t(crash.symbols.size())};
    s.channels = 1;
    SupervisedChain chain(*rig.sup, s, 0, 0);
    std::string error;
    check(chain.start(error), "the chain comes up Ready");
    if (!chain.host()) return 1;
    const pc::Loaded& l = chain.host()->loaded();
    check(l.instances.size() == 2, "Loaded has one entry per instance");
    bool keys = l.instances.size() == 2 && l.instances[0].params.size() == look.symbols.size() &&
                l.instances[1].params.size() == crash.symbols.size();
    for (std::size_t i = 0; keys && i < look.symbols.size(); ++i) keys = l.instances[0].params[i].key == look.symbols[i];
    for (std::size_t i = 0; keys && i < crash.symbols.size(); ++i) keys = l.instances[1].params[i].key == crash.symbols[i];
    check(keys, "each instance's keys are the scan's symbols, in dense order");
    check(l.instances.size() == 2 && l.instances[0].latency_frames == 100, "the look-ahead's own latency, 100");
    check(chain.latency_frames() == 100 + kBlock, "the region's figure: 100 + the IPC block");
    chain.poll(now_ms());
    check(chain.host()->protocol_fault().empty(), "no protocol fault");
    return 0;
}

// A refusal names its instance, so the control server can leave it out and
// start the rest (ADR-0012 amendment of 2026-10-14, change C).
int scenario_control_refused_instance(const Args& a) {
    std::printf("control_refused_instance: %s then urn:sonora:test#liar\n", kCrash);
    ControlRig rig(a.host, a.bundles);
    if (!rig.sup->ok()) return 1;
    ChainSpec s;
    s.plugins = {kCrash, "urn:sonora:test#liar"};
    s.param_counts = {uint32_t(probe(a, kCrash).symbols.size()), uint32_t(probe(a, "urn:sonora:test#liar").symbols.size())};
    s.channels = 1;
    SupervisedChain chain(*rig.sup, s, 0, 0);
    std::string error;
    check(!chain.start(error), "the chain does not come up");
    std::printf("  why: %s\n", chain.why().c_str());
    check(chain.down() && chain.refused(), "down, as a refusal");
    check(chain.refused_instance() == 1, "the refusal names instance 1, the liar");
    // L1 on #78: the Refused message's own text, which follows the region's
    // word, is kept rather than lost to the race.
    check(chain.why().find("declares") != std::string::npos, "the host's refusal text is kept");
    check(chain.restarts() == 0, "not retried");
    return 0;
}

// A Hello whose pid is not the process the engine started: refused before
// Ready (host_control.h, "What the engine checks").
int scenario_control_bad_hello(const Args&) {
    std::printf("control_bad_hello: a fake host says another pid\n");
    ::setenv("SONORA_FAKE_HOST", "bad-pid", 1);
    ControlRig rig(self_path(), {});
    if (!rig.sup->ok()) {
        std::printf("  %s\n", rig.sup->why().c_str());
        return 1;
    }
    SupervisedChain chain(*rig.sup, fake_spec(), 0, 0);
    std::string error;
    check(chain.begin(error), "spawned");
    const SupervisedChain::Event e = ControlRig::next_event(chain, 5000);
    std::printf("  why: %s\n", chain.why().c_str());
    check(e == SupervisedChain::Event::Down, "down at start-up");
    check(chain.why().find("pid") != std::string::npos, "the pid is named");
    check(!chain.refused(), "a broken contract, not the plugin's refusal");
    return 0;
}

// A running host that breaks the contract is killed and restarted (the crash
// path, §5.3): a type only the engine sends; a GestureEnd flood past
// kMaxGestureEndsPerSecond (M2); a message half sent for kStalledMessageMs
// (M1).
int fake_misbehaves(const char* mode, const char* named, uint64_t max_ms) {
    ::setenv("SONORA_FAKE_HOST", mode, 1);
    ControlRig rig(self_path(), {});
    if (!rig.sup->ok()) {
        std::printf("  %s\n", rig.sup->why().c_str());
        return 1;
    }
    SupervisedChain chain(*rig.sup, fake_spec(), 0, 0);
    std::string error;
    check(chain.begin(error), "spawned");
    check(ControlRig::next_event(chain, 5000) == SupervisedChain::Event::Swapped, "Ready and bound");
    chain.retired();
    const uint64_t t0 = now_ms();
    const SupervisedChain::Event e = ControlRig::next_event(chain, max_ms);
    std::printf("  why: %s (after %llu ms)\n", chain.why().c_str(), (unsigned long long)(now_ms() - t0));
    check(e == SupervisedChain::Event::Restarting, "killed, and a replacement starting");
    check(chain.why().find(named) != std::string::npos, "the fault is named");
    return 0;
}

// A fake host that breaks the contract before it is Ready: the chain is
// down at start-up, named, and it is not taken for the plugin's refusal.
int fake_fails_start(const char* mode, const char* named) {
    ::setenv("SONORA_FAKE_HOST", mode, 1);
    ControlRig rig(self_path(), {});
    if (!rig.sup->ok()) {
        std::printf("  %s\n", rig.sup->why().c_str());
        return 1;
    }
    SupervisedChain chain(*rig.sup, fake_spec(), 0, 0);
    std::string error;
    check(chain.begin(error), "spawned");
    const SupervisedChain::Event e = ControlRig::next_event(chain, 8000);
    std::printf("  why: %s\n", chain.why().c_str());
    check(e == SupervisedChain::Event::Down, "down at start-up");
    check(chain.why().find(named) != std::string::npos, "the fault is named");
    check(!chain.refused(), "a broken contract, not the plugin's refusal");
    return 0;
}

// A fake host sending at a legitimate rate, or a burst at the limit: it
// stays up for `ms` after Ready, its messages taken every poll as the
// control server takes them. Returns what it sent.
// `stall_ms`: once, after the first poll, the control thread stalls that
// long (a slow save), so what the host sent meanwhile arrives in one pump.
int fake_survives(const char* mode, uint64_t ms, ChainSpec spec, std::vector<std::string>* taken = nullptr,
                  uint64_t* coalesced = nullptr, uint64_t stall_ms = 0) {
    ::setenv("SONORA_FAKE_HOST", mode, 1);
    ControlRig rig(self_path(), {});
    if (!rig.sup->ok()) {
        std::printf("  %s\n", rig.sup->why().c_str());
        return 1;
    }
    SupervisedChain chain(*rig.sup, std::move(spec), 0, 0);
    std::string error;
    check(chain.begin(error), "spawned");
    check(ControlRig::next_event(chain, 5000) == SupervisedChain::Event::Swapped, "Ready and bound");
    chain.retired();
    const uint64_t end = now_ms() + ms;
    SupervisedChain::Event e = SupervisedChain::Event::None;
    while (now_ms() < end && e == SupervisedChain::Event::None) {
        e = chain.poll(now_ms());
        std::vector<std::string> m = chain.take_host_messages();
        if (taken) taken->insert(taken->end(), m.begin(), m.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms ? stall_ms : 5));
        stall_ms = 0;
    }
    if (e != SupervisedChain::Event::None) std::printf("  why: %s\n", chain.why().c_str());
    check(e == SupervisedChain::Event::None && chain.host() && chain.host()->running(), "the host is still up");
    if (coalesced && chain.host()) *coalesced = chain.host()->gesture_ends_coalesced();
    return 0;
}

int scenario_control_bogus(const Args&) {
    std::printf("control_bogus: a fake host sends EngineHello once running\n");
    return fake_misbehaves("bogus", "protocol", 3000);
}

// M2 on #78: a GestureEnd burst past ten times the human limit in one
// second (here 600) is a host gone wrong; exactly that many (500) is not,
// and 501 is.
int scenario_control_flood(const Args&) {
    std::printf("control_flood: GestureEnd bursts at, over, and far over the kill threshold\n");
    fake_misbehaves("flood", "GestureEnd", 3000);
    std::printf("  at the threshold, %u:\n", 10 * pc::kMaxGestureEndsPerSecond);
    uint64_t coalesced = 0;
    fake_survives("gesture-at-limit", 1500, fake_spec(), nullptr, &coalesced);
    check(coalesced > 0, "the burst past the human rate was coalesced, not refused");
    std::printf("  over it by one:\n");
    return fake_misbehaves("gesture-over", "GestureEnd", 3000);
}

// #78 note 1: a GestureEnd a burst coalesced, then a newer gesture of the
// same parameter in the same batch. The coalesced end is handed over before
// the newer touch, so the last value the control server sees for the
// parameter is the newer one.
int scenario_control_coalesce_order(const Args&) {
    std::printf("control_coalesce_order: coalesced ends of p0 at 0.5, then a newer touch at 0.75, one batch\n");
    std::vector<std::string> taken;
    uint64_t coalesced = 0;
    fake_survives("coalesce-newer", 1200, fake_spec(), &taken, &coalesced, 800);
    check(coalesced > 0, "the burst was coalesced");
    float last = -1.f;
    uint32_t ends = 0;
    for (const std::string& m : taken) {
        pc::Type t{};
        std::string e;
        if (!pc::peek_type(m, t)) continue;
        if (t == pc::Type::GestureEnd) {
            pc::GestureEnd g;
            if (pc::decode(m, g, e) && g.instance == 0 && g.param == 0) {
                last = g.value;
                ++ends;
            }
        } else if (t == pc::Type::ParamTouched) {
            pc::ParamTouched p;
            if (pc::decode(m, p, e) && p.instance == 0 && p.param == 0) last = p.value;
        }
    }
    std::printf("  %u ends of p0 handed over, last value %.2f\n", ends, double(last));
    check(ends > 3 * pc::kMaxGestureEndsPerSecond - uint32_t(coalesced), "the coalesced end was handed over");
    check(last == 0.75f, "the newer touch is the last value of p0, not the coalesced end");
    return 0;
}

// #78 note 2: the control thread stalls 1.5 s while a knob is dragged at
// 100 touches a second; the backlog arrives in one pump and the token
// bucket's stall headroom lets it through.
int scenario_control_stalled_touches(const Args&) {
    std::printf("control_stalled_touches: 100 touches/s while the control thread stalls 1.5 s\n");
    std::vector<std::string> taken;
    fake_survives("touch-legit", 2600, fake_spec(), &taken, nullptr, 1500);
    uint32_t touches = 0;
    for (const std::string& m : taken) {
        pc::Type t{};
        if (pc::peek_type(m, t) && t == pc::Type::ParamTouched) ++touches;
    }
    std::printf("  %u touches taken\n", touches);
    check(touches >= 150, "the backlog and the rest were taken, none refused");
    return 0;
}

int scenario_control_half(const Args&) {
    std::printf("control_half: a fake host sends the first frame of two and stops\n");
    return fake_misbehaves("half", "half received", uint64_t(pc::kStalledMessageMs) + 3000);
}

// M2 on #78: the legitimate rates survive, and a preset change (every
// parameter ending a gesture at once) is coalesced, the newest value of
// each parameter handed over, none lost.
int scenario_control_rates(const Args&) {
    std::printf("control_rates: a knob at 100 touches/s for 2 s, 50 GestureEnd over 1 s, a 512-parameter preset\n");
    fake_survives("touch-legit", 2600, fake_spec());
    uint64_t coalesced = 0;
    fake_survives("gesture-legit", 1600, fake_spec(), nullptr, &coalesced);
    check(coalesced == 0, "50 GestureEnd over a second: none coalesced");
    ChainSpec preset = fake_spec();
    preset.param_counts = {512};
    std::vector<std::string> taken;
    fake_survives("preset", 1000, preset, &taken, &coalesced);
    std::vector<int> seen(512, 0);
    for (const std::string& m : taken) {
        pc::GestureEnd g;
        std::string e;
        if (pc::decode(m, g, e) && g.instance == 0 && g.param < 512) ++seen[g.param];
    }
    check(std::all_of(seen.begin(), seen.end(), [](int n) { return n == 1; }),
          "every one of the 512 parameters handed over once");
    // A burst read over a few pumps refills a token or two meanwhile.
    check(coalesced <= 512 - pc::kMaxGestureEndsPerSecond && coalesced + 10 >= 512 - pc::kMaxGestureEndsPerSecond,
          "past the human rate, coalesced and counted");
    std::printf("  touches at the limit, %u:\n", pc::kMaxTouchesPerSecondPerParam);
    fake_survives("touch-at-limit", 800, fake_spec());
    std::printf("  touches over it by one:\n");
    return fake_misbehaves("touch-over", "ParamTouched", 3000);
}

// H1 on #78: a 16 MiB State nobody asked for, before Loaded and after,
// three times over: the first one is the fault, and what the engine holds
// stays bounded (no queue of them).
long rss_kib() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("VmRSS:", 0) == 0) return std::atol(line.c_str() + 6);
    return 0;
}

int scenario_control_unsolicited(const Args&) {
    std::printf("control_unsolicited: a 16 MiB State nobody asked for, before and after Loaded\n");
    const long before = rss_kib();
    fake_fails_start("state-before", "answering no GetState");
    fake_misbehaves("state-after", "answering no GetState", 5000);
    const long grew = rss_kib() - before;
    char line[128];
    std::snprintf(line, sizeof line, "memory stayed bounded: RSS grew %ld KiB for 6 x 16 MiB sent", grew);
    check(grew < 64 * 1024, line);
    return 0;
}

// The contract's other checks, each a few lines of the fake host (test
// adequacy on #78).
int scenario_control_breaches(const Args&) {
    std::printf("control_breaches: what a host must not send\n");
    fake_fails_start("before-hello", "before Hello");
    fake_fails_start("two-hellos", "second Hello");
    fake_fails_start("bad-version", "not this engine's");
    fake_fails_start("bad-count", "instances for a chain");
    fake_misbehaves("two-loaded", "second Loaded", 3000);
    fake_misbehaves("refused-after-loaded", "Refused after the answer", 3000);
    fake_misbehaves("out-of-chain", "no parameter of the chain", 3000);
    fake_misbehaves("fd", "descriptor", 3000);
    return fake_misbehaves("big-frame", "longer than", 3000);
}

// #72 review item 3, the async start's other ends: a host that exits before
// Ready is a failed start, noticed from poll(); a replacement that never
// gets to Ready leaves the chain down at the bound; a chain destroyed while
// its host is still starting kills it at once.
int scenario_control_start_paths(const Args&) {
    std::printf("control_start_paths: exits before Ready, a replacement never Ready, destroyed while starting\n");
    fake_fails_start("exit-early", "before it was ready");

    const std::string marker = "/tmp/sonora-fake-marker-" + std::to_string(::getpid());
    std::remove(marker.c_str());
    ::setenv("SONORA_FAKE_MARKER", marker.c_str(), 1);
    ::setenv("SONORA_FAKE_HOST", "ready-once", 1);
    {
        ControlRig rig(self_path(), {}, 600);
        if (!rig.sup->ok()) return 1;
        SupervisedChain chain(*rig.sup, fake_spec(), 0, 0);
        std::string error;
        check(chain.begin(error), "spawned");
        check(ControlRig::next_event(chain, 5000) == SupervisedChain::Event::Swapped, "the first host Ready");
        chain.retired();
        check(ControlRig::next_event(chain, 3000) == SupervisedChain::Event::Restarting, "it crashed: restarting");
        const SupervisedChain::Event e = ControlRig::next_event(chain, 5000);
        std::printf("  why: %s\n", chain.why().c_str());
        check(e == SupervisedChain::Event::Down && chain.why().find("not ready within 600 ms") != std::string::npos,
              "the replacement never Ready: down at the bound, named");
        check(!chain.refused(), "not a refusal");
    }
    std::remove(marker.c_str());

    ::setenv("SONORA_FAKE_HOST", "never-ready", 1);
    ControlRig rig(self_path(), {});
    if (!rig.sup->ok()) return 1;
    uint64_t took = 0;
    {
        auto chain = std::make_unique<SupervisedChain>(*rig.sup, fake_spec(), 0, 0);
        std::string error;
        check(chain->begin(error), "spawned");
        for (int i = 0; i < 20; ++i) {
            chain->poll(now_ms());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(chain->starting(), "still starting");
        const uint64_t t0 = now_ms();
        chain.reset();
        took = now_ms() - t0;
    }
    char line[96];
    std::snprintf(line, sizeof line, "destroyed while starting in %llu ms, its host killed, not waited for",
                  (unsigned long long)took);
    check(took < 1000, line);
    return 0;
}

// The destructor of a chain whose owner did not ask first never waits: a
// host deaf to its Shutdown cost it 2 s in HostProcess::stop(). The host
// goes to the supervisor, whose reap() kills it past the grace. The owner
// that asks first (request_stop, stopped) leaves the supervisor nothing.
int scenario_control_stop_paths(const Args&) {
    std::printf("control_stop_paths: a deaf host, destroyed bound without a stop, and stopped first\n");
    ::setenv("SONORA_FAKE_HOST", "deaf", 1);
    ControlRig rig(self_path(), {});
    if (!rig.sup->ok()) return 1;
    {
        auto chain = std::make_unique<SupervisedChain>(*rig.sup, fake_spec(), 0, 0);
        std::string error;
        check(chain->begin(error), "spawned");
        check(ControlRig::next_event(*chain, 5000) == SupervisedChain::Event::Swapped, "Ready and bound");
        const uint64_t t0 = now_ms();
        chain.reset();
        const uint64_t took = now_ms() - t0;
        char line[96];
        std::snprintf(line, sizeof line, "destroyed bound in %llu ms, its deaf host not waited for",
                      (unsigned long long)took);
        check(took < 500, line);
        check(rig.sup->adopted() == 1, "the supervisor holds the host still running");
        const uint64_t end = now_ms() + 5000;
        bool reaped = false;
        while (!reaped && now_ms() < end) {
            reaped = rig.sup->reap(now_ms());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(reaped && rig.sup->adopted() == 0, "reap() killed it past the grace and reaped it");
    }
    {
        auto chain = std::make_unique<SupervisedChain>(*rig.sup, fake_spec(), 0, 0);
        std::string error;
        check(chain->begin(error), "spawned again");
        check(ControlRig::next_event(*chain, 5000) == SupervisedChain::Event::Swapped, "Ready and bound again");
        chain->request_stop(now_ms());
        const uint64_t end = now_ms() + 5000;
        bool stopped = false;
        while (!stopped && now_ms() < end) {
            stopped = chain->stopped(now_ms());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(stopped, "asked first: stopped() once the kill past the grace is reaped");
        chain.reset();
        check(rig.sup->adopted() == 0, "asked first: nothing left for the supervisor");
    }
    return 0;
}

// M3 on #78: what the control thread sleeps on. A Ready host lists its exit
// handle, and its socket only while frames wait to be sent. A deaf host's
// full socket does not end the sleep; its kill does, at once, and a host
// that has exited lists nothing more, so the turn after the wake cannot be
// woken again by the same exit.
int scenario_control_watch(const Args&) {
    std::printf("control_watch: the exit handle and the socket the control thread sleeps on\n");
    ::setenv("SONORA_FAKE_HOST", "deaf", 1);
    ControlRig rig(self_path(), {});
    if (!rig.sup->ok()) return 1;
    const auto listed = [&rig]() {
        std::vector<HostWatch> w;
        rig.sup->watch(w);
        return w;
    };
    const auto as_watches = [](const std::vector<HostWatch>& hw) {
        std::vector<platform::WaitSet::Watch> w;
        for (const HostWatch& h : hw)
            w.push_back({h.handle, h.writable ? platform::WaitSet::When::Writable : platform::WaitSet::When::Readable});
        return w;
    };
    {
        SupervisedChain chain(*rig.sup, fake_spec(), 0, 0);
        std::string error;
        check(chain.begin(error), "spawned");
        check(ControlRig::next_event(chain, 5000) == SupervisedChain::Event::Swapped, "Ready and bound");
        HostProcess* h = chain.host();
        if (!h) return 1;
        check(rig.sup->live_hosts() == 1, "the supervisor knows one host");
        if (h->exit_handle() < 0) {
            std::printf("  no pidfd on this kernel: the rest is skipped\n");
            return 0;
        }
        std::vector<HostWatch> w = listed();
        check(w.size() == 1 && w[0].handle == h->exit_handle() && !w[0].writable,
              "idle: its exit handle only, the socket not listed");

        // The deaf host reads nothing more, so its socket fills.
        std::string bytes;
        pc::encode(pc::Shutdown{}, bytes);
        for (int i = 0; i < 100000 && !h->wants_write(); ++i) {
            if (!h->send(bytes, error)) break;
            h->pump(now_ms());
        }
        check(h->wants_write(), "frames wait on its full socket");
        w = listed();
        bool socket = false;
        for (const HostWatch& e : w) socket = socket || (e.handle == h->socket_handle() && e.writable);
        check(w.size() == 2 && socket, "listed: the exit handle, and the socket for writing");

        platform::WaitSet sleep;
        const std::vector<platform::WaitSet::Watch> ws = as_watches(w);
        check(sleep.wait(ws.data(), ws.size(), 200) == platform::WaitSet::Woke::Timeout,
              "a full socket and a running host do not end the sleep");
        const uint64_t t0 = now_ms();
        h->signal_kill();
        const platform::WaitSet::Woke woke = sleep.wait(ws.data(), ws.size(), 5000);
        const uint64_t took = now_ms() - t0;
        char line[96];
        std::snprintf(line, sizeof line, "its kill ends the sleep in %llu ms, not at the timeout",
                      (unsigned long long)took);
        check(woke == platform::WaitSet::Woke::Handle && took < 2000, line);
        const uint64_t end = now_ms() + 5000;
        while (h->running() && now_ms() < end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(listed().empty(), "exited and reaped: nothing listed, so the same exit cannot wake twice");
    }
    check(rig.sup->live_hosts() == rig.sup->adopted(), "a destroyed host leaves the supervisor's list");
    return 0;
}

// The #198 review's WaitSet spin: a chain its owner no longer polls while
// plans still name its slots (the control server's retiring chains, until
// their epoch passes). Its restart, still starting, has a multi-MB Load
// queued on a socket that can take more; listed Writable with nobody to
// write, it ended every sleep at once and the control thread spun.
// poll_parked() is that owner's turn: the sleep holds.
int scenario_control_parked(const Args&) {
    std::printf("control_parked: a retiring chain's starting host does not end every sleep\n");
    const std::string marker = "/tmp/sonora-fake-marker-" + std::to_string(::getpid());
    std::remove(marker.c_str());
    ::setenv("SONORA_FAKE_MARKER", marker.c_str(), 1);
    ::setenv("SONORA_FAKE_HOST", "ready-once", 1);
    int r = 0;
    {
        ControlRig rig(self_path(), {});
        if (!rig.sup->ok()) return 1;
        ChainSpec spec = fake_spec();
        spec.states = {std::string(std::size_t{8} << 20, 'x')};
        SupervisedChain chain(*rig.sup, spec, 0, 0);
        std::string error;
        check(chain.begin(error), "spawned");
        check(ControlRig::next_event(chain, 5000) == SupervisedChain::Event::Swapped, "the first host Ready");
        check(ControlRig::next_event(chain, 3000) == SupervisedChain::Event::Restarting, "it crashed: restarting");
        check(chain.starting(), "the replacement starting, its Load queued and not yet sent");
        platform::WaitSet sleep;
        uint32_t wakes = 0, turns = 0;
        const uint64_t end = now_ms() + 1000;
        while (now_ms() < end) {
            std::vector<HostWatch> hw;
            rig.sup->watch(hw);
            std::vector<platform::WaitSet::Watch> ws;
            for (const HostWatch& h : hw)
                ws.push_back(
                    {h.handle, h.writable ? platform::WaitSet::When::Writable : platform::WaitSet::When::Readable});
            if (sleep.wait(ws.data(), ws.size(), 100) != platform::WaitSet::Woke::Timeout) ++wakes;
            chain.poll_parked(now_ms());
            ++turns;
        }
        char line[128];
        std::snprintf(line, sizeof line, "%u wakes in %u turns over 1 s: the sleep holds", wakes, turns);
        check(wakes <= 20, line);
        check(!chain.starting(), "the replacement, which no plan names, let go");
        chain.request_stop(now_ms());
        const uint64_t stop_end = now_ms() + 5000;
        bool stopped = false;
        while (!stopped && now_ms() < stop_end) {
            stopped = chain.stopped(now_ms());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(stopped, "then stopped once its epoch has passed");
        if (wakes > 20) r = 1;
    }
    std::remove(marker.c_str());
    return r;
}

// The #191 review: a Ready host bind() refuses (no free channel slot) is
// let go and killed, not destroyed on the control thread, where
// ~HostProcess's stop() gave a host deaf to its Shutdown 2 s. And
// ~HostProcess itself is one SIGKILL and a bounded reap.
int scenario_control_no_slot(const Args&) {
    std::printf("control_no_slot: a Ready host with no slot, and a deaf host destroyed\n");
    ::setenv("SONORA_FAKE_HOST", "deaf", 1);
    ControlRig rig(self_path(), {});
    if (!rig.sup->ok()) return 1;
    while (rig.client.acquire_slot() != kNoSlot) {
    }
    {
        auto chain = std::make_unique<SupervisedChain>(*rig.sup, fake_spec(), 0, 0);
        std::string error;
        check(chain->begin(error), "spawned");
        SupervisedChain::Event e = SupervisedChain::Event::None;
        uint64_t longest = 0;
        const uint64_t end = now_ms() + 5000;
        while (e == SupervisedChain::Event::None && now_ms() < end) {
            const uint64_t t0 = now_ms();
            e = chain->poll(t0);
            longest = std::max(longest, now_ms() - t0);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::printf("  why: %s\n", chain->why().c_str());
        check(e == SupervisedChain::Event::Down && chain->why().find("no free channel slot") != std::string::npos,
              "Ready, no slot: down, named");
        char line[96];
        std::snprintf(line, sizeof line, "the longest poll took %llu ms, its deaf host not waited for",
                      (unsigned long long)longest);
        check(longest < 500, line);
        chain->request_stop(now_ms());
        const uint64_t stop_end = now_ms() + 5000;
        bool stopped = false;
        while (!stopped && now_ms() < stop_end) {
            stopped = chain->stopped(now_ms());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(stopped, "the host let go is reaped");
        chain.reset();
        check(rig.sup->live_hosts() == 0 && rig.sup->adopted() == 0, "nothing left");
    }
    {
        std::string error;
        std::unique_ptr<HostProcess> h = rig.sup->start(fake_spec(), error);
        check(h != nullptr, "a deaf host Ready");
        const uint64_t t0 = now_ms();
        h.reset();
        const uint64_t took = now_ms() - t0;
        char line[96];
        std::snprintf(line, sizeof line, "destroyed in %llu ms: SIGKILL and a bounded reap, no 2 s Shutdown grace",
                      (unsigned long long)took);
        check(took < 500, line);
    }
    return 0;
}

// M1 on #78: the state caps are checked before anything is copied, encoded
// or spawned. One state over 16 MiB is that instance's refusal; together
// over 64 MiB, the chain's; neither starts a host or retries.
int scenario_control_load_cap(const Args&) {
    std::printf("control_load_cap: states over 16 MiB, and over 64 MiB together\n");
    ::setenv("SONORA_FAKE_HOST", "idle", 1);
    ControlRig rig(self_path(), {});
    if (!rig.sup->ok()) return 1;
    ChainSpec one;
    one.plugins = {"urn:sonora:fake#one", "urn:sonora:fake#two"};
    one.param_counts = {2, 2};
    one.states = {std::string(), std::string(pc::kMaxStateBytes + 1, 'x')};
    std::string error;
    int32_t refused = -1;
    uint64_t t0 = now_ms();
    check(rig.sup->spawn(one, error, &refused) == nullptr, "a state over 16 MiB: not spawned");
    std::printf("  why: %s (%llu ms)\n", error.c_str(), (unsigned long long)(now_ms() - t0));
    check(refused == 1 && error.find("urn:sonora:fake#two") != std::string::npos, "its instance named");
    ChainSpec all;
    all.plugins = {"urn:sonora:fake#a", "urn:sonora:fake#b", "urn:sonora:fake#c", "urn:sonora:fake#d",
                   "urn:sonora:fake#e"};
    all.param_counts = {1, 1, 1, 1, 1};
    all.states.assign(5, std::string(pc::kMaxStateBytes - 1024, 'x'));
    error.clear();
    t0 = now_ms();
    check(rig.sup->spawn(all, error, &refused) == nullptr, "five 16 MiB states: not spawned");
    std::printf("  why: %s (%llu ms)\n", error.c_str(), (unsigned long long)(now_ms() - t0));
    check(refused == -1 && error.find("exceed the 64 MiB Load cap") != std::string::npos, "the chain's cap named");
    SupervisedChain chain(*rig.sup, one, 0, 0);
    check(!chain.begin(error) && chain.down() && chain.refused() && chain.refused_instance() == 1,
          "a chain with it is down at once, as that instance's refusal");
    check(chain.poll(now_ms()) == SupervisedChain::Event::None && chain.restarts() == 0, "and never retried");
    return 0;
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

// ADR-0012 amendment 2026-11-01, end to end: a keyed chain's node names
// the split, the key reaches the plugin's lv2:isSideChain port through the
// region's last plane, and what plays is the signal output only.
// sonora_test#key outputs its key, so block k plays key block k - 1.
int scenario_keyed(const Args& a) {
    std::printf("keyed: %s, a mono chain with a mono key\n", kKey);
    ChainSpec spec = mono_spec(a, kKey);
    spec.key_channels = 1;
    Rig rig(a, spec, 48);
    if (!rig.ready) return 1;
    check(rig.session.ref.channels == 1 && rig.session.ref.key_channels == 1, "the node names 1 signal + 1 key");
    fill_noise(rig.session, 5);
    uint32_t x = 99;
    for (float& v : rig.session.key[0]) {
        x = x * 1103515245u + 12345u;
        v = float(int32_t(x >> 9) % 800) / 1600.f;
    }
    rig.run_waited(48);
    bool exact = true;
    for (uint64_t k = 2; k < 48 && exact; ++k) {
        const float* o = rig.session.out_block(0, k);
        const float* want = rig.session.key[0].data() + (k - 1) * kBlock;
        for (uint32_t f = 0; f < kBlock; ++f)
            if (o[f] != want[f]) {
                std::printf("  block %llu frame %u: %.9g, want the key's %.9g\n", (unsigned long long)k, f,
                            double(o[f]), double(want[f]));
                exact = false;
                break;
            }
    }
    check(exact, "the plugin hears the key, one block late, bit for bit");
    const SlotStatus st = rig.client.slot_status(rig.session.ref.channel_slot);
    check(st.collected == 47 && st.misses == 0, "47 collected, no miss");
    check(rig.client.node_status(rig.session.ref.node_state).shape_faults == 0, "no shape fault");
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
    check(st.misses == kMaxConsecutiveMisses, "8 misses counted, the late host's second block included");

    rig.session.send(0, 0.f);  // into the store: the new host gets it on its first submit
    rig.run_paced(1);
    check(rig.until_event(10000) == SupervisedChain::Event::Swapped, "the supervisor restarted the chain");
    check(rig.restarting == 1 && rig.old_slot_stayed,
          "Restarting first, the old slot still in node() until the new host was Ready (make-before-break)");
    check(rig.slowest_poll_ms < kPollBudgetMs, "no poll() waited for the replacement");
    // Sampled by the control thread from the host's current_instance; the
    // audio thread never reads it (auditor L1 on PR #49).
    check(rig.chain->culprit() == 0, "the control thread names the culprit: instance 0, the sleeping plugin");
    std::printf("  why: %s\n", rig.chain->why().c_str());
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
    // Bit-exact, not the fixture's tolerance (code-reviewer L7 on PR #49):
    // the LV2 bundle runs the same Processor code the N0 render did, on the
    // same machine, so every sample must match. The fixture's tolerance
    // exists for its VST3 cases and is not used.
    std::snprintf(what, sizeof what, "%llu blocks one block late equal the N0 render bit for bit (max |diff| %.3g)",
                  (unsigned long long)blocks, worst);
    (void)tolerance;
    check(worst == 0.0, what);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    // Run by a supervisor as a scripted host (the control_* scenarios).
    if (const char* mode = std::getenv("SONORA_FAKE_HOST")) {
        bool as_host = false;
        for (int i = 1; i < argc; ++i) {
            const std::string k = argv[i];
            as_host = as_host || k == "--control-fd" || k == "--version";
        }
        if (as_host) return fake_host(argc, argv, mode);
    }
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
    if (a.scenario == "control_loaded") r = scenario_control_loaded(a);
    else if (a.scenario == "control_refused_instance") r = scenario_control_refused_instance(a);
    else if (a.scenario == "control_bad_hello") r = scenario_control_bad_hello(a);
    else if (a.scenario == "control_bogus") r = scenario_control_bogus(a);
    else if (a.scenario == "control_flood") r = scenario_control_flood(a);
    else if (a.scenario == "control_half") r = scenario_control_half(a);
    else if (a.scenario == "control_coalesce_order") r = scenario_control_coalesce_order(a);
    else if (a.scenario == "control_stalled_touches") r = scenario_control_stalled_touches(a);
    else if (a.scenario == "control_rates") r = scenario_control_rates(a);
    else if (a.scenario == "control_unsolicited") r = scenario_control_unsolicited(a);
    else if (a.scenario == "control_breaches") r = scenario_control_breaches(a);
    else if (a.scenario == "control_load_cap") r = scenario_control_load_cap(a);
    else if (a.scenario == "control_start_paths") r = scenario_control_start_paths(a);
    else if (a.scenario == "control_stop_paths") r = scenario_control_stop_paths(a);
    else if (a.scenario == "control_watch") r = scenario_control_watch(a);
    else if (a.scenario == "control_parked") r = scenario_control_parked(a);
    else if (a.scenario == "control_no_slot") r = scenario_control_no_slot(a);
    else if (a.scenario == "async_start") r = scenario_async_start(a);
    else if (a.scenario == "ready_timeout") r = scenario_ready_timeout(a);
    else if (a.scenario == "async_refused") r = scenario_async_refused(a);
    else if (a.scenario == "passthrough") r = scenario_passthrough(a);
    else if (a.scenario == "params") r = scenario_params(a);
    else if (a.scenario == "keyed") r = scenario_keyed(a);
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
