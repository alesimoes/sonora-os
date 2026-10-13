// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-plugin-host: runs third-party LV2 plugins in a process of their own
// (ADR-0007 §3), so a plugin that crashes ends this process and not the
// engine's session.
//
//   sonora-plugin-host --list [--json] [--bundle DIR]...
//   sonora-plugin-host --probe URI [--channels N] [--bundle DIR]...
//   sonora-plugin-host --shm-fd FD --chain URI[,URI...] [--bundle DIR]...
//                      [--engine-pid PID] [--set INDEX:SYMBOL=VALUE]...
//
// --engine-pid is the engine's pid: the host exits at once if that process is
// not its parent, and later when it stops being so (ipc.h, die_with_parent).
//
// Exit codes in --shm-fd mode: 0 clean shutdown, 2 usage or channel error,
// 3 chain refused at load or validation, 4 a plugin faulted while running.
// The reason is also written into the channel header (refusal,
// refusal_instance), which is what the engine reads; stderr is for people.
//
// Plugin editors (UI windows) are not hosted yet. Under ADR-0010 they will be
// parentless X11 toplevels placed by labwc through Xwayland, opened by this
// process, with the opaque parent-window token of ADR-0007 update 1 item 6
// carried but empty on the appliance.

#include "ipc.h"
#include "lv2_host.h"
#include "sonora/plugin/protocol.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace sonora;
using namespace sonora::plugin_host;

namespace {

struct Options {
    bool list = false, json = false;
    std::string probe;
    int shm_fd = -1;
    int64_t engine_pid = 0;
    std::vector<std::string> chain, bundles;
    struct Set {
        uint32_t instance;
        std::string symbol;
        float value;
    };
    std::vector<Set> sets;
    uint32_t channels = 0;
};

[[noreturn]] void usage(const char* why) {
    std::fprintf(stderr,
                 "sonora-plugin-host: %s\n"
                 "usage: sonora-plugin-host --list [--json] [--bundle DIR]...\n"
                 "       sonora-plugin-host --probe URI [--channels N] [--bundle DIR]...\n"
                 "       sonora-plugin-host --shm-fd FD --chain URI[,URI...] [--bundle DIR]...\n"
                 "                          [--engine-pid PID] [--set INDEX:SYMBOL=VALUE]...\n",
                 why);
    std::exit(2);
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
        else if (a == "--json") o.json = true;
        else if (a == "--probe") o.probe = next();
        else if (a == "--shm-fd") o.shm_fd = std::atoi(next().c_str());
        else if (a == "--engine-pid") o.engine_pid = std::atoll(next().c_str());
        else if (a == "--chain") o.chain = split(next(), ',');
        else if (a == "--bundle") o.bundles.push_back(next());
        else if (a == "--channels") o.channels = static_cast<uint32_t>(std::atoi(next().c_str()));
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
    const int modes = int(o.list) + int(!o.probe.empty()) + int(o.shm_fd >= 0);
    if (modes != 1) usage("choose exactly one of --list, --probe, --shm-fd");
    if (o.shm_fd >= 0 && o.chain.empty()) usage("--shm-fd needs --chain");
    return o;
}

void load_bundles(Lv2World& world, const Options& o) {
    for (const std::string& b : o.bundles) {
        std::string err;
        if (!world.load_bundle(b, err)) std::fprintf(stderr, "sonora-plugin-host: %s\n", err.c_str());
    }
}

// --list: static description, no plugin code runs. The latency field says
// whether the plugin reports one and through which port; the value needs an
// instance, which is --probe.
int list(const Options& o) {
    Lv2World world;
    load_bundles(world, o);
    for (const LilvPlugin* p : world.all()) {
        const PluginInfo info = world.describe(p);
        if (!o.json) {
            std::printf("%s\n  uri:      %s\n  class:    %s\n  audio:    %u in, %u out\n",
                        info.name.c_str(), info.uri.c_str(), info.class_label.c_str(),
                        info.audio_in, info.audio_out);
            if (info.latency_port >= 0)
                std::printf("  latency:  reported on port %d\n", info.latency_port);
            else
                std::printf("  latency:  not reported (0)\n");
            if (!info.missing_features.empty())
                std::printf("  unusable: requires %s\n", info.missing_features.front().c_str());
            for (const PortInfo& port : info.ports) {
                std::printf("  port %2u   %-7s %-3s %-16s", port.index, port_kind_name(port.kind).c_str(),
                            port.input ? "in" : "out", port.symbol.c_str());
                if (port.kind == PortKind::Control && port.input)
                    std::printf(" [%g .. %g] default %g", port.min, port.max, port.def);
                std::printf("  %s\n", port.name.c_str());
            }
            continue;
        }
        std::printf("{\"uri\":\"%s\",\"name\":\"%s\",\"class\":\"%s\",\"audio_in\":%u,"
                    "\"audio_out\":%u,\"latency_port\":%d,\"usable\":%s,\"ports\":[",
                    json_escape(info.uri).c_str(), json_escape(info.name).c_str(),
                    json_escape(info.class_label).c_str(), info.audio_in, info.audio_out,
                    info.latency_port, info.missing_features.empty() ? "true" : "false");
        for (std::size_t i = 0; i < info.ports.size(); ++i) {
            const PortInfo& port = info.ports[i];
            std::printf("%s{\"index\":%u,\"symbol\":\"%s\",\"name\":\"%s\",\"type\":\"%s\","
                        "\"direction\":\"%s\"",
                        i ? "," : "", port.index, json_escape(port.symbol).c_str(),
                        json_escape(port.name).c_str(), port_kind_name(port.kind).c_str(),
                        port.input ? "in" : "out");
            if (port.kind == PortKind::Control)
                std::printf(",\"min\":%g,\"max\":%g,\"default\":%g", port.min, port.max, port.def);
            std::printf("}");
        }
        std::printf("]}\n");
    }
    return 0;
}

// --probe: instantiate one plugin, validate it and report its latency at
// 48 kHz / 128. Runs plugin code, so a scanner runs one probe per process:
// a plugin that crashes on instantiate then costs one entry, not the list.
int probe(const Options& o) {
    Lv2World world;
    load_bundles(world, o);
    const LilvPlugin* p = world.find(o.probe);
    if (!p) {
        std::printf("{\"uri\":\"%s\",\"ok\":false,\"reason\":\"not installed\"}\n",
                    json_escape(o.probe).c_str());
        return 3;
    }
    const PluginInfo info = world.describe(p);
    ChainConfig cfg;
    cfg.channels = o.channels ? o.channels : (info.audio_in == info.audio_out && info.audio_in ? info.audio_in : 1);
    Lv2Chain chain(world, cfg);
    ChainError err;
    const bool ok = chain.add(o.probe, err) && chain.validate(err);
    if (!ok) {
        std::printf("{\"uri\":\"%s\",\"ok\":false,\"reason\":\"%s\"}\n", json_escape(o.probe).c_str(),
                    json_escape(err.message).c_str());
        return 3;
    }
    std::printf("{\"uri\":\"%s\",\"name\":\"%s\",\"ok\":true,\"channels\":%u,\"replicas\":%u,"
                "\"latency_frames\":%u,\"sample_rate\":%u}\n",
                json_escape(info.uri).c_str(), json_escape(info.name).c_str(), cfg.channels,
                chain.replicas(0), chain.instance_latency(0), cfg.sample_rate);
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
    return state == plugin::HostState::Refused ? 3 : 4;
}

void publish_latency(plugin::ChannelHeader& h, const Lv2Chain& chain, uint32_t ipc_frames) {
    for (std::size_t i = 0; i < chain.size(); ++i)
        h.instances[i].latency_frames.store(chain.instance_latency(i), std::memory_order_relaxed);
    // The IPC's own delay is declared like any look-ahead (ADR-0007 §4.3).
    const uint32_t total = chain.total_latency() + ipc_frames;
    if (total != h.latency_frames.load(std::memory_order_relaxed)) {
        h.latency_frames.store(total, std::memory_order_relaxed);
        h.latency_generation.fetch_add(1, std::memory_order_release);
    }
}

int serve(const Options& o) {
    if (!die_with_parent(o.engine_pid)) {
        std::fprintf(stderr, "sonora-plugin-host: the engine (pid %lld) is not this host's parent; "
                             "it is gone, so this host exits\n",
                     static_cast<long long>(o.engine_pid));
        return 2;
    }
    SharedRegion region;
    std::string err;
    if (!map_region(o.shm_fd, region, err)) {
        std::fprintf(stderr, "sonora-plugin-host: %s\n", err.c_str());
        return 2;
    }
    auto& h = *static_cast<plugin::ChannelHeader*>(region.base);
    if (!plugin::header_valid(h, region.size) || h.instance_count != o.chain.size()) {
        std::fprintf(stderr, "sonora-plugin-host: the channel header is not one this host understands "
                             "(layout version %u, want %u)\n",
                     h.layout_version, plugin::kLayoutVersion);
        return 2;
    }
    // Taken once from the validated header. Nothing below re-reads the fixed
    // part, which a plugin in this process could overwrite.
    const plugin::Layout layout = plugin::layout_of(h);
    const uint32_t ipc_frames = h.pipeline_depth * h.block_frames;

    // Loading runs at normal priority: plugins read files and allocate in
    // instantiate(), and that must not happen at SCHED_FIFO.
    Lv2World world;
    load_bundles(world, o);
    ChainConfig cfg;
    cfg.sample_rate = h.sample_rate;
    cfg.max_frames = layout.max_frames;
    cfg.block_frames = h.block_frames;
    cfg.channels = layout.channels;
    const int rt_priority = h.host_rt_priority;
    Lv2Chain chain(world, cfg);
    ChainError cerr;
    for (const std::string& uri : o.chain)
        if (!chain.add(uri, cerr)) return refuse(h, cerr, plugin::HostState::Refused);
    for (const Options::Set& s : o.sets)
        if (!chain.set_control(s.instance, s.symbol, s.value))
            std::fprintf(stderr, "sonora-plugin-host: --set %u:%s: no such input control\n",
                         s.instance, s.symbol.c_str());
    if (!chain.validate(cerr)) return refuse(h, cerr, plugin::HostState::Refused);
    for (std::size_t i = 0; i < chain.size(); ++i)
        h.instances[i].replicas.store(chain.replicas(i), std::memory_order_relaxed);
    publish_latency(h, chain, ipc_frames);

    uint32_t flags = 0;
    if (lock_memory()) flags |= plugin::kHostMemoryLocked;
    if (make_current_thread_realtime(rt_priority)) flags |= plugin::kHostRealtime;
    // Every plugin runs on this thread from here on. Validation ran without
    // the flags, so what a plugin did with denormals there was its worst case.
    if (flush_denormals_on_this_thread()) flags |= plugin::kHostFlushDenormals;
    h.host_flags.store(flags, std::memory_order_relaxed);

    std::vector<const float*> in(layout.channels);
    std::vector<float*> out(layout.channels);
    for (uint32_t c = 0; c < layout.channels; ++c) {
        in[c] = plugin::input_plane(region.base, layout, c);
        out[c] = plugin::output_plane(region.base, layout, c);
    }
    h.state.store(static_cast<uint32_t>(plugin::HostState::Ready), std::memory_order_release);
    signal(h.done_word);

    // Idle wake period: bounds how stale the heartbeat can be.
    constexpr uint64_t kIdleWaitNs = 100'000'000;
    uint64_t last = h.completed_block.load(std::memory_order_relaxed);
    for (;;) {
        // The word is read before the condition, so a submit that lands
        // between the check and the wait changes the word and the futex
        // returns at once: no lost wake-up.
        const uint32_t seen = h.request_word.load(std::memory_order_acquire);
        if (h.shutdown.load(std::memory_order_acquire)) break;
        const uint64_t block = h.submitted_block.load(std::memory_order_acquire);
        if (block == last) {
            wait_for_change(h.request_word, seen, kIdleWaitNs);
            h.heartbeat.fetch_add(1, std::memory_order_relaxed);
            // Checked when idle only, never between a submit and its
            // completion: the backstop for a death signal that cannot fire.
            if (!parent_is(o.engine_pid)) break;
            continue;
        }
        // Clamped, so a forged frame count cannot run past the planes.
        const uint32_t frames =
            std::min(h.submitted_frames.load(std::memory_order_relaxed), layout.max_frames);
        plugin::ParamChange pc;
        while (plugin::pop_param(h, pc)) chain.set_control(pc.instance, pc.port, pc.value);

        const uint64_t t0 = monotonic_ns();
        if (!chain.process(in.data(), out.data(), frames, &h, cerr)) {
            cerr.message = "wrote outside its declared buffers while running";
            return refuse(h, cerr, plugin::HostState::Faulted);
        }
        const uint64_t took = monotonic_ns() - t0;
        if (took > h.worst_block_ns.load(std::memory_order_relaxed))
            h.worst_block_ns.store(took, std::memory_order_relaxed);
        publish_latency(h, chain, ipc_frames);

        last = block;
        h.completed_block.store(block, std::memory_order_release);
        signal(h.done_word);
        h.heartbeat.fetch_add(1, std::memory_order_relaxed);
    }
    h.state.store(static_cast<uint32_t>(plugin::HostState::Stopped), std::memory_order_release);
    signal(h.done_word);
    return 0;
}


}  // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    if (o.list) return list(o);
    if (!o.probe.empty()) return probe(o);
    return serve(o);
}
