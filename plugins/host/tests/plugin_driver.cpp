// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plays the engine's side of the plugin boundary (contracts/plugin, layout
// version 1, ADR-0012 §5) against a real sonora-plugin-host process and
// real LV2 and VST3 plugins, until engine/plugin_client exists. Linux-only,
// like the host.
//
// The "callback" here is a periodic SCHED_FIFO loop (when the container
// allows it) at 128 frames / 48 kHz, the same shape as the measurement
// harness in brain/research/adr-0007-measurements.md. It follows the rules
// the engine will: pipeline depth 1, never write a slot the host may be
// reading, never wait on the host at all, drop and count instead; the
// callback's start goes into the session clock, and the tick moves after
// the submits. Hosts are timer-paced by default (the futex wake ships gated
// off, kHostWakeEnabled); --wake futex runs them on the wake instead.
//
// Like the engine's control thread it probes each plugin first
// (`sonora-plugin-host --probe`) for its dense parameter list, seeds the
// last-value table with the defaults, and keeps the authoritative values in
// a private store that a new host's first submit copies in.
//
//   plugin_driver --host EXE --bundle DIR... --scenario NAME [options]
//
// Scenarios: gain, latency, latency_change, crash, deadline, nonfinite,
// refusals (--format lv2|vst3), incompatible, bypass, swap, rtt, params and
// list_json (VST3), key (the key channels reach every plugin unchanged, and
// a key input with no key routed reads the plugin's own input), and render
// (an offline run checked against a reference render, for Sonora's own
// bundles; --args-file). A render may route a key to the plugin's
// side-chain (--key, host --sidechain), move a parameter through the ring
// before a given block (--change), and have the host re-activate the plugin
// after loading (--reactivate, for values that change its latency).
// Nothing waits on the host except rtt, key and render, which are
// measurements.
// Exit: 0 pass, 1 fail, 77 a required plugin is not installed (a failure in
// ctest unless SONORA_PLUGIN_TESTS_ALLOW_SKIP is ON).

#include "ipc.h"
#include "sonora/platform/ipc.h"
#include "sonora/platform/wake.h"
#include "sonora/plugin/host_control.h"
#include "sonora/plugin/protocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace sonora;
using namespace sonora::plugin_host;

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kBlock = 128;
constexpr uint32_t kMaxFrames = 1024;
constexpr uint64_t kPeriodNs = uint64_t(kBlock) * 1000000000ull / kRate;  // 2 666 666
// The measurement harnesses' budget for the host (rtt): what is left of the
// period after the engine's own work. 75 % is an assumption, not a decision.
constexpr uint64_t kDeadlineNs = kPeriodNs * 3 / 4;
// A host that misses this many blocks in a row leaves the plan (ADR-0012 §3).
constexpr int kMaxConsecutiveMisses = 8;
constexpr int kSkip = 77;
// Callbacks allowed over the period in a few hundred, for the OS alone. In
// the container a 20 000-block run had one, a timed wait that returned 5.9 ms
// past its timeout: the VM not running the driver, not the driver waiting on
// a plugin. A driver that did wait on a sleeping or dead host overruns every
// callback from then on, which this still catches.
constexpr uint64_t kOsNoiseCallbacks = 2;

const char* const kEgAmp = "http://lv2plug.in/plugins/eg-amp";
const char* const kDpl = "http://gareus.org/oss/lv2/dpl#mono";
const char* const kVst3Crash = "vst3:534F4E4F524156535433544553540001";
const char* const kVst3StereoOnly = "vst3:534F4E4F524156535433544553540002";
const char* const kVst3YesMan = "vst3:534F4E4F524156535433544553540003";
const char* const kVst3Liar = "vst3:534F4E4F524156535433544553540004";
const char* const kVst3Turncoat = "vst3:534F4E4F524156535433544553540005";
const char* const kVst3Params = "vst3:534F4E4F524156535433544553540006";
const char* const kVst3Once = "vst3:534F4E4F524156535433544553540008";
const char* const kKeyPlugin = "urn:sonora:test#key";
const char* const kKeyLiar = "urn:sonora:test#keyliar";

struct Args {
    std::string host, scenario, format = "lv2", wake;
    std::vector<std::string> bundles, chain;
    uint32_t channels = 1;
    uint64_t blocks = 0;
    // gain and bypass: the parameter to move, by key (LV2 symbol, VST3
    // ParamID in decimal), and whether it takes a linear factor (VST3 AGain,
    // normalised 0..1) rather than dB (LV2 eg-amp).
    std::string gain_param;
    bool gain_linear = false;
    uint32_t expect_replicas = 0;
    // latency_change: the parameter of chain instance 0 to move, and to what.
    std::string change_param;
    float change_value = 0.f;
    // bypass: "plugin" or "host", what the probe must say it uses.
    std::string expect_bypass;
    // render: the session rate, the files, and what the host is told.
    uint32_t rate = kRate;
    std::string input, expected;
    double tolerance = 1e-6;
    int64_t expect_latency = -1;
    std::vector<std::string> sets;  // host --set INDEX:SYMBOL=VALUE
    // --param INDEX:KEY=VALUE: a value the "model" holds, seeded into the
    // store and the table before the host starts, as the engine does.
    struct Seed {
        uint32_t instance;
        std::string key;
        float value;
    };
    std::vector<Seed> seeds;
    // render: a key for the plugin's side-chain, planar, key_channels wide.
    std::string key;
    uint32_t key_channels = 0;
    std::vector<std::string> changes;  // BLOCK:SYMBOL=VALUE, chain instance 0
    bool reactivate = false;           // host --reactivate
    // --control 1: the engine's way (contracts/plugin host_control.h). The
    // chain, key channels, re-activation and states go in Load on the
    // host's socket; --set becomes a seeded table value, as the engine has
    // no --set. Every scenario that starts hosts through start_ready runs
    // either way.
    bool control = false;
    // state: "counter" (the test plugins: a restored instance counts on
    // from the saved one) or "same" (the state survives a host replacement
    // byte for byte). --state-dir: where "Plugin State/" is written.
    std::string expect_state = "counter", state_dir;
};

namespace pc = sonora::plugin::control;

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

std::string join(const std::vector<std::string>& v, char sep) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : std::string(1, sep)) + x;
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = s.find(sep, start);
        out.push_back(s.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) return out;
        start = end + 1;
    }
}

const char* state_name(uint32_t s) {
    switch (static_cast<plugin::HostState>(s)) {
    case plugin::HostState::Starting: return "Starting";
    case plugin::HostState::Ready: return "Ready";
    case plugin::HostState::Refused: return "Refused";
    case plugin::HostState::Faulted: return "Faulted";
    case plugin::HostState::Stopped: return "Stopped";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// What the engine's scan knows of a plugin: its dense parameter list.

struct PluginParams {
    bool described = false;
    std::vector<std::string> keys;
    std::vector<std::string> symbols;  // what host --set takes: LV2 symbol, VST3 title
    std::vector<float> defaults;
    std::string bypass;  // "plugin" or "host"

    int index_of(const std::string& key) const {
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) return int(i);
        return -1;
    }
    int index_of_symbol(const std::string& symbol) const {
        for (std::size_t i = 0; i < symbols.size(); ++i)
            if (symbols[i] == symbol) return int(i);
        return -1;
    }
};

std::string shell_quote(const std::string& s) {
    std::string q = "'";
    for (char c : s) q += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return q + "'";
}

// `--probe ID` prints ..."bypass":"host","params":[{"key":"gain","symbol":"gain","default":0},...]
PluginParams probe_params(const Args& a, const std::string& id) {
    static std::map<std::string, PluginParams> cache;
    if (auto it = cache.find(id); it != cache.end()) return it->second;
    std::string cmd = shell_quote(a.host) + " --probe " + shell_quote(id);
    for (const std::string& b : a.bundles) cmd += " --bundle " + shell_quote(b);
    // stderr is left to the test's output: the probe validates the chain,
    // running every plugin, and a sanitizer's report there must reach the
    // log (and tools/build-rtsan.sh's scan of it).
    PluginParams out;
    if (std::FILE* p = popen(cmd.c_str(), "r")) {
        std::string all;
        char buf[4096];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, p)) > 0;) all.append(buf, n);
        pclose(p);
        if (const auto b = all.find("\"bypass\":\""); b != std::string::npos)
            out.bypass = all.substr(b + 10, all.find('"', b + 10) - (b + 10));
        const auto at = all.find("\"params\":[");
        if (at != std::string::npos) {
            out.described = true;
            std::size_t i = at;
            for (;;) {
                const auto k = all.find("{\"key\":\"", i);
                if (k == std::string::npos) break;
                std::size_t e = k + 8;
                std::string key;
                for (; e < all.size() && all[e] != '"'; ++e) {
                    if (all[e] == '\\' && e + 1 < all.size()) ++e;
                    key += all[e];
                }
                const auto d = all.find("\"default\":", e);
                if (d == std::string::npos) break;
                std::string symbol;
                if (const auto y = all.find("\"symbol\":\"", e); y != std::string::npos && y < d)
                    for (std::size_t q = y + 10; q < all.size() && all[q] != '"'; ++q) {
                        if (all[q] == '\\' && q + 1 < all.size()) ++q;
                        symbol += all[q];
                    }
                out.symbols.push_back(symbol);
                out.keys.push_back(key);
                out.defaults.push_back(std::strtof(all.c_str() + d + 10, nullptr));
                i = d;
            }
        }
    }
    cache[id] = out;
    return out;
}

// ---------------------------------------------------------------------------
// The session clock (contracts/plugin ClockHeader): one per "session", read
// by every host through a sealed, read-only descriptor (auditor M1).

class Clock {
public:
    plugin::ClockHeader* c = nullptr;
    int ro_handle = -1;
    plugin::WakeMode mode = plugin::WakeMode::Timer;

    bool create(plugin::WakeMode m, const std::function<void(plugin::ClockHeader&)>& mutate = {},
                uint32_t rate = kRate) {
        mode = m;
        std::string err;
        if (!create_region(4096, region_, err)) {
            std::printf("  cannot create the clock: %s\n", err.c_str());
            return false;
        }
        c = plugin::init_clock(region_.base, rate, kBlock, m);
        if (mutate) mutate(*c);
        if (!share_read_only(region_, ro_handle, err)) {
            std::printf("  cannot share the clock read-only: %s\n", err.c_str());
            return false;
        }
        return true;
    }
    ~Clock() {
        close_handle(ro_handle);
        release_region(region_);
    }

    // The audio thread's half (ADR-0012 §7.2): the callback's start before
    // any submit, and after the last submit one tick, plus the wake only in
    // futex mode (the gate, kHostWakeEnabled, is off in the engine).
    void begin(uint64_t now_ns) { plugin::publish_callback_start(*c, now_ns); }
    void submitted() { pending_ = true; }
    void end() {
        if (!pending_) return;
        pending_ = false;
        plugin::publish_tick(*c);
        if (mode == plugin::WakeMode::Futex) platform::wake_all(c->tick);
    }
    // Shutdown: wake futex sleepers without a tick, so they look at once.
    void wake() { platform::wake_all(c->tick); }

private:
    SharedRegion region_;
    bool pending_ = false;
};

// Channel regions the OS would not lock (lock_region); printed, never silent.
uint64_t region_lock_failures = 0;

// The engine's private, authoritative values of one chain (ADR-0012 §5.1):
// keyed by the chain, not by a host, so it outlives a host replacement.
struct Store {
    plugin::ParamMap map;
    std::vector<float> values;  // map.param_slots
    float bypass[plugin::kMaxInstances] = {};
};

// ---------------------------------------------------------------------------
// The engine's half of one host process.

class Host {
public:
    plugin::ChannelHeader* h = nullptr;
    std::vector<std::string> chain;

    struct StartOptions {
        uint32_t channels = 1;
        // Override what the scan says, for the refusal of a mismatch.
        int param_delta = 0;
        // Applied to the header before the host starts (incompatible).
        std::function<void(plugin::ChannelHeader&)> mutate;
        Store* store = nullptr;  // null: a store of this host's own
        // --control: per instance, the StateBlob Load carries ("" for none),
        // and what the incompatible scenario forges.
        std::vector<std::string> states;
        std::function<void(pc::EngineHello&)> hello;
        std::function<void(pc::Load&)> load;
        std::string first_message;  // sent instead of EngineHello and Load
    };

    bool start(const Args& a, Clock& clock, const std::vector<std::string>& uris, const StartOptions& o) {
        chain = uris;
        clock_ = &clock;
        std::vector<uint32_t> counts;
        std::vector<PluginParams> described;
        for (const std::string& id : uris) {
            described.push_back(probe_params(a, id));
            counts.push_back(uint32_t(described.back().keys.size()));
        }
        if (!counts.empty()) counts[0] = uint32_t(int(counts[0]) + o.param_delta);
        map_ = plugin::make_param_map(counts.data(), uint32_t(counts.size()));
        if (o.store) {
            store_ = o.store;
        } else {
            own_store_ = std::make_unique<Store>();
            store_ = own_store_.get();
            store_->map = map_;
            store_->values.assign(map_.param_slots, 0.f);
            for (uint32_t i = 0; i < map_.instance_count; ++i)
                for (uint32_t k = 0; k < map_.count[i] && k < described[i].defaults.size(); ++k)
                    if (const int64_t s = plugin::param_slot(map_, i, k); s >= 0)
                        store_->values[std::size_t(s)] = described[i].defaults[k];
            for (const Args::Seed& seed : a.seeds) {
                const int k = seed.instance < described.size() ? described[seed.instance].index_of(seed.key) : -1;
                const int64_t s = k < 0 ? -1 : plugin::param_slot(map_, seed.instance, uint32_t(k));
                if (s >= 0) store_->values[std::size_t(s)] = seed.value;
                else std::printf("  --param %u:%s: no such parameter\n", seed.instance, seed.key.c_str());
            }
            // The engine has no --set: a value the model holds is seeded.
            if (a.control)
                for (const std::string& set : a.sets) {
                    const auto colon = set.find(':'), eq = set.rfind('=');
                    const uint32_t i = uint32_t(std::atoi(set.substr(0, colon).c_str()));
                    const std::string symbol = set.substr(colon + 1, eq - colon - 1);
                    const int k = i < described.size() ? described[i].index_of_symbol(symbol) : -1;
                    const int64_t s = k < 0 ? -1 : plugin::param_slot(map_, i, uint32_t(k));
                    if (s >= 0) store_->values[std::size_t(s)] = std::strtof(set.c_str() + eq + 1, nullptr);
                    else std::printf("  --set %s: no such parameter\n", set.c_str());
                }
        }
        // The engine's own copy: plane and table addresses never come from
        // the region.
        layout_ = plugin::make_layout(o.channels, kMaxFrames, map_.param_slots);
        std::string err;
        if (!create_region(layout_.region_size, region_, err)) {
            std::printf("  cannot create the channel: %s\n", err.c_str());
            return false;
        }
        plugin::prefault(region_.base, region_.size);
        // Faulted in is not kept in (auditor L3): lock it too, and say so
        // when the OS refuses, as the engine reports it in its health flags.
        if (!lock_region(region_, err)) {
            ++region_lock_failures;
            std::printf("  the channel region is pre-touched but NOT locked (%s): it can be reclaimed\n",
                        err.c_str());
        }
        h = plugin::init_header(region_.base, layout_, a.rate, kBlock, uint32_t(uris.size()), counts.data(), 70);
        table_ = plugin::value_table(region_.base, layout_);
        // Seeded from the store before the host starts, so it instantiates
        // with these values and reports the latency they give.
        for (uint32_t s = 0; s < map_.param_slots && s < store_->values.size(); ++s)
            plugin::seed_value(table_, s, store_->values[s]);
        for (uint32_t i = 0; i < plugin::kMaxInstances; ++i) h->bypass[i].store(store_->bypass[i]);
        if (o.mutate) o.mutate(*h);

        // Built before fork: the child only rearranges descriptors and execs.
        const std::string engine_pid = std::to_string(getpid());
        const std::string chain_arg = join(uris, ',');
        std::vector<const char*> argv = {a.host.c_str()};
        if (a.control) {
            // The engine's way: the socket at 3, the regions after it, and
            // the chain in Load (contracts/plugin host_control.h).
            for (const char* x : {"--control-fd", "3", "--shm-fd", "4", "--clock-fd", "5"}) argv.push_back(x);
        } else {
            for (const char* x : {"--shm-fd", "3", "--clock-fd", "4", "--chain"}) argv.push_back(x);
            argv.push_back(chain_arg.c_str());
        }
        argv.push_back("--engine-pid");
        argv.push_back(engine_pid.c_str());
        for (const std::string& b : a.bundles) {
            argv.push_back("--bundle");
            argv.push_back(b.c_str());
        }
        const std::string sidechain = std::to_string(a.key_channels);
        if (!a.control) {
            for (const std::string& s : a.sets) {
                argv.push_back("--set");
                argv.push_back(s.c_str());
            }
            // The last key_channels of the region are the key.
            if (a.key_channels > 0) {
                argv.push_back("--sidechain");
                argv.push_back(sidechain.c_str());
            }
            if (a.reactivate) argv.push_back("--reactivate");
        }
        argv.push_back(nullptr);

        int child_end = -1;
        if (a.control) {
            if (!platform::ControlChannel::pair(ctl_, child_end, err)) {
                std::printf("  cannot create the control socket: %s\n", err.c_str());
                return false;
            }
            // What the engine writes before the host has said anything:
            // EngineHello first, Load right behind it (Handshake 1).
            if (!o.first_message.empty()) {
                queue_raw(o.first_message);
            } else {
                pc::EngineHello hello;
                if (o.hello) o.hello(hello);
                queue(hello);
                pc::Load load;
                load.key_channels = a.key_channels;
                load.flags = a.reactivate ? pc::kLoadReactivate : 0;
                for (std::size_t i = 0; i < uris.size(); ++i) {
                    pc::LoadInstance li;
                    const bool vst3 = uris[i].rfind("vst3:", 0) == 0;
                    li.format = vst3 ? pc::Format::Vst3 : pc::Format::Lv2;
                    li.id = vst3 ? uris[i].substr(5) : uris[i];
                    li.params = counts[i];
                    if (i < o.states.size()) li.state = o.states[i];
                    load.instances.push_back(std::move(li));
                }
                if (o.load) o.load(load);
                queue(load);
            }
        }
        const int shm = region_.handle, clk = clock.ro_handle;
        pid_ = fork();
        if (pid_ < 0) return false;
        if (pid_ == 0) {
            // Out of the way of 3 to 5 first, then into place, without CLOEXEC.
            const int s = fcntl(shm, F_DUPFD, 10), k = fcntl(clk, F_DUPFD, 10);
            if (s < 0 || k < 0) _exit(126);
            if (child_end >= 0) {
                const int c = fcntl(child_end, F_DUPFD, 10);
                if (c < 0 || dup2(c, 3) < 0 || dup2(s, 4) < 0 || dup2(k, 5) < 0) _exit(126);
            } else if (dup2(s, 3) < 0 || dup2(k, 4) < 0) {
                _exit(126);
            }
            execv(a.host.c_str(), const_cast<char* const*>(argv.data()));
            _exit(127);
        }
        platform::ControlChannel::close_handle(child_end);
        if (ctl_) pump(0);
        return true;
    }

    // ---- HostControl, the engine's control thread's half. Nothing here
    // waits except where a scenario asks for a reply: pump() polls with the
    // timeout given, sends what the Link holds and drains what came, the
    // way the engine's poll loop does beside the pidfd.

    bool controlled() const { return ctl_ != nullptr; }

    template <class M>
    void queue(const M& m) {
        std::string bytes;
        pc::encode(m, bytes);
        queue_raw(bytes);
    }
    void queue_raw(const std::string& bytes) {
        std::string e;
        if (!link_.queue(bytes, e)) std::printf("  cannot queue a message: %s\n", e.c_str());
    }

    void pump(int timeout_ms) {
        if (!ctl_ || ctl_closed_) return;
        pollfd p{ctl_->native_handle(), short(POLLIN | (link_.wants_write() ? POLLOUT : 0)), 0};
        if (::poll(&p, 1, timeout_ms) < 0) return;
        std::string e;
        const pc::Io f = link_.flush([&](std::string_view d) { return io(ctl_->send(d, 0, e)); });
        if (f == pc::Io::Closed || f == pc::Io::Error) ctl_closed_ = true;
        const pc::Io r = link_.drain(
            [&](std::string& d) { return io(ctl_->receive(d, pc::kMaxFrameBytes, nullptr, 0, e)); },
            [&](std::string_view m) {
                pc::Type t;
                // Untrusted (host_control.h): only what a host may send.
                if (!pc::peek_type(m, t) || !pc::host_to_engine(t)) {
                    e = "a message type no host sends";
                    return false;
                }
                inbox_.emplace_back(m);
                return true;
            },
            e, monotonic_ns());
        if (r == pc::Io::Closed) ctl_closed_ = true;
        // The engine's checks of M1 (host_control.h, timeouts): a host with
        // a message half received that stopped growing is pinning memory.
        if (link_.stalled(monotonic_ns(), uint64_t(pc::kStalledMessageMs) * 1000000ull)) {
            ctl_closed_ = true;
            ctl_error_ = "a message half received for " + std::to_string(pc::kStalledMessageMs) + " ms";
        }
        if (r == pc::Io::Error) {
            ctl_closed_ = true;
            ctl_error_ = e;
        }
    }

    // The first message of type `t` within `timeout_ms`, pumping; the
    // others stay in the inbox in order.
    bool expect(pc::Type t, std::string& out, int timeout_ms) {
        const uint64_t end = monotonic_ns() + uint64_t(timeout_ms) * 1000000ull;
        for (bool last = false;; ) {
            for (auto it = inbox_.begin(); it != inbox_.end(); ++it) {
                pc::Type got;
                if (pc::peek_type(*it, got) && got == t) {
                    out = std::move(*it);
                    inbox_.erase(it);
                    return true;
                }
            }
            if (last) return false;
            const uint64_t now = monotonic_ns();
            if (now >= end || ctl_closed_) {
                pump(0);  // whatever is already there
                last = true;
                continue;
            }
            pump(int(std::min<uint64_t>((end - now) / 1000000ull + 1, 50)));
        }
    }
    template <class M>
    bool expect_message(pc::Type t, M& m, int timeout_ms) {
        std::string bytes, e;
        if (!expect(t, bytes, timeout_ms)) return false;
        if (!pc::decode(bytes, m, e)) {
            std::printf("  a host message does not decode: %s\n", e.c_str());
            return false;
        }
        return true;
    }
    // Everything received so far, in order, taken.
    std::vector<std::string> take_inbox() {
        pump(0);
        std::vector<std::string> out(inbox_.begin(), inbox_.end());
        inbox_.clear();
        return out;
    }
    // A datagram past the Link, for the framing test only.
    void send_raw_frame(const std::string& frame) {
        std::string e;
        if (ctl_) ctl_->send(frame, 1000, e);
    }
    bool control_closed() const { return ctl_closed_; }
    const std::string& control_error() const { return ctl_error_; }
    const pc::Loaded& loaded() const { return loaded_; }
    const pc::Hello& hello() const { return hello_; }
    pid_t pid() const { return pid_; }

    // Wait for the host's exit, pumping its socket; the messages it sent on
    // the way out stay in the inbox.
    bool wait_exit_pumping(uint64_t timeout_ns) {
        const uint64_t end = monotonic_ns() + timeout_ns;
        while (alive() && monotonic_ns() < end) pump(5);
        pump(0);
        return !alive();
    }

    // Not RT: the engine's control thread waits for the host to come up.
    plugin::HostState wait_ready(uint64_t timeout_ns) {
        const uint64_t end = monotonic_ns() + timeout_ns;
        for (;;) {
            const uint32_t seen = h->done_word.load(std::memory_order_acquire);
            const auto s = plugin::checked_state(h->state.load(std::memory_order_acquire));
            if (s != plugin::HostState::Starting) return s;
            if (!alive() || monotonic_ns() > end) return plugin::HostState::Starting;
            if (ctl_) pump(20);  // the host's Hello is read meanwhile, as the engine's poll loop would
            else wait_for_change(h->done_word, seen, 20'000'000);
        }
    }

    // --control: what the engine checks once the region has left Starting
    // (host_control.h, Handshake 3 and ordering): a Hello naming this
    // host's pid and the engine's versions, then Loaded or Refused agreeing
    // with the region. False, said why, on any disagreement.
    bool check_handshake(plugin::HostState s) {
        if (!expect_message(pc::Type::Hello, hello_, 10000)) {
            std::printf("  no Hello from the host%s%s\n", ctl_error_.empty() ? "" : ": ", ctl_error_.c_str());
            return false;
        }
        if (hello_.wire_version != pc::kHostWireVersion || hello_.layout_version != plugin::kLayoutVersion ||
            hello_.clock_version != plugin::kClockLayoutVersion || hello_.pid != uint64_t(pid_)) {
            std::printf("  Hello: wire %u layout %u clock %u pid %llu; expected this engine's and pid %d\n",
                        hello_.wire_version, hello_.layout_version, hello_.clock_version,
                        (unsigned long long)hello_.pid, int(pid_));
            return false;
        }
        if (s == plugin::HostState::Ready) {
            if (!expect_message(pc::Type::Loaded, loaded_, pc::kLoadTimeoutMs)) {
                std::printf("  the region says Ready and no Loaded came\n");
                return false;
            }
            // The region is the truth; the message agrees with it or is wrong.
            const uint32_t gen = h->latency_generation.load(std::memory_order_acquire);
            const uint32_t frames = plugin::checked_latency(h->latency_frames.load(std::memory_order_relaxed));
            if (loaded_.instances.size() != chain.size() || loaded_.latency_frames != frames ||
                loaded_.latency_generation > gen) {
                std::printf("  Loaded: %zu instance(s), latency %u gen %u; the region: %zu, %u gen %u\n",
                            loaded_.instances.size(), loaded_.latency_frames, loaded_.latency_generation,
                            chain.size(), frames, gen);
                return false;
            }
            for (std::size_t i = 0; i < chain.size(); ++i)
                if (loaded_.instances[i].params.size() != h->instance_params[i]) {
                    std::printf("  Loaded lists %zu parameter(s) for instance %zu; the scan %u\n",
                                loaded_.instances[i].params.size(), i, h->instance_params[i]);
                    return false;
                }
            return true;
        }
        pc::Refused r;
        if (!expect_message(pc::Type::Refused, r, 10000)) {
            std::printf("  the region says %s and no Refused came\n", state_name(uint32_t(s)));
            return false;
        }
        if (uint32_t(r.reason) != h->refusal.load() || r.instance != h->refusal_instance.load()) {
            std::printf("  Refused (%u, %d) disagrees with the region (%u, %d)\n", unsigned(r.reason), r.instance,
                        h->refusal.load(), h->refusal_instance.load());
            return false;
        }
        std::printf("  Refused: %s\n", r.text.c_str());
        return true;
    }

    bool alive() {
        if (reaped_) return false;
        if (waitpid(pid_, &status_, WNOHANG) == pid_) reaped_ = true;
        return !reaped_;
    }
    bool reaped() const { return reaped_; }
    int status() const { return status_; }

    std::string death() const {
        if (!reaped_) return "still running";
        char buf[96];
        if (WIFSIGNALED(status_))
            std::snprintf(buf, sizeof buf, "killed by signal %d (%s)", WTERMSIG(status_),
                          strsignal(WTERMSIG(status_)));
        else
            std::snprintf(buf, sizeof buf, "exited with code %d", WEXITSTATUS(status_));
        return buf;
    }

    void kill_now() {
        if (!reaped_ && pid_ > 0) {
            kill(pid_, SIGKILL);
            waitpid(pid_, &status_, 0);
            reaped_ = true;
        }
    }

    // Shutdown, then the region is released: never handed to another host
    // (ADR-0012 §5.3). A make-before-break caller does this only after the
    // plan that used this host is retired.
    void stop() {
        if (h && !reaped_) {
            if (ctl_ && !ctl_closed_) {
                // The control thread's way (host_control.h, Shutdown).
                queue(pc::Shutdown{});
                wait_exit_pumping(2'000'000'000ull);
            } else {
                h->shutdown.store(1, std::memory_order_release);
                if (clock_) clock_->wake();
                const uint64_t end = monotonic_ns() + 2'000'000'000ull;
                while (alive() && monotonic_ns() < end) usleep(5000);
            }
            kill_now();
        }
        ctl_.reset();
        release_region(region_);
        h = nullptr;
    }

    ~Host() { stop(); }

    const void* region_base() const { return region_.base; }
    std::size_t region_size() const { return region_.size; }

    // ---- the RT half: what engine/plugin_client's submit/collect would do.
    // Nothing here waits on the host (contracts/plugin, block protocol).

    bool idle() const { return plugin::channel_idle(*h, eng_); }

    // A parameter change, the audio thread's way: into the private store at
    // once, and queued for this callback. If the callback submits, the
    // queue goes out as table entry then ring entry, tagged with the block
    // being submitted; if it does not, the queue is held (hold_param) and
    // the next submit copies the store in. Nothing reaches the region in a
    // callback that does not submit, so nothing can apply a block early
    // (contracts/plugin, block protocol step 2; rt-engineer M2).
    void send(uint32_t instance, uint32_t param, uint32_t offset, float value) {
        keep(instance, param, value);
        pending_.push_back({Pending::Ring, {0, instance, param, offset, value}});
    }
    // What a refused push leaves: the table and values_seq only.
    void send_table_only(uint32_t instance, uint32_t param, float value) {
        keep(instance, param, value);
        pending_.push_back({Pending::TableOnly, {0, instance, param, 0, value}});
    }
    // A raw ring entry for an explicit block (the late-entry test), pushed
    // only in a callback that submits, like everything else.
    void push_raw(const plugin::ParamChange& p) { pending_.push_back({Pending::Raw, p}); }
    uint64_t next_block() const { return plugin::next_block(eng_); }

    // A new channel's first submit copies the store in (A2). EngineChannel
    // starts that way; this says so where the swap happens.
    void mark_new_channel() { eng_.resync = true; }

    // Step 2, in the contract's order. False, with nothing written, for a
    // frame count other than block_frames (M3).
    bool submit(const float* const* in, uint32_t frames, uint64_t callback) {
        if (frames != eng_.block_frames) return false;
        if (eng_.resync) {
            ++resyncs_;
            plugin::store_values(*h, eng_, table_, layout_, store_->values.data(), store_->bypass,
                                 map_.instance_count);
        }
        for (uint32_t c = 0; c < layout_.channels; ++c)
            std::memcpy(plugin::input_plane(region_.base, layout_, c), in[c], frames * sizeof(float));
        const uint64_t tag = plugin::next_block(eng_);
        for (Pending& q : pending_) {
            if (q.kind == Pending::Raw) {
                if (!plugin::push_param(*h, eng_, q.change)) ++refused_;
            } else if (q.kind == Pending::TableOnly) {
                if (q.change.param == plugin::kBypassParam)
                    h->bypass[q.change.instance].store(q.change.value, std::memory_order_relaxed);
                else if (const int64_t s = plugin::param_slot(map_, q.change.instance, q.change.param); s >= 0)
                    table_[s].store(q.change.value, std::memory_order_relaxed);
                plugin::bump_values_seq(*h, eng_);
            } else {
                q.change.block = tag;
                if (!plugin::send_param(*h, eng_, table_, map_, q.change)) ++refused_;
            }
        }
        pending_.clear();
        submitted_at_ = callback;
        plugin::submit_block(*h, eng_, frames);
        clock_->submitted();
        return true;
    }

    // Step 1: each plane copied out, and only the copy checked (M4).
    // Returns the NaN/Inf count, which the caller treats as a miss.
    uint32_t read_output(float* const* out, uint32_t frames) {
        uint32_t bad = 0;
        for (uint32_t c = 0; c < layout_.channels; ++c)
            bad += plugin::collect_output(out[c], plugin::output_plane(region_.base, layout_, c), frames);
        nonfinite_collected_ += bad;
        return bad;
    }

    // One callback, depth 1: collect the block submitted in the previous
    // callback if it is already complete, then submit this one. Returns the
    // callback whose input produced `out`, or -1 when nothing was collected
    // (a miss). A block that completed later than that is discarded rather
    // than played a block late, which would break the declared latency.
    int64_t run(const float* const* in, float* const* out, uint32_t frames, uint64_t callback) {
        // Still on an older block (never touch its slots), or a callback of
        // another size (M3): a miss, and this callback's changes are held.
        if (frames != eng_.block_frames || !idle()) {
            hold();
            return -1;
        }
        int64_t got = -1;
        if (eng_.block > 0 && submitted_at_ + 1 == callback && read_output(out, frames) == 0)
            got = int64_t(submitted_at_);
        submit(in, frames, callback);
        return got;
    }

    // The callback of the most recent submit.
    uint64_t submitted_at() const { return submitted_at_; }
    uint64_t refused() const { return refused_; }

    // MEASUREMENT HARNESS ONLY (rtt, render): submit, tick, then sleep on
    // done_word until the block is back or `deadline` passes. The engine's
    // audio thread never does this; a timed wait there is exposed to the
    // scheduler tail (5.9 ms late seen, container).
    bool round_trip(const float* const* in, float* const* out, uint32_t frames, uint64_t callback,
                    uint64_t deadline, uint64_t& rtt_ns) {
        if (!idle()) {
            hold();
            return false;
        }
        const uint64_t t0 = monotonic_ns();
        clock_->begin(t0);
        if (!submit(in, frames, callback)) {
            hold();
            return false;
        }
        clock_->end();
        for (;;) {
            const uint32_t seen = h->done_word.load(std::memory_order_acquire);
            const bool done = idle();
            const uint64_t now = monotonic_ns();
            if (done) rtt_ns = now - t0;
            // A block noticed after the deadline is a miss, even if the host
            // was on time: the callback is what was late.
            if (now > deadline) {
                ++late_;
                return false;
            }
            if (done) {
                read_output(out, frames);
                return true;
            }
            if (!alive()) return false;
            wait_for_change(h->done_word, seen, std::min<uint64_t>(deadline - now, 100'000'000));
        }
    }

    uint64_t late() const { return late_; }
    // Callbacks whose changes were held, then copied in by a later submit.
    uint64_t resyncs() const { return resyncs_; }
    uint64_t nonfinite_collected() const { return nonfinite_collected_; }

private:
    struct Pending {
        enum Kind { Ring, TableOnly, Raw } kind;
        plugin::ParamChange change;
    };

    // The engine's private store is authoritative and is written at once.
    void keep(uint32_t instance, uint32_t param, float value) {
        if (param == plugin::kBypassParam) store_->bypass[instance] = value;
        else if (const int64_t s = plugin::param_slot(map_, instance, param); s >= 0)
            store_->values[std::size_t(s)] = value;
    }
    void hold() {
        if (pending_.empty()) return;
        plugin::hold_param(eng_);
        pending_.clear();
    }

    uint64_t late_ = 0, refused_ = 0, resyncs_ = 0, nonfinite_collected_ = 0;
    plugin::EngineChannel eng_ = plugin::make_engine_channel(kBlock);
    std::vector<Pending> pending_;
    SharedRegion region_;
    plugin::Layout layout_;
    plugin::ParamMap map_;
    std::atomic<float>* table_ = nullptr;
    Store* store_ = nullptr;
    std::unique_ptr<Store> own_store_;
    Clock* clock_ = nullptr;
    pid_t pid_ = -1;
    int status_ = 0;
    bool reaped_ = false;
    uint64_t submitted_at_ = 0;

    static pc::Io io(platform::IoStatus s) {
        switch (s) {
        case platform::IoStatus::Ok: return pc::Io::Ok;
        case platform::IoStatus::WouldBlock: return pc::Io::WouldBlock;
        case platform::IoStatus::Closed: return pc::Io::Closed;
        default: return pc::Io::Error;
        }
    }
    std::unique_ptr<platform::ControlChannel> ctl_;
    pc::Link link_;
    std::deque<std::string> inbox_;
    bool ctl_closed_ = false;
    std::string ctl_error_;
    pc::Hello hello_;
    pc::Loaded loaded_;
};

// The periodic "audio callback". Measures how long each callback ran from
// its wake-up, which is the number that must stay under the period whatever
// the host does. The callback's start goes into the clock first, the tick
// after the callback's submits.
struct LoopStats {
    uint64_t callbacks = 0, worst_callback_ns = 0, over_period = 0;
};

template <class F>
LoopStats run_periodic(Clock& clock, uint64_t blocks, F&& callback) {
    LoopStats st;
    timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (uint64_t i = 0; i < blocks; ++i) {
        next.tv_nsec += static_cast<long>(kPeriodNs);
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            ++next.tv_sec;
        }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr) == EINTR) {
        }
        const uint64_t woke = monotonic_ns();
        clock.begin(woke);
        const bool keep_going = callback(i);
        clock.end();
        const uint64_t took = monotonic_ns() - woke;
        ++st.callbacks;
        st.worst_callback_ns = std::max(st.worst_callback_ns, took);
        if (took > kPeriodNs) ++st.over_period;
        if (!keep_going) break;
    }
    return st;
}

struct Signal {
    std::vector<std::vector<float>> buf;
    std::vector<const float*> in;
    std::vector<std::vector<float>> obuf;
    std::vector<float*> out;
    explicit Signal(uint32_t channels)
        : buf(channels, std::vector<float>(kMaxFrames)), obuf(channels, std::vector<float>(kMaxFrames)) {
        for (auto& b : buf) in.push_back(b.data());
        for (auto& b : obuf) out.push_back(b.data());
    }
    void sine(uint64_t block, double hz, float amp) {
        for (std::size_t c = 0; c < buf.size(); ++c)
            for (uint32_t i = 0; i < kBlock; ++i) {
                const double t = double(block * kBlock + i) / kRate;
                buf[c][i] = amp * float(std::sin(2.0 * M_PI * hz * t + 0.7 * double(c)));
            }
    }
    void silence_out() {
        for (auto& b : obuf) std::fill(b.begin(), b.end(), 0.f);
    }
};

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const std::size_t i = std::min(v.size() - 1, std::size_t(p / 100.0 * double(v.size())));
    return v[i];
}

plugin::WakeMode wake_of(const Args& a, plugin::WakeMode fallback = plugin::default_wake_mode()) {
    if (a.wake == "futex") return plugin::WakeMode::Futex;
    if (a.wake == "timer") return plugin::WakeMode::Timer;
    return fallback;
}

// Start a host and wait for it. Returns kSkip when a plugin is not
// installed; ctest treats that as a failure unless skips were allowed
// (SONORA_PLUGIN_TESTS_ALLOW_SKIP), because CI must not go green without
// having run the test.
int start_ready(Host& host, const Args& a, Clock& clock, const std::vector<std::string>& chain,
                const Host::StartOptions& o) {
    if (!host.start(a, clock, chain, o)) return 1;
    const plugin::HostState s = host.wait_ready(30'000'000'000ull);
    if (host.controlled() && s != plugin::HostState::Starting) {
        const bool agreed = host.check_handshake(s);
        check(agreed, "HostControl: Hello, then Loaded or Refused, agreeing with the region");
        if (!agreed) return 1;
    }
    if (s == plugin::HostState::Ready) {
        const uint32_t flags = host.h->host_flags.load();
        std::printf("  host ready%s: %s wake, chain latency %u frames, host %s, memory %s, %s\n",
                    host.controlled() ? " (HostControl: Hello, Load, Loaded)" : "",
                    clock.mode == plugin::WakeMode::Futex ? "futex" : "timer",
                    plugin::checked_latency(host.h->latency_frames.load()),
                    (flags & plugin::kHostRealtime) ? "SCHED_FIFO 70" : "SCHED_OTHER (no RT allowed)",
                    (flags & plugin::kHostMemoryLocked) ? "locked" : "not locked",
                    (flags & plugin::kHostFlushDenormals) ? "FTZ/DAZ" : "denormals not flushed");
        for (std::size_t i = 0; i < chain.size(); ++i)
            std::printf("    [%zu] %s: latency %u, replicas %u, %u parameter(s)\n", i, chain[i].c_str(),
                        plugin::checked_latency(host.h->instances[i].latency_frames.load()),
                        host.h->instances[i].replicas.load(), host.h->instance_params[i]);
        return 0;
    }
    const auto reason = static_cast<plugin::Refusal>(host.h->refusal.load());
    std::printf("  host not ready: state %s, refusal %u, instance %d, %s\n",
                state_name(host.h->state.load()), unsigned(reason),
                plugin::checked_instance(host.h->refusal_instance.load(), uint32_t(chain.size())),
                host.death().c_str());
    if (reason == plugin::Refusal::NotFound) {
        std::printf("  SKIP: a plugin this test needs is not installed (tools/Dockerfile.plugins)\n");
        return kSkip;
    }
    return 1;
}

int start_ready(Host& host, const Args& a, Clock& clock, const std::vector<std::string>& chain,
                uint32_t channels) {
    Host::StartOptions o;
    o.channels = channels;
    return start_ready(host, a, clock, chain, o);
}

void report_loop(const LoopStats& st) {
    std::printf("  driver: %llu callbacks, worst callback %.1f us, %llu over the %.0f us period\n",
                (unsigned long long)st.callbacks, st.worst_callback_ns / 1e3,
                (unsigned long long)st.over_period, kPeriodNs / 1e3);
}

const char* chain_name(const std::vector<std::string>& chain, int32_t i) {
    return i >= 0 && i < int32_t(chain.size()) ? chain[size_t(i)].c_str() : "?";
}

// A parameter of chain instance `instance` by key, as the probe listed it.
int dense_param(const Args& a, const std::string& id, const std::string& key) {
    const int d = probe_params(a, id).index_of(key);
    if (d < 0) std::printf("  %s has no parameter with key '%s'\n", id.c_str(), key.c_str());
    return d;
}

// ---------------------------------------------------------------------------
// Scenarios

// A gain plugin changes the level by exactly what was asked, on every
// channel, and a change lands in the first block submitted at or after the
// callback that made it, not before and not later: through the ring (the
// -6 dB step) and through the last-value table alone, as when the ring was
// full (the second step: values_seq, auditor M3).
int scenario_gain(const Args& a) {
    const std::vector<std::string> chain = a.chain.empty() ? std::vector<std::string>{kEgAmp} : a.chain;
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    std::printf("gain: %s, %u channel(s), %s wake\n", join(chain, ',').c_str(), a.channels,
                clock.mode == plugin::WakeMode::Futex ? "futex" : "timer");
    Host host;
    if (int r = start_ready(host, a, clock, chain, a.channels)) return r;
    if (a.channels > 1) {
        const uint32_t want = a.expect_replicas ? a.expect_replicas : a.channels;
        check(host.h->instances[0].replicas.load() == want,
              want == 1 ? "the plugin took the chain's arrangement: one instance"
                        : "a mono plugin is replicated per channel");
    }
    // eg-amp: "gain" in dB (lv2info http://lv2plug.in/plugins/eg-amp).
    // AGain (VST3): ParamID 0 is a linear factor, normalised 0..1, so it can
    // only cut. Neither smooths, so every block shows its value.
    const int gp = dense_param(a, chain[0], a.gain_param.empty() ? "gain" : a.gain_param);
    if (gp < 0) return 1;

    struct Phase { uint64_t from; float db; };
    const Phase phases[] = {{0, 0.f}, {200, -6.f}, {400, a.gain_linear ? -12.f : 12.f}};
    auto value_of = [&](float db) {
        return a.gain_linear ? static_cast<float>(std::pow(10.0, db / 20.0)) : db;
    };
    const uint64_t total = 600;
    std::vector<double> in_e(total, 0), out_e(total, -1);  // by source callback
    std::vector<char> submitted(total, 0);
    uint64_t dropped = 0;
    Signal sig(a.channels);
    const LoopStats st = run_periodic(clock, total, [&](uint64_t i) {
        // The first phase is set, not assumed: a plugin's described default
        // is what the host was seeded with (AGain's is -6 dB).
        if (i == 0) host.send(0, uint32_t(gp), 0, value_of(phases[0].db));
        if (i == phases[1].from) host.send(0, uint32_t(gp), 0, value_of(phases[1].db));
        if (i == phases[2].from) host.send_table_only(0, uint32_t(gp), value_of(phases[2].db));
        sig.sine(i, 997.0, 0.1f);
        for (uint32_t c = 0; c < a.channels; ++c)
            for (uint32_t s = 0; s < kBlock; ++s) in_e[i] += double(sig.buf[c][s]) * sig.buf[c][s];
        const int64_t from = host.run(sig.in.data(), sig.out.data(), kBlock, i);
        submitted[i] = host.submitted_at() == i;
        if (from < 0) {
            ++dropped;
            return true;
        }
        double e = 0;
        for (uint32_t c = 0; c < a.channels; ++c)
            for (uint32_t s = 0; s < kBlock; ++s) e += double(sig.obuf[c][s]) * sig.obuf[c][s];
        out_e[size_t(from)] = e;
        return true;
    });
    report_loop(st);
    std::printf("  dropped %llu of %llu blocks\n", (unsigned long long)dropped, (unsigned long long)total);

    auto block_db = [&](uint64_t b) { return 10.0 * std::log10(out_e[b] / in_e[b]); };
    for (int k = 0; k < 3; ++k) {
        const uint64_t end = k < 2 ? phases[k + 1].from : total - 1;
        double in_sum = 0, out_sum = 0;
        for (uint64_t b = phases[k].from + 10; b < end; ++b)
            if (out_e[b] >= 0) in_sum += in_e[b], out_sum += out_e[b];
        const double db = 10.0 * std::log10(out_sum / in_sum);
        char what[128];
        std::snprintf(what, sizeof what, "gain %+.1f dB measured %+.4f dB (tolerance 0.01)",
                      phases[k].db, db);
        check(in_sum > 0 && std::fabs(db - phases[k].db) < 0.01, what);
    }
    for (int k = 1; k < 3; ++k) {
        uint64_t land = phases[k].from;
        while (land < total && !submitted[land]) ++land;
        bool exact = land < total;
        uint64_t seen = 0;
        for (uint64_t b = phases[k].from > 20 ? phases[k].from - 20 : 0; b < phases[k].from + 20; ++b) {
            if (out_e[b] < 0) continue;
            const float want = b >= land ? phases[k].db : phases[k - 1].db;
            if (std::fabs(block_db(b) - want) > 0.01) exact = false;
            ++seen;
        }
        char what[192];
        std::snprintf(what, sizeof what,
                      "%+.0f dB sent in callback %llu %s lands in block %llu, the first submitted after it",
                      phases[k].db, (unsigned long long)phases[k].from,
                      k == 1 ? "through the ring" : "through the table alone", (unsigned long long)land);
        check(exact && seen > 20, what);
    }
    check(dropped * 20 < total, "fewer than 5 % of blocks dropped");
    return 0;
}

// PDC delays, the arithmetic of brain/specs/plugin-latency-compensation.md:
// every path is delayed to arrive with the longest one.
std::vector<uint32_t> pdc_delays(const std::vector<uint32_t>& path_latency) {
    const uint32_t longest = *std::max_element(path_latency.begin(), path_latency.end());
    std::vector<uint32_t> d;
    for (uint32_t l : path_latency) d.push_back(longest - l);
    return d;
}

// The delay that best explains `out` as `in` delayed, over the samples
// marked valid, and how well the reported latency L does.
struct DelayFit {
    uint32_t best = 0;
    double at = 1e9, before = 1e9, after = 1e9;
};

double residual_at(const std::vector<float>& in, const std::vector<float>& out, const std::vector<char>& valid,
                   uint32_t d) {
    double worst = 0;
    uint64_t n = 0;
    for (uint64_t t = d; t < out.size(); ++t)
        if (valid[t]) {
            worst = std::max(worst, std::fabs(double(out[t]) - double(in[t - d])));
            ++n;
        }
    return n > 4 * kBlock ? worst : 1e9;
}

DelayFit fit_delay(const std::vector<float>& in, const std::vector<float>& out,
                   const std::vector<char>& valid, uint32_t L) {
    DelayFit f;
    double best_r = 1e9;
    for (uint32_t d = 0; d <= L + 4 * kBlock; ++d) {
        const double r = residual_at(in, out, valid, d);
        if (r < best_r) best_r = r, f.best = d;
    }
    f.at = residual_at(in, out, valid, L);
    f.before = L ? residual_at(in, out, valid, L - 1) : 1e9;
    f.after = residual_at(in, out, valid, L + 1);
    return f;
}

void check_delay(const DelayFit& f, uint32_t L) {
    std::printf("  measured delay %u frames; residual at L %.2e (%.1f dBFS), at L-1 %.2e, at L+1 %.2e\n",
                f.best, f.at, f.at > 0 ? 20 * std::log10(f.at) : -999.0, f.before, f.after);
    check(f.best == L, "the measured delay equals the reported latency, to the sample");
    check(f.at < 1e-4, "PDC-aligned wet path nulls against dry below -80 dBFS");
    check(f.before > 100 * std::max(f.at, 1e-7) && f.after > 100 * std::max(f.at, 1e-7),
          "one sample off is audible: the check is sample-accurate");
}

std::vector<float> noise(uint64_t samples) {
    std::vector<float> in(samples);
    uint32_t lcg = 12345;
    for (float& s : in) {
        lcg = lcg * 1664525u + 1013904223u;
        s = 0.1f * (float(lcg >> 8) / float(1u << 24) * 2.f - 1.f);  // -20 dBFS peak noise
    }
    return in;
}

// Samples comparable over [from, to): their block arrived and no dropped
// block lies within L + 2 blocks before (a drop is a gap in the plugin's own
// input history). Callback 0 has nothing to collect at depth 1.
std::vector<char> valid_window(const std::vector<char>& got, uint64_t from, uint64_t to, uint32_t L) {
    std::vector<char> valid(got.size() * kBlock, 0);
    const uint64_t reach = (L + kBlock - 1) / kBlock + 2;
    uint64_t last_drop = 0;
    bool any_drop = false;
    for (uint64_t b = 0; b < to && b < got.size(); ++b) {
        if (!got[b] && b != 0) any_drop = true, last_drop = b;
        const bool ok = got[b] && b >= from && (!any_drop || b > last_drop + reach);
        std::fill(valid.begin() + std::ptrdiff_t(b * kBlock), valid.begin() + std::ptrdiff_t((b + 1) * kBlock),
                  ok ? 1 : 0);
    }
    return valid;
}

uint64_t count_drops(const std::vector<char>& got) {
    uint64_t n = 0;
    for (uint64_t b = 1; b < got.size(); ++b) n += got[b] ? 0 : 1;
    return n;
}

// A latency-reporting chain: what it reports is what it does, to the sample,
// and PDC built on the report nulls the wet path against the dry one. The
// report includes the block of IPC delay (depth 1).
int scenario_latency(const Args& a) {
    const std::vector<std::string> chain = a.chain.empty() ? std::vector<std::string>{kDpl} : a.chain;
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    std::printf("latency: %s, mono, %s wake\n", join(chain, ',').c_str(),
                clock.mode == plugin::WakeMode::Futex ? "futex" : "timer");
    Host host;
    if (int r = start_ready(host, a, clock, chain, 1)) return r;
    const uint32_t gen0 = host.h->latency_generation.load();

    const uint64_t total = a.blocks ? a.blocks : 900;
    const std::vector<float> in = noise(total * kBlock);
    std::vector<float> out(total * kBlock, 0.f);
    std::vector<char> got(total, 0);
    Signal sig(1);
    const LoopStats st = run_periodic(clock, total, [&](uint64_t i) {
        std::memcpy(sig.buf[0].data(), &in[i * kBlock], kBlock * sizeof(float));
        // Stored by the callback that played it, so the IPC block shows up
        // as delay exactly like a plugin's look-ahead.
        if (host.run(sig.in.data(), sig.out.data(), kBlock, i) >= 0) {
            std::memcpy(&out[i * kBlock], sig.obuf[0].data(), kBlock * sizeof(float));
            got[i] = 1;
        }
        return true;
    });
    report_loop(st);

    const uint32_t L = plugin::checked_latency(host.h->latency_frames.load());
    check(L != plugin::kLatencyInvalid, "the reported latency is plausible");
    if (L == plugin::kLatencyInvalid) return 0;
    check(host.h->latency_generation.load() == gen0, "latency did not change while running");
    // Two tracks into one bus: dry (latency 0) and this chain (L).
    const std::vector<uint32_t> delays = pdc_delays({0, L});
    std::printf("  reported %u frames (%.3f ms); PDC delays dry by %u, wet by %u\n", L,
                L * 1000.0 / kRate, delays[0], delays[1]);
    const uint64_t dropped = count_drops(got);
    std::printf("  dropped %llu of %llu blocks\n", (unsigned long long)dropped, (unsigned long long)total);
    check_delay(fit_delay(in, out, valid_window(got, (L + kBlock - 1) / kBlock + 16, total, L), L), L);
    check(dropped * 20 < total, "fewer than 5 % of blocks dropped");
    return 0;
}

// A plugin whose latency changes while running (VST3: the LSP limiter's
// look-ahead, reported by restartComponent(kLatencyChanged)): the host
// publishes the new value with a new generation, and the new value is what
// the plugin then does, to the sample. The blocks between the change and the
// report are counted, not compared: that lag is how long PDC is wrong.
int scenario_latency_change(const Args& a) {
    if (a.chain.empty() || a.change_param.empty()) {
        std::printf("latency_change: needs --chain and --change-param/--change-value\n");
        return 1;
    }
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    std::printf("latency_change: %s, mono, param %s := %g at block 300\n", join(a.chain, ',').c_str(),
                a.change_param.c_str(), double(a.change_value));
    Host host;
    if (int r = start_ready(host, a, clock, a.chain, 1)) return r;
    const int cp = dense_param(a, a.chain[0], a.change_param);
    if (cp < 0) return 1;
    const uint32_t L0 = plugin::checked_latency(host.h->latency_frames.load());
    const uint32_t gen0 = host.h->latency_generation.load();

    const uint64_t total = a.blocks ? a.blocks : 1200, change_at = 300;
    const std::vector<float> in = noise(total * kBlock);
    std::vector<float> out(total * kBlock, 0.f);
    std::vector<char> got(total, 0);
    std::vector<uint32_t> reported(total, plugin::kLatencyInvalid);
    Signal sig(1);
    const LoopStats st = run_periodic(clock, total, [&](uint64_t i) {
        if (i == change_at) host.send(0, uint32_t(cp), 0, a.change_value);
        std::memcpy(sig.buf[0].data(), &in[i * kBlock], kBlock * sizeof(float));
        if (host.run(sig.in.data(), sig.out.data(), kBlock, i) >= 0) {
            std::memcpy(&out[i * kBlock], sig.obuf[0].data(), kBlock * sizeof(float));
            got[i] = 1;
        }
        reported[i] = plugin::checked_latency(host.h->latency_frames.load(std::memory_order_acquire));
        return true;
    });
    report_loop(st);
    check(host.alive(), "the host is still running");
    const uint32_t L1 = reported.back();
    check(L0 != plugin::kLatencyInvalid && L1 != plugin::kLatencyInvalid, "both reported latencies are plausible");
    if (L0 == plugin::kLatencyInvalid || L1 == plugin::kLatencyInvalid) return 0;
    check(L1 != L0 && host.h->latency_generation.load() != gen0,
          "the new latency was published with a new generation");
    uint64_t settled = total;
    for (uint64_t b = total; b-- > change_at;)
        if (reported[b] == L1) settled = b;
        else break;
    std::printf("  reported %u frames before, %u after; the report changed %llu block(s) after the push\n",
                L0, L1, (unsigned long long)(settled - change_at));
    check(settled < change_at + 40, "the change was reported within 40 blocks (107 ms)");
    std::printf("  before the change:\n");
    check_delay(fit_delay(in, out, valid_window(got, (L0 + kBlock - 1) / kBlock + 16, change_at, L0), L0), L0);
    std::printf("  after the report settled:\n");
    const uint64_t from1 = settled + (std::max(L0, L1) + kBlock - 1) / kBlock + 16;
    check_delay(fit_delay(in, out, valid_window(got, from1, total, L1), L1), L1);
    if (host.controlled()) {
        // The socket's notice follows the region's bump and agrees with it
        // (host_control.h, ordering): the control thread need not poll.
        pc::LatencyChanged last{};
        bool any = false;
        for (pc::LatencyChanged m; host.expect_message(pc::Type::LatencyChanged, m, any ? 0 : 2000);) {
            last = m;
            any = true;
        }
        std::printf("  LatencyChanged: generation %u, %u frames; the region: generation %u, %u frames\n",
                    last.latency_generation, last.latency_frames, host.h->latency_generation.load(), L1);
        check(any && last.latency_generation == host.h->latency_generation.load() && last.latency_frames == L1,
              "LatencyChanged came on the socket with the region's generation and figure");
    }
    return 0;
}

// A plugin that segfaults takes its host with it and nothing else: every
// callback still returns inside the period, the death is reported, and the
// culprit is named.
int scenario_crash(const Args& a) {
    // Index 1 is the crashing plugin and its parameter 0 the trigger, in
    // either format (tests/lv2, tests/vst3).
    const std::vector<std::string> chain =
        a.chain.empty() ? std::vector<std::string>{kEgAmp, "urn:sonora:test#crash"} : a.chain;
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    std::printf("crash: %s\n", join(chain, ',').c_str());
    Host host;
    if (int r = start_ready(host, a, clock, chain, 1)) return r;
    const uint64_t total = 400, trigger = 100;
    uint64_t gone_at = 0, completed_after = 0;
    int32_t culprit = -2;
    Signal sig(1);
    const LoopStats st = run_periodic(clock, total, [&](uint64_t i) {
        if (i == trigger) host.send(1, 0, 0, 1.f);  // crash := 1
        sig.sine(i, 440.0, 0.1f);
        const bool ok = host.run(sig.in.data(), sig.out.data(), kBlock, i) >= 0;
        if (!ok) sig.silence_out();
        if (gone_at && ok) ++completed_after;
        // The control thread's job in the engine (a pidfd there). Polled
        // here for brevity; it is one non-blocking waitpid, and the
        // over-period check below still holds with it inside the callback.
        if (!gone_at && !host.alive()) {
            gone_at = i;
            culprit = plugin::checked_instance(host.h->current_instance.load(std::memory_order_relaxed),
                                               uint32_t(chain.size()));
        }
        return true;
    });
    report_loop(st);
    std::printf("  host gone at block %llu: %s; it was running instance %d (%s)\n",
                (unsigned long long)gone_at, host.death().c_str(), culprit, chain_name(chain, culprit));
    check(gone_at >= trigger && gone_at < trigger + 50, "the host's death was detected within 50 blocks");
    check(host.reaped() && WIFSIGNALED(host.status()) && WTERMSIG(host.status()) == SIGSEGV,
          "the host died of SIGSEGV");
    check(culprit == 1, "the crashing plugin is named (chain index 1)");
    check(completed_after == 0, "no block completed after the host died");
    check(st.callbacks == total, "the driver ran every callback after the crash");
    check(st.over_period <= kOsNoiseCallbacks, "no callback overran the period while the host died");
    return 0;
}

// A plugin that stops returning in time is dropped per block, never waited
// on, and removed after kMaxConsecutiveMisses with its name.
int scenario_deadline(const Args& a) {
    const std::vector<std::string> chain = {kEgAmp, "urn:sonora:test#sleep"};
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    std::printf("deadline: %s, sleep 20 ms from block 100\n", join(chain, ',').c_str());
    Host host;
    if (int r = start_ready(host, a, clock, chain, 1)) return r;
    const uint64_t total = 300, trigger = 100;
    int streak = 0;
    int32_t culprit = -2;
    uint64_t removed_at = 0, misses_before = 0;
    Signal sig(1);
    const LoopStats st = run_periodic(clock, total, [&](uint64_t i) {
        if (i == trigger) host.send(1, 0, 0, 20.f);  // sleep_ms := 20
        sig.sine(i, 440.0, 0.1f);
        if (removed_at) return true;  // out of the plan: the path is silent (ADR-0012 §3)
        if (host.run(sig.in.data(), sig.out.data(), kBlock, i) >= 0) {
            streak = 0;
            return true;
        }
        // Depth 1 has nothing to collect in callback 0; that is not a miss.
        if (i == 0) return true;
        if (i < trigger) ++misses_before;
        // Remember who was running during the streak (finding 6): the last
        // miss can fall just after the slow block finished, when
        // current_instance is already -1.
        const int32_t running = plugin::checked_instance(
            host.h->current_instance.load(std::memory_order_relaxed), uint32_t(chain.size()));
        if (streak == 0) culprit = -1;
        if (running >= 0) culprit = running;
        if (++streak >= kMaxConsecutiveMisses) removed_at = i;
        return true;
    });
    host.kill_now();
    report_loop(st);
    std::printf("  removed at block %llu after %d consecutive misses; it was in instance %d (%s); "
                "%llu misses before the trigger\n",
                (unsigned long long)removed_at, kMaxConsecutiveMisses, culprit,
                chain_name(chain, culprit), (unsigned long long)misses_before);
    check(removed_at >= trigger && removed_at < trigger + 2 * kMaxConsecutiveMisses,
          "the late host left the plan after the miss limit");
    check(culprit == 1, "the late plugin is named (chain index 1)");
    check(st.callbacks == total, "the driver ran every callback");
    check(st.over_period <= kOsNoiseCallbacks, "no callback waited past the period on a sleeping plugin");
    return 0;
}

// A plugin that writes NaN and Inf while running: the host replaces them with
// 0 before the next plugin and the engine see them, counts them against that
// plugin and nobody else, and the chain is clean again once it stops.
int scenario_nonfinite(const Args& a) {
    const std::vector<std::string> chain = {"urn:sonora:test#nan", kEgAmp};
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    std::printf("nonfinite: %s, NaN/Inf from block 100 to 200\n", join(chain, ',').c_str());
    Host host;
    if (int r = start_ready(host, a, clock, chain, 1)) return r;
#if defined(__x86_64__) || defined(__i386__) || defined(__aarch64__)
    check(host.h->host_flags.load() & plugin::kHostFlushDenormals,
          "the host set FTZ/DAZ on its processing thread");
#endif
    const uint64_t total = 300, on = 100, off = 200;
    uint64_t nonfinite_out = 0, zeroed = 0, bad_blocks = 0, clean_after = 0, compared_after = 0;
    std::vector<std::vector<float>> inputs(total, std::vector<float>(kBlock));
    Signal sig(1);
    const LoopStats st = run_periodic(clock, total, [&](uint64_t i) {
        if (i == on) host.send(0, 0, 0, 1.f);
        if (i == off) host.send(0, 0, 0, 0.f);
        sig.sine(i, 440.0, 0.1f);
        std::copy(sig.buf[0].begin(), sig.buf[0].begin() + kBlock, inputs[i].begin());
        const int64_t from = host.run(sig.in.data(), sig.out.data(), kBlock, i);
        if (from < 0) return true;
        const float* o = sig.obuf[0].data();
        bool had_zero = false, equal = true;
        for (uint32_t s = 0; s < kBlock; ++s) {
            if (!std::isfinite(o[s])) ++nonfinite_out;
            if (s % 4 == 0 && o[s] == 0.f && inputs[size_t(from)][s] != 0.f) had_zero = true;
            if (o[s] != inputs[size_t(from)][s]) equal = false;
        }
        if (had_zero) ++bad_blocks, zeroed += kBlock / 4;
        if (uint64_t(from) > off + 2) {
            ++compared_after;
            if (equal) ++clean_after;
        }
        return true;
    });
    report_loop(st);
    const uint64_t counted0 = host.h->instances[0].nonfinite_samples.load();
    const uint64_t counted1 = host.h->instances[1].nonfinite_samples.load();
    std::printf("  %llu blocks carried replaced samples; host counted %llu against [0], %llu against [1]\n",
                (unsigned long long)bad_blocks, (unsigned long long)counted0, (unsigned long long)counted1);
    check(nonfinite_out == 0, "no NaN or Inf reached the engine");
    check(host.nonfinite_collected() == 0, "the engine's own check of what it collected found none (M4)");
    check(bad_blocks >= 50 && counted0 >= zeroed && counted0 % (kBlock / 4) == 0,
          "every NaN/Inf was replaced by 0 and counted against the plugin that wrote it");
    check(counted1 == 0, "the plugin after it saw only finite input and is not blamed");
    check(compared_after > 50 && clean_after == compared_after,
          "once the plugin stops, the chain passes the input unchanged");
    return 0;
}

// The worker test plugin's output, block by block: the input times 1, times
// 0.5, or neither. The plugin goes silent for good when it sees the worker
// broken (work() on the run thread, a response elsewhere or half of one,
// no end_run), which shows as neither.
struct Halving {
    uint64_t unity = 0, halved = 0, other = 0, unity_after_half = 0;
    int64_t first_half = -1;
    LoopStats loop;
    void report() const {
        report_loop(loop);
        std::printf("  %llu blocks at unity, %llu halved (from block %lld), %llu neither\n",
                    (unsigned long long)unity, (unsigned long long)halved, (long long)first_half,
                    (unsigned long long)other);
    }
};

template <class F>
Halving play_halving(Host& host, Clock& clock, uint64_t total, F&& before) {
    Halving h;
    std::vector<std::vector<float>> inputs(total, std::vector<float>(kBlock));
    Signal sig(1);
    h.loop = run_periodic(clock, total, [&](uint64_t i) {
        before(i);
        sig.sine(i, 440.0, 0.25f);
        std::copy(sig.buf[0].begin(), sig.buf[0].begin() + kBlock, inputs[i].begin());
        const int64_t from = host.run(sig.in.data(), sig.out.data(), kBlock, i);
        if (from < 0) return true;
        const float* o = sig.obuf[0].data();
        const std::vector<float>& x = inputs[size_t(from)];
        bool is_unity = true, is_half = true;
        for (uint32_t s = 0; s < kBlock; ++s) {
            if (o[s] != x[s]) is_unity = false;
            if (o[s] != 0.5f * x[s]) is_half = false;
        }
        if (is_half) {
            ++h.halved;
            if (h.first_half < 0) h.first_half = from;
        } else if (is_unity) {
            ++h.unity;
            if (h.first_half >= 0) ++h.unity_after_half;
        } else {
            ++h.other;
        }
        return true;
    });
    return h;
}

// LV2 worker:schedule: the test plugin applies a gain only once work() has
// answered for it with two responses, so the step reaches the output only
// through a worker thread, both responses handed back inside run(), and
// end_run() after each run(). The output is the input times 1 until then,
// times 0.5 after, and nothing else.
int scenario_worker(const Args& a) {
    const std::vector<std::string> chain = {"urn:sonora:test#worker"};
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    std::printf("worker: %s, gain 1 -> 0.5 at block 100 through work()\n", join(chain, ',').c_str());
    Host host;
    if (int r = start_ready(host, a, clock, chain, 1)) return r;
    const uint64_t total = 600, step = 100;
    // Generous for a loaded CI box: the work itself sleeps 2 ms.
    const uint64_t within = 100;
    const Halving h = play_halving(host, clock, total, [&](uint64_t i) {
        if (i == step) host.send(0, 0, 0, 0.5f);
    });
    h.report();
    check(h.other == 0, "every block is the input times 1 or times 0.5: the plugin never saw a broken worker");
    check(h.first_half >= int64_t(step) && h.first_half < int64_t(step + within),
          "the gain the work answered for reached the output within 100 blocks of the change");
    check(h.unity_after_half == 0, "and stayed there");
    check(h.unity >= step / 2, "before the change, the input passed at unity");
    return 0;
}

bool wait_exit(Host& host, uint64_t timeout_ns) {
    const uint64_t end = monotonic_ns() + timeout_ns;
    while (host.alive() && monotonic_ns() < end) usleep(5000);
    return host.reaped();
}

// What a plugin declares is checked against what it does before the engine
// is offered the chain; a refused chain exits kHostExitRefused (5).
int scenario_refusals(const Args& a) {
    struct Case {
        const char* what;
        std::vector<std::string> chain;
        uint32_t channels;
        plugin::Refusal want;
        int instance;
        int param_delta = 0;
        uint32_t key_channels = 0;  // of `channels`, the last ones (host --sidechain)
        bool reactivate = false;    // host --reactivate
    };
    const std::vector<Case> lv2_cases = {
        {"declares 2 channels, writes 8: refused", {"urn:sonora:test#liar"}, 2,
         plugin::Refusal::WroteOutsideBuffers, 0, 0},
        {"stereo-only plugin on a mono chain: refused", {"urn:sonora:test#liar"}, 1,
         plugin::Refusal::ChannelMismatch, 0, 0},
        {"unknown URI: refused, and named", {"urn:sonora:test#crash", "urn:sonora:test#absent"}, 1,
         plugin::Refusal::NotFound, 1, 0},
        {"parameter list other than the scan's: refused, and named", {kEgAmp}, 1,
         plugin::Refusal::ParamMismatch, 0, 1},
        // The key channels have canaries behind them like every other buffer.
        {"writes past its key input: refused", {kKeyLiar}, 2, plugin::Refusal::WroteOutsideBuffers, 0, 0, 1},
        // With no key routed the key input reads the plugin's own input,
        // whose canaries catch the same write.
        {"writes past its key input, no key routed: refused", {kKeyLiar}, 1,
         plugin::Refusal::WroteOutsideBuffers, 0},
    };
    // tests/vst3/sonora_test_vst3.cpp
    const std::vector<Case> vst3_cases = {
        {"VST3 stereo-only plugin on a mono chain: refused", {kVst3StereoOnly}, 1,
         plugin::Refusal::ChannelMismatch, 0, 0},
        {"VST3 plugin that says yes to mono and stays stereo: refused", {kVst3YesMan}, 1,
         plugin::Refusal::ChannelMismatch, 0, 0},
        {"VST3 plugin that declares 2 channels and writes 8: refused", {kVst3Liar}, 2,
         plugin::Refusal::WroteOutsideBuffers, 0, 0},
        // Once read 8 channels for a 2-channel chain and indexed past it.
        {"VST3 plugin that takes stereo, then answers 8 channels: refused", {kVst3Turncoat}, 2,
         plugin::Refusal::ChannelMismatch, 0, 0},
        {"unknown VST3 class: refused, and named", {kVst3Crash, "vst3:00000000000000000000000000000000"},
         1, plugin::Refusal::NotFound, 1, 0},
        {"VST3 parameter list other than the scan's: refused, and named", {kVst3Crash}, 1,
         plugin::Refusal::ParamMismatch, 0, -1},
        // Re-activation (host --reactivate) is checked like the first one:
        // a plugin that will not come back is refused, not run inactive.
        {"VST3 plugin that refuses to re-activate: refused", {kVst3Once}, 1,
         plugin::Refusal::InstantiateFailed, 0, 0, 0, true},
    };
    const std::vector<Case>& cases = a.format == "vst3" ? vst3_cases : lv2_cases;
    std::printf("refusals (%s)\n", a.format.c_str());
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    for (const Case& c : cases) {
        Host host;
        Args ca = a;
        ca.key_channels = c.key_channels;
        ca.reactivate = c.reactivate;
        Host::StartOptions o;
        o.channels = c.channels;
        o.param_delta = c.param_delta;
        if (!host.start(ca, clock, c.chain, o)) return 1;
        const plugin::HostState s = host.wait_ready(30'000'000'000ull);
        const auto reason = static_cast<plugin::Refusal>(host.h->refusal.load());
        const int inst = plugin::checked_instance(host.h->refusal_instance.load(), uint32_t(c.chain.size()));
        wait_exit(host, 2'000'000'000ull);
        std::printf("  [%s] state %s, refusal %u, instance %d, host %s\n", join(c.chain, ',').c_str(),
                    state_name(uint32_t(s)), unsigned(reason), inst, host.death().c_str());
        check(s == plugin::HostState::Refused && reason == c.want && inst == c.instance, c.what);
        check(host.reaped() && WIFEXITED(host.status()) && WEXITSTATUS(host.status()) == plugin::kHostExitRefused,
              "  ...and the host exited with code 5");
    }
    return 0;
}

// A host that does not speak the engine's layout or clock version exits 3,
// after one stderr line, BEFORE writing anything to the region (ADR-0012
// §5.5); a region that is not a channel at all is a usage error (2). The
// clock descriptor hosts get cannot be mapped writable (auditor M1).
int scenario_incompatible(const Args& a) {
    struct Case {
        const char* what;
        std::function<void(plugin::ChannelHeader&)> channel;
        std::function<void(plugin::ClockHeader&)> clock;
        int want_exit;
    };
    const std::vector<Case> cases = {
        {"channel layout version 0 (the draft): exit 3, nothing written",
         [](plugin::ChannelHeader& h) { h.layout_version = 0; }, {}, plugin::kHostExitIncompatible},
        {"channel layout version 2: exit 3, nothing written",
         [](plugin::ChannelHeader& h) { h.layout_version = 2; }, {}, plugin::kHostExitIncompatible},
        {"channel header of another size: exit 3, nothing written",
         [](plugin::ChannelHeader& h) { h.header_bytes -= 64; }, {}, plugin::kHostExitIncompatible},
        {"clock layout version 2: exit 3, nothing written", {},
         [](plugin::ClockHeader& c) { c.layout_version = 2; }, plugin::kHostExitIncompatible},
        {"not a channel (magic): exit 2, nothing written",
         [](plugin::ChannelHeader& h) { h.magic ^= 1; }, {}, plugin::kHostExitUsage},
        {"channel block_frames other than the clock's: exit 2, nothing written",
         [](plugin::ChannelHeader& h) { h.block_frames = 2 * kBlock; }, {}, plugin::kHostExitUsage},
        {"channel sample_rate other than the clock's: exit 2, nothing written",
         [](plugin::ChannelHeader& h) { h.sample_rate = 44100; }, {}, plugin::kHostExitUsage},
    };
    std::printf("incompatible\n");
    for (const Case& c : cases) {
        Clock clock;
        if (!clock.create(wake_of(a), c.clock)) return 1;
        Host host;
        Host::StartOptions o;
        std::vector<unsigned char> before;
        o.mutate = [&](plugin::ChannelHeader& h) {
            if (c.channel) c.channel(h);
            const auto* p = reinterpret_cast<const unsigned char*>(&h);
            before.assign(p, p + plugin::make_layout(1, kMaxFrames, h.param_slots).region_size);
        };
        if (!host.start(a, clock, {kEgAmp}, o)) return 1;
        const bool exited = wait_exit(host, 10'000'000'000ull);
        const bool untouched = before.size() <= host.region_size() &&
                               std::memcmp(host.region_base(), before.data(), before.size()) == 0;
        std::printf("  host %s; region %s\n", host.death().c_str(), untouched ? "untouched" : "WRITTEN");
        check(exited && WIFEXITED(host.status()) && WEXITSTATUS(host.status()) == c.want_exit && untouched,
              c.what);
    }
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    void* w = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, clock.ro_handle, 0);
    const int err = errno;
    check(w == MAP_FAILED && err == EACCES, "the clock descriptor a host gets cannot be mapped writable");
    if (w != MAP_FAILED) munmap(w, 4096);
    void* r = mmap(nullptr, 4096, PROT_READ, MAP_SHARED, clock.ro_handle, 0);
    check(r != MAP_FAILED, "...and maps read-only");
    if (r != MAP_FAILED) munmap(r, 4096);
    // A memfd inode is mode 0777, so a host can reopen it read-write
    // through /proc; F_SEAL_FUTURE_WRITE is what stops the mapping (L2).
    const std::string proc = "/proc/self/fd/" + std::to_string(clock.ro_handle);
    const int rw = open(proc.c_str(), O_RDWR | O_CLOEXEC);
    void* w2 = rw >= 0 ? mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, rw, 0) : MAP_FAILED;
    check(w2 == MAP_FAILED, "...nor through a read-write reopen of /proc/self/fd (the seal)");
    if (w2 != MAP_FAILED) munmap(w2, 4096);
    if (rw >= 0) close(rw);

    // The --version line parses with the engine's parser (§5.5 grammar).
    {
        const std::string cmd = "'" + a.host + "' --version";
        FILE* f = popen(cmd.c_str(), "r");
        char line[128] = {};
        const bool read = f && std::fgets(line, sizeof line, f) != nullptr;
        const int status = f ? pclose(f) : -1;
        uint32_t layout = 0, clock_version = 0;
        std::printf("  --version: %s", read ? line : "(nothing)\n");
        check(read && status == 0 && plugin::parse_version_line(line, layout, clock_version) &&
                  layout == plugin::kLayoutVersion && clock_version == plugin::kClockLayoutVersion,
              "--version prints the contract's line, with this layout and clock version");
    }

    // A frame count other than block_frames can only be a scribble (M3):
    // the host processes nothing and faults.
    {
        Host host;
        if (int r2 = start_ready(host, a, clock, {kEgAmp}, 1)) return r2;
        host.h->submitted_frames.store(kBlock / 2, std::memory_order_relaxed);
        host.h->submitted_block.store(1, std::memory_order_release);
        clock.begin(monotonic_ns());
        clock.submitted();
        clock.end();
        clock.wake();
        const bool exited = wait_exit(host, 10'000'000'000ull);
        std::printf("  forged frame count: host %s, state %s, completed %llu\n", host.death().c_str(),
                    state_name(host.h->state.load()), (unsigned long long)host.h->completed_block.load());
        check(exited && WIFEXITED(host.status()) && WEXITSTATUS(host.status()) == plugin::kHostExitFaulted &&
                  host.h->completed_block.load() == 0,
              "a submitted_frames other than block_frames faults the host, nothing processed");
    }
    return 0;
}

// kBypassParam (ADR-0012 §5.1): bypass through the ring, then back through
// the last-value table. Bypassed, the chain plays its input delayed by
// exactly the latency it reports, so a toggle never shifts timing; active,
// it plays the plugin. Host-side (a delay line) for a plugin without a
// bypass parameter, the plugin's own (VST3 kIsBypass) otherwise.
int scenario_bypass(const Args& a) {
    const std::vector<std::string> chain = a.chain.empty() ? std::vector<std::string>{kEgAmp} : a.chain;
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    const PluginParams described = probe_params(a, chain[0]);
    std::printf("bypass: %s, mono, bypass by the %s\n", join(chain, ',').c_str(),
                described.bypass == "plugin" ? "plugin's own parameter" : "host's delay line");
    if (!a.expect_bypass.empty()) check(described.bypass == a.expect_bypass, "the probe names the bypass expected");
    Host host;
    if (int r = start_ready(host, a, clock, chain, 1)) return r;
    int gp = -1;
    if (!a.gain_param.empty() && (gp = dense_param(a, chain[0], a.gain_param)) < 0) return 1;
    const float gain_db = -6.f;
    const float gain_value = a.gain_linear ? float(std::pow(10.0, gain_db / 20.0)) : gain_db;
    const uint32_t L = plugin::checked_latency(host.h->latency_frames.load());
    const uint32_t gen0 = host.h->latency_generation.load();
    if (L == plugin::kLatencyInvalid) {
        check(false, "the reported latency is plausible");
        return 0;
    }

    const uint64_t total = 900, on = 300, off = 600;
    const std::vector<float> in = noise(total * kBlock);
    std::vector<float> out(total * kBlock, 0.f);
    std::vector<char> got(total, 0);
    Signal sig(1);
    const LoopStats st = run_periodic(clock, total, [&](uint64_t i) {
        if (i == 0 && gp >= 0) host.send(0, uint32_t(gp), 0, gain_value);
        if (i == on) host.send(0, plugin::kBypassParam, 0, 1.f);
        if (i == off) host.send_table_only(0, plugin::kBypassParam, 0.f);
        std::memcpy(sig.buf[0].data(), &in[i * kBlock], kBlock * sizeof(float));
        if (host.run(sig.in.data(), sig.out.data(), kBlock, i) >= 0) {
            std::memcpy(&out[i * kBlock], sig.obuf[0].data(), kBlock * sizeof(float));
            got[i] = 1;
        }
        return true;
    });
    report_loop(st);
    std::printf("  reported %u frames; dropped %llu of %llu blocks\n", L,
                (unsigned long long)count_drops(got), (unsigned long long)total);
    check(host.h->latency_generation.load() == gen0, "bypass did not change the reported latency");
    // One block for the crossfade, and L + 2 blocks for the history.
    const uint64_t settle = (L + kBlock - 1) / kBlock + 3;
    const auto bypassed = valid_window(got, on + settle, off, L);
    const double r = residual_at(in, out, bypassed, L);
    std::printf("  bypassed: worst |out - in delayed by %u| = %.3g\n", L, r);
    check(r < 1e-6, "bypassed, the chain plays its input delayed by exactly the reported latency");
    for (const auto& w : {std::make_pair(uint64_t(16) + settle, on), std::make_pair(off + settle, total)}) {
        const auto valid = valid_window(got, w.first, w.second, L);
        if (gp >= 0) {
            double ei = 0, eo = 0;
            for (uint64_t t = L; t < out.size(); ++t)
                if (valid[t]) ei += double(in[t - L]) * in[t - L], eo += double(out[t]) * out[t];
            const double db = 10.0 * std::log10(eo / ei);
            char what[128];
            std::snprintf(what, sizeof what, "active from block %llu: %+.4f dB, the plugin's %+.1f dB",
                          (unsigned long long)w.first, db, gain_db);
            check(ei > 0 && std::fabs(db - gain_db) < 0.01, what);
        } else {
            check_delay(fit_delay(in, out, valid, L), L);
        }
    }
    return 0;
}

// Make-before-break (ADR-0012 §5.3, §5.4): a chain moves to a new host, in a
// NEW region and a new channel slot, while the old host keeps playing; the
// plan swap happens in a callback, the old host is shut down and its region
// released only after the old plan is retired. The new channel's first
// submit copies the engine's private store in, so a value set after the new
// host was seeded is the one it plays (A2). A slot reassigned later no
// longer matches a plan built against its old generation (H1).
int scenario_swap(const Args& a) {
    const std::vector<std::string> chain = a.chain.empty() ? std::vector<std::string>{kEgAmp} : a.chain;
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    std::printf("swap: %s, make-before-break into a new region and slot\n", join(chain, ',').c_str());
    const int gp = dense_param(a, chain[0], a.gain_param.empty() ? "gain" : a.gain_param);
    if (gp < 0) return 1;
    // The chain's store, keyed by the chain, outlives both hosts.
    Store store;
    {
        const PluginParams d = probe_params(a, chain[0]);
        const uint32_t n = uint32_t(d.keys.size());
        store.map = plugin::make_param_map(&n, 1);
        store.values = d.defaults;
        store.values.resize(store.map.param_slots);
    }
    plugin::SlotGeneration slots[2];
    struct Node {
        int slot;
        uint32_t generation;
        Host* host;
    };
    Host A, B;
    Host::StartOptions o;
    o.store = &store;
    if (int r = start_ready(A, a, clock, chain, o)) return r;
    Node plans[2] = {{0, plugin::assign_slot(slots[0]), &A}, {1, 0, &B}};

    std::atomic<bool> start_b{false}, b_ready{false}, retired{false}, finished{false};
    std::atomic<int> b_failed{0};
    bool a_alive_at_retire = false;
    std::thread control([&] {
        while (!start_b.load(std::memory_order_acquire)) usleep(1000);
        if (start_ready(B, a, clock, chain, o) != 0) {
            b_failed.store(1);
            return;
        }
        plans[1].generation = plugin::assign_slot(slots[1]);  // before the plan is published
        B.mark_new_channel();
        b_ready.store(true, std::memory_order_release);
        while (!retired.load(std::memory_order_acquire)) usleep(1000);
        a_alive_at_retire = A.alive();
        A.stop();  // shut down, region released: never reused
        // B was forked from this thread, and Linux sends a host's death
        // signal when the THREAD that forked it exits (ipc.h,
        // die_with_parent): like the engine's control thread, this one
        // outlives its hosts.
        while (!finished.load(std::memory_order_acquire)) usleep(1000);
    });

    // The late change goes to the old host after the new one was seeded
    // (b_ready), and the swap comes 20 blocks after that.
    const uint64_t total = 800, set_a = 100, start_at = 150, set_late = 250;
    const float db_a = -6.f, db_late = 6.f;
    std::vector<double> in_e(total, 0), out_e(total, -1);
    int active = 0;
    int64_t swap_at = -1, late_at = -1;
    uint64_t faults = 0;
    Signal sig(1);
    const LoopStats st = run_periodic(clock, total, [&](uint64_t i) {
        // A plan swap is picked up at a callback's start (ADR-0009 §4.1).
        if (active == 0 && late_at >= 0 && i >= uint64_t(late_at) + 20) {
            active = 1;
            swap_at = int64_t(i);
        } else if (active == 1 && !retired.load(std::memory_order_relaxed)) {
            retired.store(true, std::memory_order_release);  // the old plan ran its last callback
        }
        const Node& node = plans[active];
        if (i == set_a) node.host->send(0, uint32_t(gp), 0, db_a);
        if (i == start_at) start_b.store(true, std::memory_order_release);
        if (late_at < 0 && i >= set_late && b_ready.load(std::memory_order_acquire)) {
            node.host->send(0, uint32_t(gp), 0, db_late);
            late_at = int64_t(i);
        }
        sig.sine(i, 997.0, 0.1f);
        for (uint32_t s = 0; s < kBlock; ++s) in_e[i] += double(sig.buf[0][s]) * sig.buf[0][s];
        // Step 0 of the block protocol: the slot must still be the one the
        // plan was built against.
        if (!plugin::slot_current(slots[node.slot], node.generation)) {
            ++faults;
            return true;
        }
        const int64_t from = node.host->run(sig.in.data(), sig.out.data(), kBlock, i);
        if (from >= 0) {
            double e = 0;
            for (uint32_t s = 0; s < kBlock; ++s) e += double(sig.obuf[0][s]) * sig.obuf[0][s];
            out_e[size_t(from)] = e;
        }
        return true;
    });
    if (!retired.load()) retired.store(true);
    finished.store(true, std::memory_order_release);
    control.join();
    report_loop(st);
    check(b_failed.load() == 0 && swap_at > 0, "the new host came up and the plan swapped to it");
    if (swap_at <= 0) return 0;
    std::printf("  swapped at callback %lld; old host %s at its retirement\n", (long long)swap_at,
                a_alive_at_retire ? "still running" : "already gone");
    check(a_alive_at_retire, "the old host played until the old plan was retired (make-before-break)");
    check(A.h == nullptr, "the old region was released after the retirement");
    check(faults == 0, "no plan ran against a slot of another generation");
    const auto window_db = [&](uint64_t from, uint64_t to) {
        double ei = 0, eo = 0;
        for (uint64_t b = from; b < to; ++b)
            if (out_e[b] >= 0) ei += in_e[b], eo += out_e[b];
        return ei > 0 && eo > 0 ? 10.0 * std::log10(eo / ei) : -999.0;
    };
    uint64_t silent = 0;
    for (uint64_t b = uint64_t(swap_at) - 1; b < uint64_t(swap_at) + 4; ++b) silent += out_e[b] < 0 ? 1 : 0;
    std::printf("  around the swap, %llu block(s) not played\n", (unsigned long long)silent);
    // The old host's in-flight block (submitted at swap - 1) is discarded,
    // and the new channel has nothing to collect in its first callback.
    check(silent <= 2, "the swap costs at most the discarded in-flight block and one silent block");
    char what[128];
    const double before = window_db(uint64_t(late_at) + 3, uint64_t(swap_at) - 2);
    const double after = window_db(uint64_t(swap_at) + 3, total);
    std::snprintf(what, sizeof what, "the old host played %+.0f dB to the end: %+.4f dB", db_late, before);
    check(std::fabs(before - db_late) < 0.01, what);
    std::snprintf(what, sizeof what,
                  "the new host plays %+.0f dB from its first block (seeded with %+.0f, store copied on "
                  "first submit): %+.4f dB",
                  db_late, db_a, after);
    check(std::fabs(after - db_late) < 0.01, what);
    // H1: once the slot is given to another host, a plan built against the
    // old generation matches nothing.
    const uint32_t old_generation = plans[0].generation;
    plugin::assign_slot(slots[0]);
    check(!plugin::slot_current(slots[0], old_generation), "a reassigned slot rejects the retired plan's node");
    check(st.over_period <= kOsNoiseCallbacks, "no callback overran the period during the swap");
    B.alive();  // reaps it if it is gone, so death() says how
    if (B.h)
        std::printf("  new host: state %s, completed %llu of %llu submitted, %s\n", state_name(B.h->state.load()),
                    (unsigned long long)B.h->completed_block.load(),
                    (unsigned long long)B.h->submitted_block.load(), B.death().c_str());
    return 0;
}

// Parameters by dense index, tagged with a block and a frame offset
// (ADR-0012 §5.1), through the params test plugin, which writes how many
// changes it saw into sample 0, their sum into sample 1, and the offset of
// parameter 0's change into sample 2:
//  - as many distinct changes in one block as the ring holds all reach the
//    plugin in that block, and one for a parameter that does not exist is
//    counted, not lost silently;
//  - a change at offset 37 reaches the plugin at frame 37;
//  - an entry tagged with an older block is applied at offset 0 and counted
//    late;
//  - a value carried by the table alone reaches the plugin at offset 0.
int scenario_params(const Args& a) {
    const std::vector<std::string> chain = {kVst3Params};
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    std::printf("params: %s\n", join(chain, ',').c_str());
    Host host;
    if (int r = start_ready(host, a, clock, chain, 1)) return r;
    // One slot of the ring is left for the change the host must reject.
    const uint32_t distinct = plugin::kParamRingCapacity - 1;
    const float value = 0.25f;
    const uint64_t total = 400, at = 100, at_offset = 150, at_late = 200, at_table = 250;
    // Each change goes out in the first callback from its time on that will
    // submit: one that does not is held and arrives through the table at
    // offset 0 (block protocol step 2), which is not what these check.
    uint64_t sent_at[4] = {0, 0, 0, 0};
    const uint64_t when[4] = {at, at_offset, at_late, at_table};
    std::vector<std::array<float, 3>> seen(total, {-1.f, -1.f, -1.f});
    Signal sig(1);
    const LoopStats st = run_periodic(clock, total, [&](uint64_t i) {
        for (int k = 0; k < 4; ++k) {
            if (sent_at[k] || i < when[k] || !host.idle()) continue;
            sent_at[k] = i;
            if (k == 0) {
                for (uint32_t d = 0; d < distinct; ++d) host.send(0, d, 0, value);
                host.send(0, 9999, 0, value);  // no such parameter
            }
            if (k == 1) host.send(0, 0, 37, 0.5f);
            if (k == 2) host.push_raw({host.next_block() - 5, 0, 0, 90, 0.75f});
            if (k == 3) host.send_table_only(0, 0, 0.125f);
        }
        sig.sine(i, 440.0, 0.1f);
        const int64_t from = host.run(sig.in.data(), sig.out.data(), kBlock, i);
        if (from >= 0) seen[size_t(from)] = {sig.obuf[0][0], sig.obuf[0][1], sig.obuf[0][2]};
        return true;
    });
    report_loop(st);
    // The first block from callback `from` on that carried a change.
    auto first_change = [&](uint64_t from) -> const std::array<float, 3>* {
        for (uint64_t b = from; b < from + 20 && b < total; ++b)
            if (seen[b][0] > 0.f) return &seen[b];
        return nullptr;
    };
    const auto* bulk = first_change(sent_at[0]);
    std::printf("  pushed %u changes in callback %llu; the plugin saw %.0f (sum %.2f); host rejected %llu, "
                "late %llu\n",
                distinct + 1, (unsigned long long)sent_at[0], bulk ? double((*bulk)[0]) : -1.0, bulk ? double((*bulk)[1]) : -1.0,
                (unsigned long long)host.h->params_rejected.load(), (unsigned long long)host.h->params_late.load());
    check(sent_at[0] && host.refused() == 0, "the ring took every change");
    check(bulk && (*bulk)[0] == float(distinct), "every distinct change reached the plugin in one block");
    check(bulk && std::fabs((*bulk)[1] - float(distinct) * value) < 1e-3f, "with the value pushed");
    check(host.h->params_rejected.load() == 1, "the change for a parameter that does not exist was counted as rejected");
    const auto* offset = first_change(sent_at[1]);
    check(offset && (*offset)[0] == 1.f && (*offset)[1] == 0.5f && (*offset)[2] == 37.f,
          "a change tagged with frame 37 reached the plugin at frame 37");
    const auto* late = first_change(sent_at[2]);
    check(late && (*late)[1] == 0.75f && (*late)[2] == 0.f && host.h->params_late.load() == 1,
          "an entry for an older block was applied at frame 0 and counted late");
    const auto* table = first_change(sent_at[3]);
    check(table && (*table)[0] == 1.f && (*table)[1] == 0.125f && (*table)[2] == 0.f,
          "a value carried by the table alone reached the plugin at frame 0");
    return 0;
}
// --list --json is one JSON object per line, whatever a scan does: a bundle
// whose scan hangs is killed and reported on a line of its own, and every
// record written before that is whole.
namespace json {
bool value(const std::string& s, std::size_t& i, int depth);
void ws(const std::string& s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
}
bool string(const std::string& s, std::size_t& i) {
    if (i >= s.size() || s[i] != '"') return false;
    for (++i; i < s.size(); ++i) {
        if (s[i] == '\\') {
            ++i;
            continue;
        }
        if (s[i] == '"') {
            ++i;
            return true;
        }
        if (static_cast<unsigned char>(s[i]) < 0x20) return false;
    }
    return false;
}
bool container(const std::string& s, std::size_t& i, int depth, char open, char close, bool keyed) {
    if (i >= s.size() || s[i] != open || depth > 32) return false;
    ++i;
    ws(s, i);
    if (i < s.size() && s[i] == close) return ++i, true;
    for (;;) {
        ws(s, i);
        if (keyed) {
            if (!string(s, i)) return false;
            ws(s, i);
            if (i >= s.size() || s[i++] != ':') return false;
        }
        if (!value(s, i, depth + 1)) return false;
        ws(s, i);
        if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
        }
        return i < s.size() && s[i++] == close;
    }
}
bool value(const std::string& s, std::size_t& i, int depth) {
    ws(s, i);
    if (i >= s.size()) return false;
    const char c = s[i];
    if (c == '{') return container(s, i, depth, '{', '}', true);
    if (c == '[') return container(s, i, depth, '[', ']', false);
    if (c == '"') return string(s, i);
    for (const char* lit : {"true", "false", "null"})
        if (s.compare(i, std::strlen(lit), lit) == 0) return i += std::strlen(lit), true;
    const std::size_t from = i;
    while (i < s.size() && std::strchr("+-0123456789.eE", s[i])) ++i;
    return i > from;
}
bool object_line(const std::string& line) {
    std::size_t i = 0;
    if (line.empty() || line[0] != '{' || !value(line, i, 0)) return false;
    ws(line, i);
    return i == line.size();
}
}  // namespace json

int scenario_list_json(const Args& a) {
    // Under SONORA_TEST_VST3_HANG_ON_SCAN=1 the in-tree bundle hangs on its
    // last class, after a params class whose record is longer than a pipe
    // buffer.
    std::string cmd = "'" + a.host + "' --list --json --format vst3 --scan-timeout 3";
    for (const std::string& b : a.bundles)
        if (b.size() > 5 && b.compare(b.size() - 5, 5, ".vst3") == 0) cmd += " --bundle '" + b + "'";
    std::printf("list_json: %s\n", cmd.c_str());
    std::FILE* p = popen(cmd.c_str(), "r");
    if (!p) return 1;
    std::string all;
    char buf[65536];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, p)) > 0;) all.append(buf, n);
    const int status = pclose(p);
    std::size_t lines = 0, bad = 0;
    bool hung_entry = false, params_entry = false;
    std::size_t start = 0;
    while (start < all.size()) {
        std::size_t end = all.find('\n', start);
        if (end == std::string::npos) end = all.size();
        const std::string line = all.substr(start, end - start);
        start = end + 1;
        ++lines;
        if (!json::object_line(line)) {
            ++bad;
            std::printf("  not a JSON object: %.200s%s\n", line.c_str(), line.size() > 200 ? "..." : "");
            continue;
        }
        if (line.find("\"bundle\":") != std::string::npos && line.find("sonora-test.vst3") != std::string::npos &&
            line.find("the scan hung") != std::string::npos)
            hung_entry = true;
        if (line.find(std::string("\"uri\":\"") + kVst3Params + "\"") != std::string::npos) params_entry = true;
    }
    std::printf("  %zu line(s), %zu malformed, %zu bytes; exit status %d\n", lines, bad, all.size(), status);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "--list exited 0");
    check(lines > 0 && bad == 0, "every line of --list --json is one whole JSON object");
    check(hung_entry, "the hanging bundle has an entry saying the scan hung");
    check(params_entry, "the classes described before the hang are listed");
    return 0;
}
// Planar float32 (every sample of channel 0, then channel 1, ...), the
// layout sonora-dsp-measure --host-fixture writes.
bool read_planar(const std::string& path, uint32_t channels, std::vector<std::vector<float>>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<float> all;
    float buf[4096];
    for (std::size_t n; (n = std::fread(buf, sizeof(float), 4096, f)) > 0;) all.insert(all.end(), buf, buf + n);
    std::fclose(f);
    if (channels == 0 || all.size() % channels != 0) return false;
    const std::size_t frames = all.size() / channels;
    out.assign(channels, {});
    for (uint32_t c = 0; c < channels; ++c)
        out[c].assign(all.begin() + std::ptrdiff_t(c * frames), all.begin() + std::ptrdiff_t((c + 1) * frames));
    return true;
}

// A parameter move for the render: in block `block`, chain instance 0's
// dense parameter `dense` to `value` (format-native).
struct Move {
    uint64_t block;
    uint32_t dense;
    float value;
};

// Each --change BLOCK:SYMBOL=VALUE by dense index, from the host's own
// --probe: SYMBOL is what --set takes (LV2 port symbol, VST3 title), as the
// engine maps a name from the descriptor to the dense index of its scan.
bool resolve_changes(const Args& a, std::vector<Move>& moves) {
    if (a.changes.empty()) return true;
    const PluginParams d = probe_params(a, a.chain[0]);
    for (const std::string& c : a.changes) {
        const std::size_t colon = c.find(':'), eq = c.rfind('=');
        if (colon == std::string::npos || eq == std::string::npos || eq < colon) return false;
        const std::string symbol = c.substr(colon + 1, eq - colon - 1);
        const int dense = d.index_of_symbol(symbol);
        if (dense < 0) {
            std::printf("  --change %s: the host's --probe lists no parameter '%s'\n", c.c_str(), symbol.c_str());
            return false;
        }
        moves.push_back({std::strtoull(c.c_str(), nullptr, 10), uint32_t(dense),
                         std::strtof(c.c_str() + eq + 1, nullptr)});
    }
    return true;
}

// Every plane of `planes` (the region's channels: signal, then key) through
// a started host, block by block, each submitted and waited for; `got` gets
// the region's output planes. Every block is block_frames (contracts/plugin,
// Frame count): the last, short one is padded with silence and only its
// head kept. `moves` are sent in the callback that submits their block, so
// the host applies them at its start. False when a block did not come
// back. Not periodic, and it waits: only a harness does this, never the
// engine (Host::round_trip says why).
bool render_blocks(Host& host, const std::vector<std::vector<float>>& planes, const std::vector<Move>& moves,
                   std::vector<std::vector<float>>& got) {
    const std::size_t frames = planes[0].size();
    const std::size_t region_channels = planes.size();
    got.assign(region_channels, std::vector<float>(frames, 0.f));
    std::vector<std::vector<float>> pad_in(region_channels, std::vector<float>(kBlock)),
        pad_out(region_channels, std::vector<float>(kBlock));
    std::vector<const float*> ip(region_channels);
    std::vector<float*> op(region_channels);
    uint64_t block = 0;
    for (std::size_t pos = 0; pos < frames; pos += kBlock, ++block) {
        const uint32_t n = uint32_t(std::min<std::size_t>(kBlock, frames - pos));
        for (std::size_t c = 0; c < region_channels; ++c) {
            if (n == kBlock) {
                ip[c] = planes[c].data() + pos;
                op[c] = got[c].data() + pos;
            } else {
                std::fill(pad_in[c].begin(), pad_in[c].end(), 0.f);
                std::copy(planes[c].begin() + std::ptrdiff_t(pos), planes[c].end(), pad_in[c].begin());
                ip[c] = pad_in[c].data();
                op[c] = pad_out[c].data();
            }
        }
        for (const Move& m : moves)
            if (m.block == block) host.send(0, m.dense, 0, m.value);
        uint64_t rtt = 0;
        if (!host.round_trip(ip.data(), op.data(), kBlock, block, monotonic_ns() + 5'000'000'000ull, rtt))
            return false;
        if (n != kBlock)
            for (std::size_t c = 0; c < region_channels; ++c)
                std::copy(pad_out[c].begin(), pad_out[c].begin() + n, got[c].begin() + std::ptrdiff_t(pos));
    }
    return true;
}

// The key channels, through the test plugin that outputs its key and then
// writes over it (tests/lv2/sonora_test.cpp):
// - two of them in a chain with a key routed: the second still reads the
//   key, not what the first left in it, so the chain's output is the key;
// - one with no key routed: its key input reads its own input, so the
//   output is the input (a key input left on silence would output zeros).
int scenario_key(const Args& a) {
    std::printf("key\n");
    const std::size_t frames = 64 * kBlock;
    std::vector<float> signal(frames), key(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        signal[i] = 0.5f * std::sin(0.013f * float(i));
        key[i] = (i % 97) / 97.f - 0.5f;
    }
    struct Case {
        const char* what;
        std::vector<std::string> chain;
        bool keyed;
    };
    const Case cases[] = {
        {"two plugins that write into their key input: each reads the key", {kKeyPlugin, kKeyPlugin}, true},
        {"no key routed: the key input reads the plugin's own input", {kKeyPlugin}, false},
    };
    Clock clock;
    if (!clock.create(wake_of(a, plugin::WakeMode::Futex))) return 1;
    for (const Case& c : cases) {
        Args ca = a;
        ca.key_channels = c.keyed ? 1 : 0;
        Host host;
        std::vector<std::vector<float>> planes = {signal};
        if (c.keyed) planes.push_back(key);
        if (int r = start_ready(host, ca, clock, c.chain, uint32_t(planes.size()))) return r;
        std::vector<std::vector<float>> got;
        const bool complete = render_blocks(host, planes, {}, got);
        check(complete, "every block came back from the host");
        if (!complete) {
            std::printf("  host: %s\n", host.death().c_str());
            continue;
        }
        const std::vector<float>& want = c.keyed ? key : signal;
        std::size_t differ = 0;
        for (std::size_t i = 0; i < frames; ++i) differ += got[0][i] != want[i];
        std::printf("  [%s] %zu of %zu samples differ from the %s\n", join(c.chain, ',').c_str(), differ, frames,
                    c.keyed ? "key" : "input");
        check(differ == 0, c.what);
    }
    return 0;
}

// --args-file PATH stands for the arguments in PATH, one per line: the
// render fixtures sonora-dsp-measure writes. VST3 parameter titles have
// spaces, so a line is one argument whatever it holds.
std::vector<std::string> expand_args(int argc, char** argv) {
    std::vector<std::string> out;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--args-file") != 0 || i + 1 >= argc) {
            out.push_back(argv[i]);
            continue;
        }
        std::FILE* f = std::fopen(argv[++i], "r");
        if (!f) {
            std::fprintf(stderr, "plugin_driver: cannot read %s\n", argv[i]);
            std::exit(2);
        }
        char line[4096];
        while (std::fgets(line, sizeof line, f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            out.push_back(s);
        }
        std::fclose(f);
    }
    return out;
}

// Round trip per block: a measurement of the IPC, not a protocol mode. It
// sleeps on done_word, which the engine's audio thread never does. Futex
// wake by default: it measures the wake path the gate would turn on.
int scenario_rtt(const Args& a) {
    const std::vector<std::string> chain = a.chain.empty() ? std::vector<std::string>{kEgAmp} : a.chain;
    const uint64_t total = a.blocks ? a.blocks : 3000;
    Clock clock;
    // The session rate in both headers: the host checks they agree.
    if (!clock.create(wake_of(a, plugin::WakeMode::Futex), {}, a.rate)) return 1;
    std::printf("rtt: %s, %u channel(s), %llu blocks of %u at %u Hz, %s wake, measurement wait, deadline %.0f us\n",
                join(chain, ',').c_str(), a.channels, (unsigned long long)total, kBlock, kRate,
                clock.mode == plugin::WakeMode::Futex ? "futex" : "timer", kDeadlineNs / 1e3);
    Host host;
    if (int r = start_ready(host, a, clock, chain, a.channels)) return r;
    std::vector<double> rtt_us;
    rtt_us.reserve(total);
    uint64_t dropped = 0;
    Signal sig(a.channels);
    const LoopStats st = run_periodic(clock, total, [&](uint64_t i) {
        sig.sine(i, 997.0, 0.1f);
        uint64_t rtt = 0;
        if (host.round_trip(sig.in.data(), sig.out.data(), kBlock, i, monotonic_ns() + kDeadlineNs, rtt))
            rtt_us.push_back(rtt / 1e3);
        else
            ++dropped;
        return true;
    });
    report_loop(st);
    std::printf("  CONTAINER FIGURES, not appliance figures (shared host kernel, no RT kernel):\n");
    std::printf("  round trip us: p50 %.1f  p99 %.1f  p99.9 %.1f  max %.1f  (n=%zu)\n",
                percentile(rtt_us, 50), percentile(rtt_us, 99), percentile(rtt_us, 99.9),
                rtt_us.empty() ? 0.0 : *std::max_element(rtt_us.begin(), rtt_us.end()), rtt_us.size());
    std::printf("  dropped %llu of %llu (%.2f %%), of which %llu noticed after the deadline; "
                "host worst block (chain only) %.1f us\n",
                (unsigned long long)dropped, (unsigned long long)total, 100.0 * dropped / total,
                (unsigned long long)host.late(), host.h->worst_block_ns.load() / 1e3);
    check(rtt_us.size() * 2 > total, "more than half the blocks came back in time");
    return 0;
}

// A plugin whose exact output is known, rendered through a real host
// process: Sonora's own bundles against the N0 reference renders
// (sonora-dsp-measure --host-fixture, ADR-0011 §1.2's checkpoint). Not
// periodic: each block is submitted and then waited for, so none is dropped
// and the output is the plugin's, sample for sample: the output planes after
// block N are block N's, whatever the pipeline depth says the engine does
// with them. Only a harness waits like this; the engine never does.
int scenario_render(const Args& a) {
    std::printf("render: %s at %u Hz, %u channel(s), %u key channel(s)%s\n", join(a.chain, ',').c_str(),
                a.rate, a.channels, a.key_channels, a.reactivate ? ", re-activated after loading" : "");
    std::vector<std::vector<float>> in, want, key;
    if (a.chain.size() != 1 || !read_planar(a.input, a.channels, in) ||
        !read_planar(a.expected, a.channels, want) || in[0].size() != want[0].size() ||
        (a.key_channels > 0 && (!read_planar(a.key, a.key_channels, key) || key[0].size() != in[0].size()))) {
        check(false, "one plugin, and --input, --expected and --key of the same length and channel count");
        return 0;
    }
    std::vector<Move> moves;
    if (!resolve_changes(a, moves)) {
        check(false, "every --change names a parameter of the plugin");
        return 0;
    }
    Clock clock;
    // The session rate in both headers: the host checks they agree.
    if (!clock.create(wake_of(a, plugin::WakeMode::Futex), {}, a.rate)) return 1;
    Host host;
    // The key rides in the region after the signal channels.
    if (int r = start_ready(host, a, clock, a.chain, a.channels + a.key_channels)) return r;
    const uint32_t latency = plugin::checked_latency(host.h->instances[0].latency_frames.load());
    if (a.expect_latency >= 0) {
        char what[128];
        std::snprintf(what, sizeof what, "the plugin reports %u frames of latency, the reference %lld", latency,
                      (long long)a.expect_latency);
        check(int64_t(latency) == a.expect_latency, what);
    }

    const std::size_t frames = in[0].size();
    std::vector<std::vector<float>> planes = in;
    planes.insert(planes.end(), key.begin(), key.end());
    std::vector<std::vector<float>> got;
    const bool complete = render_blocks(host, planes, moves, got);
    check(complete, "every block came back from the host");
    if (!moves.empty()) check(host.refused() == 0, "every --change went on the parameter ring");
    if (!complete) {
        std::printf("  host: %s\n", host.death().c_str());
        return 0;
    }

    double worst = 0.0;
    std::size_t worst_at = 0;
    uint32_t worst_ch = 0;
    for (uint32_t c = 0; c < a.channels; ++c)
        for (std::size_t i = 0; i < frames; ++i) {
            const double d = std::fabs(double(got[c][i]) - double(want[c][i]));
            if (d > worst) worst = d, worst_at = i, worst_ch = c;
        }
    std::printf("  %zu frames: max |plugin - reference| = %.3g (channel %u, frame %zu: %.9g against %.9g)\n",
                frames, worst, worst_ch, worst_at, double(got[worst_ch][worst_at]),
                double(want[worst_ch][worst_at]));
    char what[128];
    std::snprintf(what, sizeof what, "output equals the N0 reference render within %.3g", a.tolerance);
    check(worst <= a.tolerance, what);
    check(host.h->instances[0].nonfinite_samples.load() == 0, "no NaN or Inf from the plugin");
    check(host.h->params_rejected.load() == 0, "no parameter change refused");
    return 0;
}

// ---------------------------------------------------------------------------
// HostControl scenarios (contracts/plugin host_control.h). The driver plays
// the engine's control thread: non-blocking sends and receives through a
// Link, polled, with a bounded wait only where it expects a reply.

// A run of periodic callbacks, depth 1, the audio thread's way. Returns how
// many blocks came back.
uint64_t play(Host& host, Clock& clock, uint32_t channels, uint64_t callbacks, uint64_t first = 0) {
    Signal sig(channels);
    uint64_t collected = 0;
    run_periodic(clock, callbacks, [&](uint64_t i) {
        sig.sine(first + i, 440.0, 0.25f);
        if (host.run(sig.in.data(), sig.out.data(), kBlock, first + i) >= 0) ++collected;
        return true;
    });
    return collected;
}

bool request_state(Host& host, uint32_t request, uint32_t instance, pc::State& st) {
    host.queue(pc::GetState{request, instance});
    if (!host.expect_message(pc::Type::State, st, pc::kReplyTimeoutMs)) return false;
    return st.request == request && st.instance == instance;
}

// Handshake, Loaded's key table, DescribeValue, editors, GetState on a
// plugin with no state of its own and on an instance that does not exist,
// and Shutdown: the host leaves Stopped and exits 0.
int scenario_control(const Args& a) {
    Args ca = a;
    ca.control = true;
    const std::vector<std::string> chain = a.chain.empty() ? std::vector<std::string>{kEgAmp} : a.chain;
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    std::printf("control: %s over HostControl\n", join(chain, ',').c_str());
    Host host;
    if (int r = start_ready(host, ca, clock, chain, a.channels)) return r;

    const PluginParams d = probe_params(a, chain[0]);
    const pc::Loaded& loaded = host.loaded();
    bool keys = loaded.instances[0].params.size() == d.keys.size();
    for (std::size_t k = 0; keys && k < d.keys.size(); ++k)
        keys = loaded.instances[0].params[k].key == d.keys[k] &&
               loaded.instances[0].params[k].default_value == d.defaults[k] &&
               loaded.instances[0].params[k].min <= loaded.instances[0].params[k].max;
    check(keys, "Loaded lists the running instance's keys in dense order, as the scan did");
    check(loaded.instances[0].replicas == host.h->instances[0].replicas.load(),
          "Loaded's replicas are the region's");

    const uint64_t got = play(host, clock, a.channels, 200);
    std::printf("  %llu of 200 blocks came back while the socket was open\n", (unsigned long long)got);
    check(got > 150, "the chain runs while HostControl is served");

    if (!d.keys.empty()) {
        host.queue(pc::DescribeValue{1, 0, 0, d.defaults[0]});
        pc::ValueText vt;
        check(host.expect_message(pc::Type::ValueText, vt, pc::kReplyTimeoutMs) && vt.request == 1 && vt.ok &&
                  !vt.text.empty(),
              "DescribeValue answers with display text");
        std::printf("  DescribeValue(%s = %g) -> \"%s\"\n", d.keys[0].c_str(), double(d.defaults[0]), vt.text.c_str());
    }
    host.queue(pc::DescribeValue{2, 0, uint32_t(d.keys.size()), 0.f});
    pc::ValueText bad;
    check(host.expect_message(pc::Type::ValueText, bad, pc::kReplyTimeoutMs) && bad.request == 2 && !bad.ok,
          "DescribeValue of a parameter that does not exist says so");

    host.queue(pc::OpenEditor{3, 0, ""});
    pc::EditorFailed ef;
    check(host.expect_message(pc::Type::EditorFailed, ef, pc::kReplyTimeoutMs) && ef.request == 3 && ef.instance == 0,
          "OpenEditor is answered (EditorFailed: this build hosts no editor)");
    std::printf("  EditorFailed: %s\n", ef.text.c_str());
    host.queue(pc::CloseEditor{0});
    std::string any;
    check(!host.expect(pc::Type::EditorClosed, any, 300), "CloseEditor of an editor that is not open is ignored");

    pc::State st;
    if (chain[0].rfind("vst3:", 0) == 0) {
        // A VST3 component always has a state (getState).
        pc::StateBlob blob;
        std::string e;
        check(request_state(host, 4, 0, st) && st.status == pc::StateStatus::Ok &&
                  pc::decode_state_blob(st.bytes, blob, e) && blob.format == pc::Format::Vst3 &&
                  "vst3:" + blob.id == chain[0] && blob.sections.size() == 2,
              "GetState: a StateBlob naming this class, component and controller sections");
    } else {
        check(request_state(host, 4, 0, st) && st.status == pc::StateStatus::Empty && st.bytes.empty(),
              "GetState of a plugin with no state of its own: Empty");
    }
    check(request_state(host, 5, 7, st) && st.status == pc::StateStatus::Failed && !st.text.empty(),
          "GetState of an instance that does not exist: Failed, with the reason");

    host.queue(pc::Shutdown{});
    const bool exited = host.wait_exit_pumping(5'000'000'000ull);
    check(exited && WIFEXITED(host.status()) && WEXITSTATUS(host.status()) == plugin::kHostExitClean,
          "Shutdown: the host exits 0");
    check(plugin::checked_state(host.h->state.load()) == plugin::HostState::Stopped, "and leaves the region Stopped");
    std::vector<std::string> rest = host.take_inbox();
    check(rest.empty(), "and sends nothing unasked on the way out");
    return 0;
}

// Versions and protocol errors (host_control.h, Handshake): a wire, layout
// or clock version the host does not speak exits 3 having sent nothing and
// written nothing; a first message that is not EngineHello exits 2 having
// sent nothing; after the handshake, a broken contract is a Fault{Protocol}
// and exit 2.
int scenario_control_incompatible(const Args& a) {
    Args ca = a;
    ca.control = true;
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    std::printf("control_incompatible\n");
    struct Case {
        const char* what;
        int exit_code;
        Host::StartOptions o;
    };
    std::vector<Case> silent;
    {
        Case c{"an EngineHello of wire 99: exit 3", plugin::kHostExitIncompatible, {}};
        c.o.hello = [](pc::EngineHello& h) { h.wire_version = 99; };
        silent.push_back(c);
        Case l{"an EngineHello naming layout 7: exit 3", plugin::kHostExitIncompatible, {}};
        l.o.hello = [](pc::EngineHello& h) { h.layout_version = 7; };
        silent.push_back(l);
        Case f{"a first message that is not EngineHello: exit 2", plugin::kHostExitUsage, {}};
        pc::encode(pc::Shutdown{}, f.o.first_message);
        silent.push_back(f);
    }
    for (Case& c : silent) {
        std::printf(" %s\n", c.what);
        Host host;
        if (!host.start(ca, clock, {kEgAmp}, c.o)) return 1;
        const bool exited = host.wait_exit_pumping(15'000'000'000ull);
        check(exited && WIFEXITED(host.status()) && WEXITSTATUS(host.status()) == c.exit_code, c.what);
        check(host.take_inbox().empty(), "  nothing was sent on the socket");
        check(host.h->state.load() == 0 && host.h->heartbeat.load() == 0 && host.h->latency_generation.load() == 0,
              "  nothing was written to the region");
    }

    // After the handshake. Each forged Load is refused with Fault{Protocol}.
    struct Bad {
        const char* what;
        std::function<void(pc::Load&)> load;
    };
    const Bad bad[] = {
        {"a Load naming more instances than the header", [](pc::Load& l) { l.instances.push_back(l.instances[0]); }},
        {"a Load whose dense count is not the header's", [](pc::Load& l) { l.instances[0].params += 1; }},
        {"a Load asking for every channel as key", [](pc::Load& l) { l.key_channels = 1; }},
    };
    for (const Bad& b : bad) {
        std::printf(" %s\n", b.what);
        Host::StartOptions o;
        o.load = b.load;
        Host host;
        if (!host.start(ca, clock, {kEgAmp}, o)) return 1;
        pc::Fault f;
        check(host.expect_message(pc::Type::Fault, f, 15000) && f.kind == pc::FaultKind::Protocol, "  Fault{Protocol}");
        std::printf("  Fault: %s\n", f.text.c_str());
        const bool exited = host.wait_exit_pumping(5'000'000'000ull);
        check(exited && WIFEXITED(host.status()) && WEXITSTATUS(host.status()) == plugin::kHostExitUsage,
              "  then exit 2");
    }
    {
        std::printf(" a second Load after Loaded\n");
        Host host;
        if (int r = start_ready(host, ca, clock, {kEgAmp}, 1)) return r;
        pc::Load again;
        again.instances.push_back({pc::Format::Lv2, kEgAmp, host.h->instance_params[0], ""});
        host.queue(again);
        pc::Fault f;
        check(host.expect_message(pc::Type::Fault, f, 5000) && f.kind == pc::FaultKind::Protocol,
              "  Fault{Protocol}");
        const bool exited = host.wait_exit_pumping(5'000'000'000ull);
        check(exited && WIFEXITED(host.status()) && WEXITSTATUS(host.status()) == plugin::kHostExitUsage,
              "  then exit 2");
    }
    {
        std::printf(" a frame out of place after Loaded\n");
        Host host;
        if (int r = start_ready(host, ca, clock, {kEgAmp}, 1)) return r;
        std::string frame(16, '\0');
        const uint32_t total = 8, offset = 4;  // a message that does not start at 0
        std::memcpy(&frame[0], &total, 4);
        std::memcpy(&frame[4], &offset, 4);
        // Straight onto the socket: a Link would never frame like this.
        host.send_raw_frame(frame);
        const bool gone = host.wait_exit_pumping(5'000'000'000ull);
        check(gone && WIFEXITED(host.status()) && WEXITSTATUS(host.status()) == plugin::kHostExitUsage,
              "  the host stops with exit 2: a peer that frames wrongly cannot be resynchronised");
    }
    return 0;
}

// The test plugins' block counter, from a StateBlob: VST3 "SNTS" + int64 in
// the component section; LV2 the integer after "blocks" in the Turtle.
bool state_counter(const std::string& bytes, int64_t& n) {
    pc::StateBlob b;
    std::string e;
    if (!pc::decode_state_blob(bytes, b, e) || b.sections.empty()) return false;
    const std::string& s = b.sections[0];
    if (b.format == pc::Format::Vst3) {
        if (s.size() != 12 || s.compare(0, 4, "SNTS") != 0) return false;
        std::memcpy(&n, s.data() + 4, 8);
        return true;
    }
    const std::size_t at = s.find("blocks");
    if (at == std::string::npos) return false;
    const std::size_t d = s.find_first_of("0123456789", at + 6);
    if (d == std::string::npos) return false;
    n = std::strtoll(s.c_str() + d, nullptr, 10);
    return true;
}

// State save and restore (host_control.h, State): GetState from a running
// host, the bytes written to "Plugin State/<sha256>.state" and read back
// through the hash check, then a NEW host loaded with them (make-before-
// break) with the same parameter store. --expect-state counter: the test
// plugins, whose restored instance counts on from the saved one; same: the
// state survives the replacement byte for byte (Sonora's own bundles).
// Then the refusals: another plugin's state, and bytes the plugin rejects.
int scenario_state(const Args& a) {
    Args ca = a;
    ca.control = true;
    if (a.chain.size() != 1) {
        std::printf("state: needs one plugin in --chain\n");
        return 1;
    }
    const std::vector<std::string>& chain = a.chain;
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    std::printf("state: %s, expect %s\n", chain[0].c_str(), a.expect_state.c_str());
    Store store;
    Host::StartOptions oa;
    oa.channels = a.channels;
    Host first;
    if (int r = start_ready(first, ca, clock, chain, oa)) return r;
    // Move a parameter away from its default first, as an operator would:
    // the store is the engine's, and the next host is seeded from it.
    const PluginParams d = probe_params(a, chain[0]);
    play(first, clock, a.channels, 20);
    if (!d.keys.empty()) {
        const pc::ParamDesc& p = first.loaded().instances[0].params[0];
        first.send(0, 0, 0, p.min + 0.3f * (p.max - p.min));
    }
    const uint64_t played = play(first, clock, a.channels, 300, 20);
    pc::State saved;
    check(request_state(first, 1, 0, saved), "GetState is answered");
    if (saved.status == pc::StateStatus::Empty) {
        std::printf("  the plugin keeps no state of its own (no LV2 state:interface): Empty\n");
        check(a.expect_state == "same", "Empty only where the parameters are all the state there is");
        return 0;
    }
    check(saved.status == pc::StateStatus::Ok && !saved.bytes.empty(), "a StateBlob came back");
    if (saved.status != pc::StateStatus::Ok) {
        std::printf("  State failed: %s\n", saved.text.c_str());
        return 0;
    }
    int64_t counted = -1;
    if (a.expect_state == "counter") {
        check(state_counter(saved.bytes, counted) && counted >= int64_t(played),
              "the saved state holds the blocks the plugin ran");
        std::printf("  saved after %llu blocks collected: the plugin counted %lld\n", (unsigned long long)played,
                    (long long)counted);
    }

    // The file, as the saver thread writes it and the session names it.
    const std::string dir = a.state_dir.empty() ? "/tmp/sonora-plugin-state-" + std::to_string(getpid()) : a.state_dir;
    ::mkdir(dir.c_str(), 0700);
    ::mkdir((dir + "/Plugin State").c_str(), 0700);
    const std::string sha = sonora::sha256_hex(saved.bytes);
    const std::string name = pc::state_file_name(sha);
    const std::string path = dir + "/" + name;
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(saved.bytes.data(), 1, saved.bytes.size(), f);
        std::fclose(f);
    }
    std::string reread;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[65536];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) reread.append(buf, n);
        std::fclose(f);
    }
    std::printf("  %s: %zu bytes\n", name.c_str(), reread.size());
    check(!name.empty() && pc::state_matches(reread, sha), "the file reads back and hashes to its name");
    first.stop();

    // Make-before-break: a new host, the same store, the file's bytes.
    Host::StartOptions ob;
    ob.channels = a.channels;
    ob.states = {reread};
    Host second;
    if (int r = start_ready(second, ca, clock, chain, ob)) return r;
    check(second.loaded().instances[0].state == pc::StateResult::Restored, "Loaded: the state was Restored");
    play(second, clock, a.channels, 50);
    pc::State again;
    check(request_state(second, 2, 0, again) && again.status == pc::StateStatus::Ok, "the new host saves too");
    if (a.expect_state == "counter") {
        int64_t now = -1;
        check(state_counter(again.bytes, now) && now > counted,
              "the restored instance counts on from the saved one, not from zero");
        std::printf("  restored, then counted on to %lld\n", (long long)now);
    } else {
        check(again.bytes == saved.bytes, "the state survives the host replacement byte for byte");
    }
    second.stop();

    // Refused, not fatal: another plugin's state, and bytes the plugin rejects.
    pc::StateBlob blob;
    std::string e;
    pc::decode_state_blob(saved.bytes, blob, e);
    pc::StateBlob other = blob;
    other.id = other.format == pc::Format::Vst3 ? "0123456789ABCDEF0123456789ABCDEF" : "urn:sonora:test#other";
    std::string other_bytes;
    pc::encode_state_blob(other, other_bytes, e);
    Host::StartOptions oc;
    oc.channels = a.channels;
    oc.states = {other_bytes};
    Host third;
    if (int r = start_ready(third, ca, clock, chain, oc)) return r;
    check(third.loaded().instances[0].state == pc::StateResult::Refused,
          "another plugin's state: Refused, and the chain still loads");
    third.stop();
    if (a.expect_state == "counter") {
        pc::StateBlob junk = blob;
        for (std::string& s : junk.sections) s = "not a state";
        std::string junk_bytes;
        pc::encode_state_blob(junk, junk_bytes, e);
        Host::StartOptions od;
        od.channels = a.channels;
        od.states = {junk_bytes};
        Host fourth;
        if (int r = start_ready(fourth, ca, clock, chain, od)) return r;
        check(fourth.loaded().instances[0].state == pc::StateResult::Refused,
              "bytes the plugin rejects: Refused, and the chain still loads");
        check(play(fourth, clock, a.channels, 50) > 30, "and runs");
    }
    return 0;
}

// A state restored through the worker (LV2 worker 1.2): the test plugin's
// restore() schedules the gain its saved instance had applied, from the
// schedule feature restore() is handed, and the new host's first blocks
// carry it although its own gain port is back at 1.
int scenario_worker_restore(const Args& a) {
    Args ca = a;
    ca.control = true;
    const std::vector<std::string> chain = {"urn:sonora:test#worker"};
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    std::printf("worker_restore: %s, saved at gain 0.5, restored through work()\n", chain[0].c_str());
    Host first;
    if (int r = start_ready(first, ca, clock, chain, 1)) return r;
    const Halving before = play_halving(first, clock, 200, [&](uint64_t i) {
        if (i == 20) first.send(0, 0, 0, 0.5f);
    });
    check(before.other == 0 && before.first_half >= 0 && before.unity_after_half == 0,
          "the saved instance applied 0.5 through its worker");
    pc::State saved;
    check(request_state(first, 1, 0, saved) && saved.status == pc::StateStatus::Ok && !saved.bytes.empty(),
          "GetState is answered with a state");
    first.stop();
    if (saved.status != pc::StateStatus::Ok) return 0;

    Host::StartOptions ob;
    ob.channels = 1;
    ob.states = {saved.bytes};
    Host second;
    if (int r = start_ready(second, ca, clock, chain, ob)) return r;
    check(second.loaded().instances[0].state == pc::StateResult::Restored, "Loaded: the state was Restored");
    const Halving h = play_halving(second, clock, 300, [](uint64_t) {});
    h.report();
    check(h.other == 0, "every block is the input times 1 or times 0.5: the plugin never saw a broken worker");
    check(h.first_half >= 0 && h.first_half < 100,
          "what restore() scheduled reached the output within 100 blocks of the start");
    check(h.unity_after_half == 0, "and stayed there");
    second.stop();
    return 0;
}

// Touches from a plugin's own controller (the VST3 touch class's "editor"
// thread): every touch inside a gesture, a lone edit wrapped in one, the
// read-only parameter dropped and counted, the gesture left open ended
// before the host stops, and the host never applying a touch itself.
int scenario_gesture(const Args& a) {
    Args ca = a;
    ca.control = true;
    const std::string touch = "vst3:534F4E4F52415653543354455354000A";
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    std::printf("gesture: %s\n", touch.c_str());
    Host host;
    if (int r = start_ready(host, ca, clock, {touch}, 1)) return r;
    const pc::Loaded& loaded = host.loaded();
    check(loaded.instances[0].params.size() == 1 && loaded.instances[0].params[0].key == "7",
          "Loaded's key table: ParamID 7 only (9 is read-only)");
    host.queue(pc::DescribeValue{1, 0, 0, 0.5f});
    pc::ValueText vt;
    check(host.expect_message(pc::Type::ValueText, vt, pc::kReplyTimeoutMs) && vt.ok && vt.text == "50.0 %",
          "DescribeValue goes through the plugin's controller (\"50.0 %\")");

    // The script starts 300 ms after load; play through it.
    Signal sig(1);
    uint64_t unchanged = 0, collected = 0;
    run_periodic(clock, 300, [&](uint64_t i) {
        sig.sine(i, 440.0, 0.25f);
        const int64_t from = host.run(sig.in.data(), sig.out.data(), kBlock, i);
        if (from >= 0) {
            ++collected;
            Signal ref(1);
            ref.sine(uint64_t(from), 440.0, 0.25f);
            bool same = true;
            for (uint32_t s = 0; s < kBlock; ++s) same = same && sig.obuf[0][s] == ref.buf[0][s];
            unchanged += same;
        }
        host.pump(0);
        return true;
    });
    check(collected > 200 && unchanged == collected,
          "the touched values never reached the plugin's audio: only the engine applies them");
    host.queue(pc::Shutdown{});
    host.wait_exit_pumping(5'000'000'000ull);

    struct Ev {
        pc::Type t;
        uint32_t instance, param;
        float value;
    };
    std::vector<Ev> ev;
    uint64_t dropped = 0;
    for (const std::string& m : host.take_inbox()) {
        pc::Type t;
        std::string e;
        pc::peek_type(m, t);
        if (t == pc::Type::GestureBegin) {
            pc::GestureBegin g;
            if (pc::decode(m, g, e)) ev.push_back({t, g.instance, g.param, 0.f});
        } else if (t == pc::Type::ParamTouched) {
            pc::ParamTouched g;
            if (pc::decode(m, g, e)) ev.push_back({t, g.instance, g.param, g.value});
        } else if (t == pc::Type::GestureEnd) {
            pc::GestureEnd g;
            if (pc::decode(m, g, e)) ev.push_back({t, g.instance, g.param, g.value});
        } else if (t == pc::Type::Fault) {
            pc::Fault f;
            if (pc::decode(m, f, e) && f.kind == pc::FaultKind::EventsDropped) dropped += f.count;
        }
    }
    for (const Ev& e : ev)
        std::printf("    %s %u/%u %g\n",
                    e.t == pc::Type::GestureBegin ? "begin  " : e.t == pc::Type::ParamTouched ? "touched" : "end    ",
                    e.instance, e.param, double(e.value));
    // The first gesture: begin, 0.25, maybe the coalesced 0.5 or 0.75, end 0.75.
    std::size_t i = 0;
    auto is = [&](pc::Type t, float v, bool any_value = false) {
        if (i >= ev.size() || ev[i].t != t || ev[i].instance != 0 || ev[i].param != 0) return false;
        if (!any_value && ev[i].value != v) return false;
        ++i;
        return true;
    };
    bool ok = is(pc::Type::GestureBegin, 0, true) && is(pc::Type::ParamTouched, 0.25f);
    while (ok && i < ev.size() && ev[i].t == pc::Type::ParamTouched) ok = ev[i].value == 0.5f || ev[i++].value == 0.75f;
    ok = ok && is(pc::Type::GestureEnd, 0.75f);
    check(ok, "a gesture: GestureBegin, ParamTouched 0.25, (coalesced), GestureEnd with its last value 0.75");
    ok = is(pc::Type::GestureBegin, 0, true) && is(pc::Type::ParamTouched, 0.5f) && is(pc::Type::GestureEnd, 0.5f);
    check(ok, "a lone edit is wrapped in a gesture of its own");
    ok = is(pc::Type::GestureBegin, 0, true) && is(pc::Type::ParamTouched, 0.125f) &&
         is(pc::Type::GestureEnd, 0.125f) && i == ev.size();
    check(ok, "the gesture left open is ended, with its last value, before the host stops");
    check(dropped == 1, "the touch on the read-only parameter is dropped and counted (Fault{EventsDropped})");
    check(WIFEXITED(host.status()) && WEXITSTATUS(host.status()) == 0, "the host exits 0");
    return 0;
}

// Reconfigure while running (BACKLOG N1): a latency-changing value moved
// through the ring takes effect only at activation; Reconfigure re-activates
// the chain on the host's main thread, and the new latency is published in
// the region first, then in Reconfigured; the host keeps processing after.
int scenario_reconfigure(const Args& a) {
    Args ca = a;
    ca.control = true;
    if (a.chain.size() != 1 || a.change_param.empty() || a.expect_latency < 0) {
        std::printf("reconfigure: needs one plugin, --change-param/--change-value and --expect-latency\n");
        return 1;
    }
    Clock clock;
    if (!clock.create(wake_of(a))) return 1;
    std::printf("reconfigure: %s, %s := %g while running\n", a.chain[0].c_str(), a.change_param.c_str(),
                double(a.change_value));
    Host host;
    Host::StartOptions o;
    o.channels = a.channels;
    if (int r = start_ready(host, ca, clock, a.chain, o)) return r;
    const PluginParams d = probe_params(a, a.chain[0]);
    int cp = d.index_of(a.change_param);
    if (cp < 0) cp = d.index_of_symbol(a.change_param);
    if (cp < 0) {
        std::printf("  no parameter %s\n", a.change_param.c_str());
        return 1;
    }
    const uint32_t before = plugin::checked_latency(host.h->instances[0].latency_frames.load());
    const uint32_t gen0 = host.h->latency_generation.load();
    play(host, clock, a.channels, 50);
    host.send(0, uint32_t(cp), 0, a.change_value);
    play(host, clock, a.channels, 50, 50);
    const uint32_t held = plugin::checked_latency(host.h->instances[0].latency_frames.load());
    host.queue(pc::Reconfigure{9});
    pc::Reconfigured r;
    const bool answered = host.expect_message(pc::Type::Reconfigured, r, pc::kReconfigureTimeoutMs);
    check(answered && r.request == 9, "Reconfigured answers Reconfigure");
    const uint32_t after = plugin::checked_latency(host.h->instances[0].latency_frames.load());
    const uint32_t gen = host.h->latency_generation.load(std::memory_order_acquire);
    std::printf("  plugin latency %u at load, %u with the value held, %u after Reconfigure (expected %lld); "
                "generation %u -> %u, Reconfigured says %u frames gen %u\n",
                before, held, after, (long long)a.expect_latency, gen0, gen, r.latency_frames, r.latency_generation);
    check(int64_t(after) == a.expect_latency && after != before, "the new latency is the plugin's, after re-activation");
    check(r.latency_generation == gen && gen != gen0 &&
              r.latency_frames == plugin::checked_latency(host.h->latency_frames.load()),
          "published in the region first, with a new generation, and Reconfigured agrees");
    const uint64_t got = play(host, clock, a.channels, 100, 100);
    check(got > 70 && host.alive(), "the host processes again after Reconfigure");
    return 0;
}

Args parse(int argc, char** argv) {
    Args a;
    const std::vector<std::string> args = expand_args(argc, argv);
    for (std::size_t i = 0; i + 1 < args.size(); i += 2) {
        const std::string k = args[i], v = args[i + 1];
        if (k == "--host") a.host = v;
        else if (k == "--bundle") a.bundles.push_back(v);
        else if (k == "--format") a.format = v;
        else if (k == "--gain-param") a.gain_param = v;
        else if (k == "--gain-linear") a.gain_linear = v == "1";
        else if (k == "--expect-replicas") a.expect_replicas = uint32_t(std::atoi(v.c_str()));
        else if (k == "--expect-bypass") a.expect_bypass = v;
        else if (k == "--change-param") a.change_param = v;
        else if (k == "--change-value") a.change_value = std::strtof(v.c_str(), nullptr);
        else if (k == "--scenario") a.scenario = v;
        else if (k == "--wake") {
            if (v != "timer" && v != "futex") {
                std::fprintf(stderr, "plugin_driver: --wake is timer or futex\n");
                std::exit(2);
            }
            a.wake = v;
        }
        else if (k == "--chain") a.chain = split(v, ',');
        else if (k == "--channels") a.channels = uint32_t(std::atoi(v.c_str()));
        else if (k == "--blocks") a.blocks = uint64_t(std::atoll(v.c_str()));
        else if (k == "--rate") a.rate = uint32_t(std::atoi(v.c_str()));
        else if (k == "--input") a.input = v;
        else if (k == "--expected") a.expected = v;
        else if (k == "--tolerance") a.tolerance = std::atof(v.c_str());
        else if (k == "--expect-latency") a.expect_latency = std::atoll(v.c_str());
        else if (k == "--set") a.sets.push_back(v);
        else if (k == "--param") {
            const auto colon = v.find(':'), eq = v.find('=');
            if (colon == std::string::npos || eq == std::string::npos || eq < colon) {
                std::fprintf(stderr, "plugin_driver: --param wants INDEX:KEY=VALUE\n");
                std::exit(2);
            }
            a.seeds.push_back({uint32_t(std::atoi(v.substr(0, colon).c_str())), v.substr(colon + 1, eq - colon - 1),
                               std::strtof(v.c_str() + eq + 1, nullptr)});
        }
        else if (k == "--key") a.key = v;
        else if (k == "--key-channels") a.key_channels = uint32_t(std::atoi(v.c_str()));
        else if (k == "--change") a.changes.push_back(v);
        else if (k == "--reactivate") a.reactivate = v == "1";
        else if (k == "--control") a.control = v == "1";
        else if (k == "--expect-state") a.expect_state = v;
        else if (k == "--state-dir") a.state_dir = v;
        else {
            std::fprintf(stderr, "plugin_driver: unknown argument %s\n", k.c_str());
            std::exit(2);
        }
    }
    if (a.host.empty() || a.bundles.empty() || a.scenario.empty()) {
        std::fprintf(stderr, "usage: plugin_driver --host EXE --bundle DIR... --scenario NAME "
                             "[--chain ID,...] [--channels N] [--wake timer|futex] [--blocks N]\n"
                             "       [--format lv2|vst3] [--gain-param KEY] [--gain-linear 1] "
                             "[--expect-replicas N] [--expect-bypass plugin|host]\n"
                             "       render: [--args-file F] --input F --expected F [--rate HZ] "
                             "[--tolerance X] [--expect-latency N] [--set INDEX:SYMBOL=VALUE]...\n"
                             "               [--key F --key-channels N] [--change BLOCK:SYMBOL=VALUE]... "
                             "[--reactivate 1]\n");
        std::exit(2);
    }
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    const Args a = parse(argc, argv);
    // The host death in the crash scenario must not take the driver along.
    ::signal(SIGPIPE, SIG_IGN);
    const bool locked = lock_memory();
    const bool rt = make_current_thread_realtime(80);
    std::printf("driver: %s, memory %s\n", rt ? "SCHED_FIFO 80" : "SCHED_OTHER (no RT allowed)",
                locked ? "locked" : "not locked");

    int r = 2;
    if (a.scenario == "gain") r = scenario_gain(a);
    else if (a.scenario == "latency") r = scenario_latency(a);
    else if (a.scenario == "latency_change") r = scenario_latency_change(a);
    else if (a.scenario == "crash") r = scenario_crash(a);
    else if (a.scenario == "deadline") r = scenario_deadline(a);
    else if (a.scenario == "nonfinite") r = scenario_nonfinite(a);
    else if (a.scenario == "worker") r = scenario_worker(a);
    else if (a.scenario == "worker_restore") r = scenario_worker_restore(a);
    else if (a.scenario == "refusals") r = scenario_refusals(a);
    else if (a.scenario == "incompatible") r = scenario_incompatible(a);
    else if (a.scenario == "bypass") r = scenario_bypass(a);
    else if (a.scenario == "swap") r = scenario_swap(a);
    else if (a.scenario == "rtt") r = scenario_rtt(a);
    else if (a.scenario == "params") r = scenario_params(a);
    else if (a.scenario == "list_json") r = scenario_list_json(a);
    else if (a.scenario == "render") r = scenario_render(a);
    else if (a.scenario == "key") r = scenario_key(a);
    else if (a.scenario == "control") r = scenario_control(a);
    else if (a.scenario == "control_incompatible") r = scenario_control_incompatible(a);
    else if (a.scenario == "state") r = scenario_state(a);
    else if (a.scenario == "gesture") r = scenario_gesture(a);
    else if (a.scenario == "reconfigure") r = scenario_reconfigure(a);
    else std::fprintf(stderr, "plugin_driver: unknown scenario %s\n", a.scenario.c_str());

    if (r != 0) return r;
    std::printf("RESULT %s: %s\n", a.scenario.c_str(), failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
