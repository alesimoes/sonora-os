// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-plugin-host: runs third-party LV2 and VST3 plugins in a process of
// their own (ADR-0007 §3), so a plugin that crashes ends this process and not
// the engine's session.
//
//   sonora-plugin-host --list [--json] [--format lv2|vst3] [--scan-timeout S] [--bundle DIR]...
//   sonora-plugin-host --probe ID [--channels N] [--bundle DIR]...
//   sonora-plugin-host --version
//   sonora-plugin-host --shm-fd FD --clock-fd FD --chain ID[,ID...] [--bundle DIR]...
//                      [--engine-pid PID] [--set INDEX:SYMBOL=VALUE]...
//
// An ID is an LV2 plugin URI, or "vst3:" and a VST3 class ID in hex
// (vst3_host.h). A chain may mix the two. A --bundle ending in ".vst3" is a
// VST3 bundle searched before ~/.vst3, /usr/lib/vst3 and /usr/local/lib/vst3;
// any other is an LV2 bundle.
//
// --shm-fd is the channel region (read-write), --clock-fd the session's
// clock region (read-only, sealed; contracts/plugin). --engine-pid is the
// engine's pid: the host exits at once if that process is not its parent,
// and later when it stops being so (ipc.h, die_with_parent).
//
// --version prints the contract versions this binary speaks; the engine
// runs it at start-up (ADR-0012 §5.5).
//
// Exit codes in --shm-fd mode (contracts/plugin, kHostExit*): 0 clean
// shutdown, 2 usage or a channel that is not valid, 3 the engine's layout or
// clock version is not this host's (nothing written), 4 a plugin faulted
// while running, 5 the chain was refused at load or validation. The reason
// is also written into the channel header (refusal, refusal_instance),
// which is what the engine reads; stderr is for people.
//
// Plugin editors (UI windows) are not hosted yet. Under ADR-0010 they will be
// parentless X11 toplevels placed by labwc through Xwayland, opened by this
// process, with the opaque parent-window token of ADR-0007 update 1 item 6
// carried but empty on the appliance.

#include "chain.h"
#include "ipc.h"
#include "loader.h"
#include "lv2_host.h"
#include "sonora/plugin/protocol.h"
#ifdef SONORA_HAVE_VST3
#include "vst3_host.h"
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace sonora;
using namespace sonora::plugin_host;

namespace {

bool is_vst3_id(const std::string& id) { return id.rfind("vst3:", 0) == 0; }

bool is_vst3_bundle(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path.size() > 5 && path.compare(path.size() - 5, 5, ".vst3") == 0;
}

// Every format behind one PluginSource, so a chain neither knows nor cares
// which format each of its plugins is.
class Catalog : public PluginSource {
public:
    Lv2World lv2;
#ifdef SONORA_HAVE_VST3
    Vst3World vst3;
#endif

    std::unique_ptr<Processor> instantiate(const std::string& id, const ChainConfig& config,
                                           const std::vector<float*>& in,
                                           const std::vector<float*>& out,
                                           ChainError& error) override {
        if (is_vst3_id(id)) {
#ifdef SONORA_HAVE_VST3
            return vst3.instantiate(id, config, in, out, error);
#else
            error.reason = plugin::Refusal::UnsupportedFeature;
            error.message = "this sonora-plugin-host was built without VST3: " + id;
            return nullptr;
#endif
        }
        return lv2.instantiate(id, config, in, out, error);
    }
};

struct Options {
    bool list = false, json = false, version = false;
    std::string probe, format;
    int shm_fd = -1, clock_fd = -1;
    int64_t engine_pid = 0;
    std::vector<std::string> chain, bundles;
    struct Set {
        uint32_t instance;
        std::string symbol;
        float value;
    };
    std::vector<Set> sets;
    uint32_t channels = 0;
    // LSP's bundle, 184 classes each instantiated and run, took 19.9 s in
    // the container (brain/research/plugin-host-vst3.md).
    uint32_t scan_timeout_s = 120;
};

[[noreturn]] void usage(const char* why) {
    std::fprintf(stderr,
                 "sonora-plugin-host: %s\n"
                 "usage: sonora-plugin-host --list [--json] [--format lv2|vst3] [--scan-timeout S]\n"
                 "                          [--bundle DIR]...\n"
                 "       sonora-plugin-host --probe ID [--channels N] [--bundle DIR]...\n"
                 "       sonora-plugin-host --version\n"
                 "       sonora-plugin-host --shm-fd FD --clock-fd FD --chain ID[,ID...] [--bundle DIR]...\n"
                 "                          [--engine-pid PID] [--set INDEX:SYMBOL=VALUE]...\n",
                 why);
    std::exit(plugin::kHostExitUsage);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        const std::size_t end = s.find(sep, start);
        const std::string part = s.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty()) out.push_back(part);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) usage(("missing value after " + a).c_str());
            return argv[++i];
        };
        if (a == "--list") o.list = true;
        else if (a == "--version") o.version = true;
        else if (a == "--clock-fd") o.clock_fd = std::atoi(next().c_str());
        else if (a == "--json") o.json = true;
        else if (a == "--format") {
            o.format = next();
            if (o.format != "lv2" && o.format != "vst3") usage("--format is lv2 or vst3");
        }
        else if (a == "--probe") o.probe = next();
        else if (a == "--shm-fd") o.shm_fd = std::atoi(next().c_str());
        else if (a == "--engine-pid") o.engine_pid = std::atoll(next().c_str());
        else if (a == "--chain") o.chain = split(next(), ',');
        else if (a == "--bundle") o.bundles.push_back(next());
        else if (a == "--channels") o.channels = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (a == "--scan-timeout") {
            const int s = std::atoi(next().c_str());
            if (s < 1) usage("--scan-timeout wants a number of seconds, 1 or more");
            o.scan_timeout_s = static_cast<uint32_t>(s);
        }
        else if (a == "--set") {
            const std::string v = next();
            const auto colon = v.find(':'), eq = v.find('=');
            if (colon == std::string::npos || eq == std::string::npos || eq < colon)
                usage("--set wants INDEX:SYMBOL=VALUE");
            o.sets.push_back({static_cast<uint32_t>(std::atoi(v.substr(0, colon).c_str())),
                              v.substr(colon + 1, eq - colon - 1),
                              std::strtof(v.c_str() + eq + 1, nullptr)});
        } else usage(("unknown argument " + a).c_str());
    }
    const int modes = int(o.list) + int(!o.probe.empty()) + int(o.shm_fd >= 0) + int(o.version);
    if (modes != 1) usage("choose exactly one of --list, --probe, --shm-fd, --version");
    if (o.shm_fd >= 0 && o.chain.empty()) usage("--shm-fd needs --chain");
    if (o.shm_fd >= 0 && o.clock_fd < 0) usage("--shm-fd needs --clock-fd");
    return o;
}

void load_bundles(Catalog& catalog, const Options& o) {
    for (const std::string& b : o.bundles) {
        if (is_vst3_bundle(b)) {
#ifdef SONORA_HAVE_VST3
            catalog.vst3.add_bundle(b);
#else
            std::fprintf(stderr, "sonora-plugin-host: built without VST3, ignoring %s\n", b.c_str());
#endif
            continue;
        }
        std::string err;
        if (!catalog.lv2.load_bundle(b, err)) std::fprintf(stderr, "sonora-plugin-host: %s\n", err.c_str());
    }
}

// One --list entry, the same shape for every format.
void print_info(std::FILE* f, const PluginInfo& info, bool json) {
    const bool vst3 = info.format == "vst3";
    if (!json) {
        std::fprintf(f, "%s\n  uri:      %s\n  format:   %s\n  class:    %s\n  audio:    %u in, %u out\n",
                    info.name.c_str(), info.uri.c_str(), info.format.c_str(), info.class_label.c_str(),
                    info.audio_in, info.audio_out);
        if (info.latency_port >= 0)
            std::fprintf(f, "  latency:  reported on port %d\n", info.latency_port);
        else if (info.latency_frames >= 0)
            std::fprintf(f, "  latency:  %lld frames at 48 kHz, 128-frame blocks\n",
                        static_cast<long long>(info.latency_frames));
        else
            std::fprintf(f, "  latency:  not reported (0)\n");
        if (!info.missing_features.empty())
            std::fprintf(f, "  unusable: %s%s\n", vst3 ? "" : "requires ", info.missing_features.front().c_str());
        for (const PortInfo& port : info.ports) {
            std::fprintf(f, "  port %2u   %-7s %-3s %-16s", port.index, port_kind_name(port.kind).c_str(),
                        port.input ? "in" : "out", port.symbol.c_str());
            if (port.kind == PortKind::Audio && vst3) std::fprintf(f, " %u channel(s)", port.channels);
            if (port.kind == PortKind::Control && port.input)
                std::fprintf(f, " [%g .. %g] default %g", port.min, port.max, port.def);
            if (!port.units.empty()) std::fprintf(f, " %s", port.units.c_str());
            if (!vst3) std::fprintf(f, "  %s", port.name.c_str());
            std::fprintf(f, "\n");
        }
        return;
    }
    std::fprintf(f, "{\"uri\":\"%s\",\"format\":\"%s\",\"name\":\"%s\",\"class\":\"%s\",\"audio_in\":%u,"
                "\"audio_out\":%u,\"latency_port\":%d,\"latency_frames\":%lld,\"usable\":%s",
                json_escape(info.uri).c_str(), json_escape(info.format).c_str(),
                json_escape(info.name).c_str(), json_escape(info.class_label).c_str(), info.audio_in,
                info.audio_out, info.latency_port, static_cast<long long>(info.latency_frames),
                info.missing_features.empty() ? "true" : "false");
    if (!info.missing_features.empty())
        std::fprintf(f, ",\"reason\":\"%s\"", json_escape(info.missing_features.front()).c_str());
    std::fprintf(f, ",\"ports\":[");
    uint32_t dense = 0;
    for (std::size_t i = 0; i < info.ports.size(); ++i) {
        const PortInfo& port = info.ports[i];
        std::fprintf(f, "%s{\"index\":%u,\"symbol\":\"%s\",\"name\":\"%s\",\"type\":\"%s\","
                    "\"direction\":\"%s\"",
                    i ? "," : "", port.index, json_escape(port.symbol).c_str(),
                    json_escape(port.name).c_str(), port_kind_name(port.kind).c_str(),
                    port.input ? "in" : "out");
        // The dense index the engine sends (contracts/plugin, "Parameters").
        if (port.kind == PortKind::Control && port.input) std::fprintf(f, ",\"param\":%u", dense++);
        if (port.kind == PortKind::Audio && vst3) std::fprintf(f, ",\"channels\":%u", port.channels);
        if (port.kind == PortKind::Control)
            std::fprintf(f, ",\"min\":%g,\"max\":%g,\"default\":%g", port.min, port.max, port.def);
        if (!port.units.empty()) std::fprintf(f, ",\"units\":\"%s\"", json_escape(port.units).c_str());
        std::fprintf(f, "}");
    }
    std::fprintf(f, "]}\n");
}

#ifdef SONORA_HAVE_VST3
void print_failed_bundle(std::FILE* f, const std::string& bundle, const std::string& reason, bool json) {
    if (json)
        std::fprintf(f, "{\"bundle\":\"%s\",\"format\":\"vst3\",\"usable\":false,\"reason\":\"%s\"}\n",
                    json_escape(bundle).c_str(), json_escape(reason).c_str());
    else
        std::fprintf(f, "%s\n  format:   vst3\n  unusable: %s\n", bundle.c_str(), reason.c_str());
}
#endif

// --list. LV2 is a static description: no plugin code runs, and the latency
// field says through which port a plugin reports it. A VST3 class has to be
// loaded and instantiated to be described at all, so each bundle is scanned
// in a child process of its own: one that crashes or hangs costs its own
// entry, not the list, and the latency printed is the instance's own at
// 48 kHz.
int list(const Options& o) {
    Catalog catalog;
    load_bundles(catalog, o);
    std::size_t lv2_count = 0, vst3_count = 0, vst3_failed = 0;
    if (o.format.empty() || o.format == "lv2") {
        for (const LilvPlugin* p : catalog.lv2.all()) {
            print_info(stdout, catalog.lv2.describe(p), o.json);
            ++lv2_count;
        }
    }
#ifdef SONORA_HAVE_VST3
    if (o.format.empty() || o.format == "vst3") {
        const uint64_t scan_timeout_ns = uint64_t(o.scan_timeout_s) * 1'000'000'000ull;
        for (const std::string& bundle : catalog.vst3.bundles()) {
            // Each record is flushed as soon as it is written (run_isolated:
            // the parent keeps whole lines only), so a class that hangs or
            // crashes costs itself and the classes after it, not those before.
            const IsolatedResult r = run_isolated(
                [&](std::FILE* out) {
                    Vst3World world;
                    std::string err;
                    const bool ok = world.describe_bundle(
                        bundle,
                        [&](const PluginInfo& info) {
                            print_info(out, info, o.json);
                            std::fflush(out);
                        },
                        err);
                    if (!ok) {
                        print_failed_bundle(out, bundle, err, o.json);
                        std::fflush(out);
                        return 1;
                    }
                    return 0;
                },
                scan_timeout_ns);
            std::fwrite(r.output.data(), 1, r.output.size(), stdout);
            const char* const key = o.json ? "{\"uri\":" : "\n  uri:";
            for (std::size_t at = r.output.find(key); at != std::string::npos; at = r.output.find(key, at + 1))
                ++vst3_count;
            const bool printed_failure = r.exited && r.exit_code == 1;
            if (!r.exited || r.exit_code != 0) {
                ++vst3_failed;
                if (!printed_failure) {
                    std::string why;
                    if (r.timed_out)
                        why = "the scan hung (killed after " + std::to_string(o.scan_timeout_s) + " s)";
                    else if (!r.exited) why = "the scan crashed (signal " + std::to_string(r.signal) + ", " +
                                              strsignal(r.signal) + ")";
                    else why = "the scan exited with code " + std::to_string(r.exit_code);
                    print_failed_bundle(stdout, bundle, why, o.json);
                }
            }
        }
    }
#endif
    std::fflush(stdout);
    std::fprintf(stderr, "sonora-plugin-host: listed %zu LV2 plugin(s), %zu VST3 class(es); "
                         "vst3_failed=%zu bundle(s)\n",
                 lv2_count, vst3_count, vst3_failed);
    return 0;
}

// --probe: instantiate one plugin, validate it and report its latency at
// 48 kHz / 128. Runs plugin code, so a scanner runs one probe per process:
// a plugin that crashes on instantiate then costs one entry, not the list.
int probe(const Options& o) {
    Catalog catalog;
    load_bundles(catalog, o);
    PluginInfo info;
    ChainError err;
    bool found = false;
    if (is_vst3_id(o.probe)) {
#ifdef SONORA_HAVE_VST3
        found = catalog.vst3.describe(o.probe, info, err);
#else
        err.message = "this sonora-plugin-host was built without VST3";
#endif
    } else if (const LilvPlugin* p = catalog.lv2.find(o.probe)) {
        info = catalog.lv2.describe(p);
        found = true;
    } else {
        err.message = "not installed";
    }
    if (!found) {
        std::printf("{\"uri\":\"%s\",\"ok\":false,\"reason\":\"%s\"}\n", json_escape(o.probe).c_str(),
                    json_escape(err.message).c_str());
        return plugin::kHostExitRefused;
    }
    ChainConfig cfg;
    cfg.channels = o.channels ? o.channels : (info.audio_in == info.audio_out && info.audio_in ? info.audio_in : 1);
    Chain chain(catalog, cfg);
    const bool added = chain.add(o.probe, err);
    const bool ok = added && chain.validate(err);
    // The dense parameter list the engine's catalog keeps (ADR-0012 §8):
    // the index is the position, the key what the session file stores.
    auto params = [&] {
        std::string s = ",\"bypass\":\"";
        s += chain.designated_bypass(0) ? "plugin" : "host";
        s += "\",\"params\":[";
        for (uint32_t d = 0; d < chain.param_count(0); ++d) {
            char def[48];
            std::snprintf(def, sizeof def, "%.9g", double(chain.param_default(0, d)));
            s += (d ? ",{\"key\":\"" : "{\"key\":\"") + json_escape(chain.param_key(0, d)) +
                 "\",\"default\":" + def + "}";
        }
        return s + "]";
    };
    if (!ok) {
        std::printf("{\"uri\":\"%s\",\"ok\":false,\"reason\":\"%s\"%s}\n", json_escape(o.probe).c_str(),
                    json_escape(err.message).c_str(), added ? params().c_str() : "");
        return plugin::kHostExitRefused;
    }
    std::printf("{\"uri\":\"%s\",\"name\":\"%s\",\"format\":\"%s\",\"ok\":true,\"channels\":%u,"
                "\"replicas\":%u,\"latency_frames\":%u,\"sample_rate\":%u%s}\n",
                json_escape(info.uri).c_str(), json_escape(info.name).c_str(), info.format.c_str(),
                cfg.channels, chain.replicas(0), chain.instance_latency(0), cfg.sample_rate,
                params().c_str());
    return 0;
}

int refuse(plugin::ChannelHeader& h, const ChainError& err, plugin::HostState state) {
    std::fprintf(stderr, "sonora-plugin-host: %s instance %d: %s\n",
                 state == plugin::HostState::Refused ? "refused" : "faulted", err.instance,
                 err.message.c_str());
    h.refusal.store(static_cast<uint32_t>(err.reason), std::memory_order_relaxed);
    h.refusal_instance.store(err.instance, std::memory_order_relaxed);
    h.state.store(static_cast<uint32_t>(state), std::memory_order_release);
    signal(h.done_word);
    return state == plugin::HostState::Refused ? plugin::kHostExitRefused : plugin::kHostExitFaulted;
}

void publish_latency(plugin::ChannelHeader& h, const Chain& chain, uint32_t ipc_frames) {
    for (std::size_t i = 0; i < chain.size(); ++i)
        h.instances[i].latency_frames.store(chain.instance_latency(i), std::memory_order_relaxed);
    // The IPC's own delay is declared like any look-ahead (ADR-0007 §4.3).
    const uint32_t total = chain.total_latency() + ipc_frames;
    if (total != h.latency_frames.load(std::memory_order_relaxed)) {
        h.latency_frames.store(total, std::memory_order_relaxed);
        h.latency_generation.fetch_add(1, std::memory_order_release);
    }
}

// Idle wake period in futex mode: bounds how stale the heartbeat can be.
constexpr uint64_t kIdleWaitNs = 100'000'000;
// Timer mode (ADR-0012 §1.3). How long after a callback's start the host
// looks for its submit (placeholder), the step while that callback is late,
// and the sleep while the engine is not calling back at all.
constexpr uint64_t kTimerOffsetNs = 150'000;
constexpr uint64_t kLatePollNs = 50'000;
constexpr uint64_t kStaleSleepNs = 10'000'000;
constexpr uint64_t kParentCheckNs = 100'000'000;

// The timer-paced wait, the default until the futex wake is measured on
// bare metal (ADR-0012 §1.3, auditor A1). Hosts wake on a guess: the next
// expected callback plus an offset; if nothing has been submitted then, the
// callback is late, so look again every 50 us for at most half a period.
class TimerPacer {
public:
    void found() noexcept {
        expected_ = 0;
        poll_until_ = 0;
    }

    void wait(const plugin::ClockHeader& clock, uint64_t period_ns) noexcept {
        const uint64_t now = monotonic_ns();
        const uint64_t cb = clock.callback_ns.load(std::memory_order_acquire);
        // No callback yet, or none for several periods: the engine is not
        // running its graph. Polling every period would be wasted wakes; the
        // cost is a few missed blocks when it starts again.
        if (cb == 0 || now > cb + 4 * period_ns) {
            found();
            sleep_until_ns(now + kStaleSleepNs);
            return;
        }
        if (expected_ != 0) {
            // Woken for a callback that has not submitted: it is late.
            poll_until_ = expected_ + period_ns / 2;
            expected_ = 0;
        }
        if (poll_until_ != 0 && now < poll_until_) {
            sleep_until_ns(now + kLatePollNs);
            return;
        }
        poll_until_ = 0;
        const uint64_t at = plugin::next_wake_ns(cb, period_ns, kTimerOffsetNs, now);
        expected_ = at - kTimerOffsetNs;
        sleep_until_ns(at);
    }

private:
    uint64_t expected_ = 0;    // the callback start the last sleep aimed at
    uint64_t poll_until_ = 0;  // polling a late callback until then
};

// What the processing thread works with, every part of it taken once from
// validated headers or built in private memory.
struct Session {
    plugin::ChannelHeader& h;
    const plugin::ClockHeader& clock;
    Chain& chain;
    plugin::Layout layout;
    plugin::ParamMap map;
    plugin::WakeMode mode;
    uint64_t period_ns;
    const std::atomic<float>* table;
    plugin::HostValues values;
    std::vector<const float*> in;
    std::vector<float*> out;
    uint32_t ipc_frames;
    int64_t engine_pid;
    uint32_t block_frames;  // every block's size in v1 (contracts/plugin, Frame count)
};

// The processing thread's loop, at SCHED_FIFO when allowed. Returns the exit
// code: 0 on shutdown or a gone engine, kHostExitFaulted when a plugin faulted.
int process_blocks(Session& s) {
    plugin::ChannelHeader& h = s.h;
    ChainError cerr;
    TimerPacer pacer;
    uint64_t last = h.completed_block.load(std::memory_order_relaxed);
    uint64_t next_parent_check = monotonic_ns() + kParentCheckNs;
    auto deliver = [&](uint32_t i, uint32_t p, uint32_t offset, float v) noexcept {
        return p == plugin::kBypassParam ? s.chain.set_bypass(i, v > 0.5f, offset)
                                         : s.chain.set_param(i, p, v, offset);
    };
    auto replace = [&](uint32_t i, uint32_t p, float v) noexcept {
        return p == plugin::kBypassParam ? s.chain.set_bypass(i, v > 0.5f, 0) : s.chain.replace_param(i, p, v);
    };
    for (;;) {
        if (h.shutdown.load(std::memory_order_acquire)) break;
        // Auditor M2: tick FIRST, then submitted_block. A submit landing
        // between the two has already moved tick, so the futex wait below
        // returns at once instead of sleeping through the block.
        const uint32_t seen = s.clock.tick.load(std::memory_order_acquire);
        const uint64_t block = h.submitted_block.load(std::memory_order_acquire);
        if (block != last) {
            pacer.found();
            // v1 pins every block to block_frames (rt-engineer M3 on PR
            // #45): the latency this host declared assumes it. The engine
            // never submits another size, so a different word means a
            // plugin in this process wrote it; process nothing, fault.
            const uint32_t submitted = h.submitted_frames.load(std::memory_order_relaxed);
            if (submitted != s.block_frames) {
                cerr.reason = plugin::Refusal::None;
                cerr.instance = -1;
                cerr.message = "the channel's submitted_frames is " + std::to_string(submitted) +
                               ", not the pinned block_frames " + std::to_string(s.block_frames) +
                               ": the region was overwritten";
                return refuse(h, cerr, plugin::HostState::Faulted);
            }
            const uint32_t frames = s.block_frames;
            // The ring for this block and older ones, then the table if
            // values_seq moved (L1); contracts/plugin, block protocol c.
            plugin::consume_block_params(h, s.table, s.map, s.values, block, frames, deliver, replace);

            const uint64_t t0 = monotonic_ns();
            if (!s.chain.process(s.in.data(), s.out.data(), frames, &h, cerr)) {
                cerr.message = "wrote outside its declared buffers while running";
                return refuse(h, cerr, plugin::HostState::Faulted);
            }
            const uint64_t took = monotonic_ns() - t0;
            if (took > h.worst_block_ns.load(std::memory_order_relaxed))
                h.worst_block_ns.store(took, std::memory_order_relaxed);
            publish_latency(h, s.chain, s.ipc_frames);

            last = block;
            h.completed_block.store(block, std::memory_order_release);
            signal(h.done_word);
            h.heartbeat.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (s.mode == plugin::WakeMode::Futex) wait_for_change(s.clock.tick, seen, kIdleWaitNs);
        else pacer.wait(s.clock, s.period_ns);
        h.heartbeat.fetch_add(1, std::memory_order_relaxed);
        // Checked when idle only, never between a submit and its completion:
        // the backstop for a death signal that cannot fire.
        const uint64_t now = monotonic_ns();
        if (now >= next_parent_check) {
            if (!parent_is(s.engine_pid)) break;
            next_parent_check = now + kParentCheckNs;
        }
    }
    h.state.store(static_cast<uint32_t>(plugin::HostState::Stopped), std::memory_order_release);
    signal(h.done_word);
    return plugin::kHostExitClean;
}

// Exit 3 before writing anything (ADR-0012 §5.5): one line naming both
// versions, for the engine to report as an install fault.
int incompatible(const char* what, const plugin::VersionWords& got, uint32_t want_version,
                 std::size_t want_bytes) {
    std::fprintf(stderr,
                 "sonora-plugin-host: incompatible engine: the %s region is layout %u (%u-byte header); "
                 "this host speaks layout %u (%zu-byte header)\n",
                 what, got.layout_version, got.header_bytes, want_version, want_bytes);
    return plugin::kHostExitIncompatible;
}

int serve(const Options& o) {
    if (!die_with_parent(o.engine_pid)) {
        std::fprintf(stderr, "sonora-plugin-host: the engine (pid %lld) is not this host's parent; "
                             "it is gone, so this host exits\n",
                     static_cast<long long>(o.engine_pid));
        return plugin::kHostExitUsage;
    }
    SharedRegion region, clock_region;
    std::string err;
    if (!map_region(o.shm_fd, region, err) || !map_region_read_only(o.clock_fd, clock_region, err)) {
        std::fprintf(stderr, "sonora-plugin-host: %s\n", err.c_str());
        return plugin::kHostExitUsage;
    }
    if (region.size < sizeof(plugin::VersionWords) || clock_region.size < sizeof(plugin::VersionWords)) {
        std::fprintf(stderr, "sonora-plugin-host: a region is too small to hold a header\n");
        return plugin::kHostExitUsage;
    }
    // The words that never move, first: "not my version" is exit 3, and
    // nothing has been written to either region yet.
    const plugin::VersionWords cv = plugin::version_words(region.base);
    const plugin::VersionWords kv = plugin::version_words(clock_region.base);
    if (cv.magic != plugin::kChannelMagic || kv.magic != plugin::kClockMagic) {
        std::fprintf(stderr, "sonora-plugin-host: --shm-fd is not a plugin channel or --clock-fd not a clock\n");
        return plugin::kHostExitUsage;
    }
    if (!plugin::channel_compatible(cv))
        return incompatible("channel", cv, plugin::kLayoutVersion, sizeof(plugin::ChannelHeader));
    if (!plugin::clock_compatible(kv))
        return incompatible("clock", kv, plugin::kClockLayoutVersion, sizeof(plugin::ClockHeader));

    auto& h = *static_cast<plugin::ChannelHeader*>(region.base);
    const auto& clock = *static_cast<const plugin::ClockHeader*>(clock_region.base);
    if (!plugin::header_valid(h, region.size) || h.instance_count != o.chain.size() ||
        !plugin::clock_valid(clock, clock_region.size) || !plugin::channel_matches_clock(h, clock)) {
        std::fprintf(stderr, "sonora-plugin-host: the channel or clock header is not valid for a chain of %zu\n",
                     o.chain.size());
        return plugin::kHostExitUsage;
    }
    // Taken once from the validated headers. Nothing below re-reads the
    // fixed parts, which a plugin in this process could overwrite.
    const plugin::Layout layout = plugin::layout_of(h);
    const plugin::ParamMap map = plugin::param_map_of(h);
    const auto mode = static_cast<plugin::WakeMode>(clock.wake_mode);
    const uint64_t period_ns = clock.period_ns;
    const uint32_t ipc_frames = plugin::kPipelineDepth * h.block_frames;

    // Loading runs at normal priority: plugins read files and allocate when
    // they are instantiated, and that must not happen at SCHED_FIFO.
    Catalog catalog;
    load_bundles(catalog, o);
    ChainConfig cfg;
    cfg.sample_rate = h.sample_rate;
    cfg.max_frames = layout.max_frames;
    cfg.block_frames = h.block_frames;
    cfg.channels = layout.channels;
    const int rt_priority = h.host_rt_priority;
    Chain chain(catalog, cfg);
    ChainError cerr;
    for (const std::string& id : o.chain)
        if (!chain.add(id, cerr)) return refuse(h, cerr, plugin::HostState::Refused);
    // The engine addresses parameters by the dense list its scan saw. A
    // plugin that now has a different list (another version installed
    // since the scan) would get every change on the wrong parameter.
    for (std::size_t i = 0; i < chain.size(); ++i)
        if (chain.param_count(i) != map.count[i]) {
            cerr.reason = plugin::Refusal::ParamMismatch;
            cerr.instance = static_cast<int>(i);
            cerr.message = chain.info(i).name + " <" + chain.info(i).uri + "> has " +
                           std::to_string(chain.param_count(i)) + " parameters; the engine expects " +
                           std::to_string(map.count[i]);
            return refuse(h, cerr, plugin::HostState::Refused);
        }

    // The values the control thread seeded (contracts/plugin, §5.1), before
    // validation, so the latency reported is the one of these values.
    Session s{h, clock, chain, layout, map, mode, period_ns, plugin::value_table(region.base, layout),
              {}, {}, {}, ipc_frames, o.engine_pid, h.block_frames};
    std::vector<float> applied(map.param_slots, 0.f);
    s.values.applied = applied.data();
    s.values.seen_seq = h.values_seq.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < map.instance_count; ++i)
        for (uint32_t k = 0; k < map.count[i]; ++k) {
            const int64_t slot = plugin::param_slot(map, i, k);
            if (slot < 0) break;
            const float v = s.table[slot].load(std::memory_order_relaxed);
            applied[std::size_t(slot)] = v;
            if (plugin::finite_bits(v)) chain.replace_param(i, k, v);
        }
    for (const Options::Set& set : o.sets)
        if (!chain.set_control(set.instance, set.symbol, set.value))
            std::fprintf(stderr, "sonora-plugin-host: --set %u:%s: no such input control\n", set.instance,
                         set.symbol.c_str());
    if (!chain.validate(cerr)) return refuse(h, cerr, plugin::HostState::Refused);
    chain.prepare_bypass();
    for (uint32_t i = 0; i < map.instance_count; ++i) {
        const float b = h.bypass[i].load(std::memory_order_relaxed);
        s.values.applied_bypass[i] = b;
        if (b > 0.5f) chain.set_bypass(i, true, 0);
    }
    for (std::size_t i = 0; i < chain.size(); ++i)
        h.instances[i].replicas.store(chain.replicas(i), std::memory_order_relaxed);
    publish_latency(h, chain, ipc_frames);

    uint32_t flags = 0;
    // Process-wide, so it covers the processing thread's stack too.
    if (lock_memory()) flags |= plugin::kHostMemoryLocked;

    s.in.resize(layout.channels);
    s.out.resize(layout.channels);
    for (uint32_t c = 0; c < layout.channels; ++c) {
        s.in[c] = plugin::input_plane(region.base, layout, c);
        s.out[c] = plugin::output_plane(region.base, layout, c);
    }

    // Every plugin runs on a thread of its own from here on. This thread,
    // which created them, stays their main thread: VST3 allows some calls
    // (getLatencySamples) only there, never on the thread running process().
    int exit_code = 0;
    std::atomic<bool> finished{false};
    auto body = [&] {
        uint32_t f = flags;
        if (make_current_thread_realtime(rt_priority)) f |= plugin::kHostRealtime;
        // Validation ran without the flags, so what a plugin did with
        // denormals there was its worst case.
        if (flush_denormals_on_this_thread()) f |= plugin::kHostFlushDenormals;
        h.host_flags.store(f, std::memory_order_relaxed);
        h.state.store(static_cast<uint32_t>(plugin::HostState::Ready), std::memory_order_release);
        signal(h.done_word);
        exit_code = process_blocks(s);
        finished.store(true, std::memory_order_release);
        notify_control();
    };
    // 1 MiB, not the default 8: the stack is locked and populated whole
    // (start_thread), so its size is resident RAM in every host process.
    constexpr std::size_t kProcessingStack = std::size_t(1) << 20;
    void* processing = start_thread([](void* b) { (*static_cast<decltype(body)*>(b))(); }, &body,
                                    kProcessingStack);
    if (!processing) {
        std::fprintf(stderr, "sonora-plugin-host: cannot start the processing thread\n");
        return plugin::kHostExitUsage;
    }
    // The control loop. Read the word before the work, so a notice that
    // lands during the work changes it and the wait returns at once.
    for (;;) {
        const uint32_t seen = control_word().load(std::memory_order_acquire);
        if (finished.load(std::memory_order_acquire)) break;
        chain.control();
        wait_for_change(control_word(), seen, kIdleWaitNs);
    }
    join_thread(processing);
    return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    if (o.version) {
        // Read by the engine at start-up (ADR-0012 §5.5); a mismatch
        // disables plugin hosting for the run, loudly.
        // The grammar is the contract's (format_version_line).
        char line[96];
        plugin::format_version_line(line, sizeof line);
        std::printf("%s\n", line);
        return plugin::kHostExitClean;
    }
    // Before any plugin code loads, and inherited by the --list scan children.
    no_core_dumps();
    if (o.list) return list(o);
    if (!o.probe.empty()) return probe(o);
    return serve(o);
}
