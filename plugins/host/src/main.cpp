// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-plugin-host: runs third-party LV2 and VST3 plugins in a process of
// their own (ADR-0007 §3), so a plugin that crashes ends this process and not
// the engine's session.
//
//   sonora-plugin-host --list [--json] [--format lv2|vst3] [--scan-timeout S] [--bundle DIR]...
//                      [--bundles-only] [--only-given] [--engine-pid PID]
//   sonora-plugin-host --probe ID [--channels N] [--bundle DIR]... [--engine-pid PID]
//   sonora-plugin-host --version
//   sonora-plugin-host --shm-fd FD --clock-fd FD --chain ID[,ID...] [--bundle DIR]...
//                      [--engine-pid PID] [--set INDEX:SYMBOL=VALUE]...
//                      [--sidechain N] [--reactivate]
//   sonora-plugin-host --control-fd FD --shm-fd FD --clock-fd FD [--bundle DIR]...
//                      [--engine-pid PID]
//
// --control-fd: the engine's way (contracts/plugin host_control.h, ADR-0012
// §4). The chain, its key channels, its states and whether to re-activate
// come in the Load message on that socket, not on the command line; the
// host answers Loaded or Refused, then serves GetState, DescribeValue,
// OpenEditor/CloseEditor, Reconfigure and Shutdown, and reports gestures,
// latency changes and faults. Without it (the command-line chain) the host
// is the one the tests and measurements drive directly.
//
// --sidechain N: the last N of the region's channels are a key, not signal.
// Every plugin with a key input (LV2 lv2:isSideChain ports, a VST3 aux input
// bus) reads them; the chain processes the others, and the key channels'
// output planes stay silent. The channel header is unchanged: how the engine
// asks for a key belongs to the M9 contract, which this anticipates.
//
// --reactivate: after validation, which hands the --set values to every
// plugin in its first block, deactivate and re-activate every plugin, then
// validate again. A value that only takes effect at activation (ADR-0011
// §1.5: a latency-changing one, such as an EQ's phase mode or a limiter's
// look-ahead) then does, and the latency published is the new one. LV2
// delivers control values only inside run(), so a host that loads a session
// with such a value has to do this; changing one while running is the M9
// control channel's (re-activate and re-plan PDC).
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
// --version prints the contract versions this binary speaks, the probe
// record's included (`probe 1`); the engine runs it at start-up (ADR-0012
// §5.5) and its plugin scanner before it trusts a record.
//
// --list gives every record the bundle it came from (ADR-0012, amendment of
// 2026-10-19, item 1), so the engine's scanner can fingerprint it. With
// --bundles-only it prints only the bundles, one {"bundle","format"} line
// each, running no plugin code at all (VST3 included): what the scanner
// fingerprints before deciding what to enumerate. --only-given limits the
// VST3 scan to the --bundle paths given, so a changed bundle is enumerated
// without running every other one's code again.
//
// --probe prints one probe record (contracts/plugin probe_record.h). With
// --engine-pid, --list and --probe die with the engine as a serving host
// does: a probe runs third-party code and must not outlive the scan.
//
// Exit codes in --shm-fd mode (contracts/plugin, kHostExit*): 0 clean
// shutdown, 2 usage or a channel that is not valid, 3 the engine's layout or
// clock version is not this host's (nothing written), 4 a plugin faulted
// while running, 5 the chain was refused at load or validation. The reason
// is also written into the channel header (refusal, refusal_instance),
// which is what the engine reads; stderr is for people.
//
// Plugin editors (UI windows) are not hosted yet: OpenEditor is answered
// with EditorFailed. Under ADR-0010 they will be parentless X11 toplevels
// placed by labwc through Xwayland, opened by this process, with the opaque
// parent-window token of ADR-0007 update 1 item 6 carried but empty on the
// appliance.

#include "chain.h"
#include "control.h"
#include "ipc.h"
#include "loader.h"
#include "lv2_host.h"
#include "sonora/plugin/host_control.h"
#include "sonora/plugin/probe_record.h"
#include "sonora/plugin/protocol.h"
#include "sonora/rt.h"
#ifdef SONORA_HAVE_VST3
#include "vst3_host.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <unistd.h>

using namespace sonora;
using namespace sonora::plugin_host;
namespace pc = sonora::plugin::control;

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
                                           const std::vector<float*>& key,
                                           ChainError& error) override {
        if (is_vst3_id(id)) {
#ifdef SONORA_HAVE_VST3
            return vst3.instantiate(id, config, in, out, key, error);
#else
            error.reason = plugin::Refusal::UnsupportedFeature;
            error.message = "this sonora-plugin-host was built without VST3: " + id;
            return nullptr;
#endif
        }
        return lv2.instantiate(id, config, in, out, key, error);
    }
};

struct Options {
    bool list = false, json = false, version = false, bundles_only = false, only_given = false;
    std::string probe, format;
    int shm_fd = -1, clock_fd = -1, control_fd = -1;
    int64_t engine_pid = 0;
    std::vector<std::string> chain, bundles;
    struct Set {
        uint32_t instance;
        std::string symbol;
        float value;
    };
    std::vector<Set> sets;
    uint32_t channels = 0;
    uint32_t sidechain = 0;
    bool reactivate = false;
    // LSP's bundle, 184 classes each instantiated and run, took 19.9 s in
    // the container (brain/research/plugin-host-vst3.md).
    uint32_t scan_timeout_s = 120;
};

[[noreturn]] void usage(const char* why) {
    std::fprintf(stderr,
                 "sonora-plugin-host: %s\n"
                 "usage: sonora-plugin-host --list [--json] [--format lv2|vst3] [--scan-timeout S]\n"
                 "                          [--bundle DIR]... [--bundles-only] [--only-given] [--engine-pid PID]\n"
                 "       sonora-plugin-host --probe ID [--channels N] [--bundle DIR]... [--engine-pid PID]\n"
                 "       sonora-plugin-host --version\n"
                 "       sonora-plugin-host --shm-fd FD --clock-fd FD --chain ID[,ID...] [--bundle DIR]...\n"
                 "                          [--engine-pid PID] [--set INDEX:SYMBOL=VALUE]...\n"
                 "                          [--sidechain N] [--reactivate]\n"
                 "       sonora-plugin-host --control-fd FD --shm-fd FD --clock-fd FD [--bundle DIR]...\n"
                 "                          [--engine-pid PID]\n",
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
        else if (a == "--bundles-only") o.bundles_only = true;
        else if (a == "--only-given") o.only_given = true;
        else if (a == "--format") {
            o.format = next();
            if (o.format != "lv2" && o.format != "vst3") usage("--format is lv2 or vst3");
        }
        else if (a == "--probe") o.probe = next();
        else if (a == "--shm-fd") o.shm_fd = std::atoi(next().c_str());
        else if (a == "--control-fd") o.control_fd = std::atoi(next().c_str());
        else if (a == "--engine-pid") o.engine_pid = std::atoll(next().c_str());
        else if (a == "--chain") o.chain = split(next(), ',');
        else if (a == "--bundle") o.bundles.push_back(next());
        else if (a == "--channels") o.channels = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (a == "--sidechain") o.sidechain = static_cast<uint32_t>(std::atoi(next().c_str()));
        else if (a == "--reactivate") o.reactivate = true;
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
    if (o.control_fd >= 0) {
        if (o.shm_fd < 0) usage("--control-fd needs --shm-fd and --clock-fd");
        if (!o.chain.empty() || !o.sets.empty() || o.sidechain || o.reactivate)
            usage("with --control-fd the chain, --set, --sidechain and --reactivate come in Load");
    } else if (o.shm_fd >= 0 && o.chain.empty()) {
        usage("--shm-fd needs --chain or --control-fd");
    }
    if (o.shm_fd >= 0 && o.clock_fd < 0) usage("--shm-fd needs --clock-fd");
    if ((o.bundles_only || o.only_given) && !o.list) usage("--bundles-only and --only-given are --list's");
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
        std::fprintf(f, "  bundle:   %s\n", info.bundle.c_str());
        if (!info.bridge.empty()) std::fprintf(f, "  bridge:   %s (runs under Wine)\n", info.bridge.c_str());
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
                "\"audio_out\":%u,\"sidechain_in\":%u,\"latency_port\":%d,\"latency_frames\":%lld,"
                "\"usable\":%s",
                json_escape(info.uri).c_str(), json_escape(info.format).c_str(),
                json_escape(info.name).c_str(), json_escape(info.class_label).c_str(), info.audio_in,
                info.audio_out, info.sidechain_in, info.latency_port, static_cast<long long>(info.latency_frames),
                info.missing_features.empty() ? "true" : "false");
    if (!info.missing_features.empty())
        std::fprintf(f, ",\"reason\":\"%s\"", json_escape(info.missing_features.front()).c_str());
    std::fprintf(f, ",\"bundle\":\"%s\"", json_escape(info.bundle).c_str());
    // Only when bridged, so a native record's line is what it always was.
    if (!info.bridge.empty()) std::fprintf(f, ",\"bridge\":\"%s\"", json_escape(info.bridge).c_str());
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
        if (port.sidechain) std::fprintf(f, ",\"sidechain\":true");
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
std::string absolute_bundle(const std::string& path) {
    std::error_code ec;
    std::string b = std::filesystem::absolute(path, ec).lexically_normal().string();
    if (ec) b = path;
    while (b.size() > 1 && b.back() == '/') b.pop_back();
    return b;
}

// --list --bundles-only: what is installed, bundle by bundle, with no plugin
// code run. LV2 from the world lilv loaded (a static description); VST3
// from the paths alone.
int list_bundles(const Catalog& catalog, const Options& o) {
    std::size_t n = 0;
    const auto line = [&](const std::string& bundle, const char* format, const std::string& bridge) {
        if (o.json && bridge.empty())
            std::printf("{\"bundle\":\"%s\",\"format\":\"%s\"}\n", json_escape(bundle).c_str(), format);
        else if (o.json)
            std::printf("{\"bundle\":\"%s\",\"format\":\"%s\",\"bridge\":\"%s\"}\n", json_escape(bundle).c_str(),
                        format, json_escape(bridge).c_str());
        else
            std::printf("%s  %s%s%s\n", format, bundle.c_str(), bridge.empty() ? "" : "  ", bridge.c_str());
        ++n;
    };
    if (o.format.empty() || o.format == "lv2") {
        std::set<std::string> seen;
        for (const LilvPlugin* p : catalog.lv2.all()) {
            const std::string b = catalog.lv2.describe(p).bundle;
            if (!b.empty() && seen.insert(b).second) line(b, "lv2", std::string());
        }
    }
#ifdef SONORA_HAVE_VST3
    if (o.format.empty() || o.format == "vst3") {
        std::set<std::string> seen;
        for (const std::string& b : catalog.vst3.bundles())
            if (seen.insert(absolute_bundle(b)).second)
                line(absolute_bundle(b), "vst3", Vst3World::bridge_of(b));
    }
#endif
    std::fflush(stdout);
    std::fprintf(stderr, "sonora-plugin-host: listed %zu bundle(s)\n", n);
    return 0;
}

int list(const Options& o) {
    Catalog catalog;
    load_bundles(catalog, o);
    if (o.bundles_only) return list_bundles(catalog, o);
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
        std::vector<std::string> scan;
        if (o.only_given) {
            for (const std::string& b : o.bundles)
                if (is_vst3_bundle(b)) scan.push_back(b);
        } else {
            scan = catalog.vst3.bundles();
        }
        for (const std::string& given : scan) {
            const std::string bundle = absolute_bundle(given);
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

// The --version line: the contract versions, and the probe record's.
std::string host_version_line() {
    char line[96];
    pc::format_host_version_line(line, sizeof line);
    return std::string(line) + " probe " + std::to_string(plugin::probe::kProbeRecordVersion);
}

// What the probe record says of one dense parameter (probe_record.h).
plugin::probe::Param probe_param(const Chain& chain, uint32_t d) {
    namespace pr = plugin::probe;
    const PortInfo& port = chain.param_port(0, d);
    pr::Param p;
    p.key = chain.param_key(0, d);
    p.symbol = chain.param_symbol(0, d);
    p.name = port.name;
    chain.param_range(0, d, p.min, p.max);
    p.default_value = chain.param_default(0, d);
    p.unit = port.units;
    p.steps = port.steps == 1 ? 0 : port.steps;
    if (port.steps > 1 && port.labels.size() == port.steps) p.labels = port.labels;
    if (port.automatable) p.flags |= pr::kParamAutomatable;
    if (port.toggle) p.flags |= pr::kParamToggle;
    if (port.enumeration) p.flags |= pr::kParamEnum;
    if (port.logarithmic) p.flags |= pr::kParamLogarithmic;
    // Past the chain's table (protocol.h): not automatable from Sonora,
    // still saved in the plugin's own state.
    if (d >= plugin::kMaxParamsPerChain) {
        p.flags |= pr::kParamPastCap;
        p.flags &= ~uint32_t(pr::kParamAutomatable);
    }
    return p;
}

// --probe: instantiate one plugin, validate it and report its latency at
// 48 kHz / 128, as one probe record (contracts/plugin probe_record.h). Runs
// plugin code, so a scanner runs one probe per process: a plugin that
// crashes on instantiate then costs one entry, not the list.
int probe(const Options& o) {
    namespace pr = plugin::probe;
    if (!die_with_parent(o.engine_pid)) {
        std::fprintf(stderr, "sonora-plugin-host: the engine (pid %lld) is gone; the probe exits\n",
                     static_cast<long long>(o.engine_pid));
        return plugin::kHostExitUsage;
    }
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
        err.reason = plugin::Refusal::NotFound;
    }
    pr::Record r;
    r.host = host_version_line();
    r.uri = o.probe;
    r.format = is_vst3_id(o.probe) ? "vst3" : "lv2";
    r.sample_rate = ChainConfig{}.sample_rate;
    // What it was asked at, so the engine files the answer under its key.
    r.channels = o.channels;
    if (!found) {
        // "not installed" is what the engine shows on the insert.
        r.outcome = err.reason == plugin::Refusal::NotFound ? pr::Outcome::NotInstalled : pr::Outcome::Refused;
        r.reason = r.outcome == pr::Outcome::NotInstalled ? "not installed" : err.message;
        std::printf("%s\n", pr::encode(r).c_str());
        return plugin::kHostExitRefused;
    }
    r.bundle = info.bundle;
    r.name = info.name;
    r.vendor = info.vendor;
    r.version = info.version;
    r.categories = info.categories;
    if (r.categories.size() > pr::kMaxRecordCategories) r.categories.resize(pr::kMaxRecordCategories);
    // No editor is hosted yet (OpenEditor is answered EditorFailed): the
    // record says what this host can open, not what the plugin ships.
    r.has_editor = false;
    r.audio_in = info.audio_in;
    r.audio_out = info.audio_out;
    r.sidechain_in = info.sidechain_in;
    ChainConfig cfg;
    cfg.channels = o.channels ? o.channels : (info.audio_in == info.audio_out && info.audio_in ? info.audio_in : 1);
    r.channels = cfg.channels;
    Chain chain(catalog, cfg);
    const bool added = chain.add(o.probe, err);
    const bool ok = added && chain.validate(err);
    // The dense parameter list the engine's catalog keeps (ADR-0012 §8):
    // the index is the position, the key what the session file stores.
    if (added) {
        r.plugin_bypass = chain.designated_bypass(0);
        for (uint32_t d = 0; d < chain.param_count(0) && d < pr::kMaxRecordParams; ++d)
            r.params.push_back(probe_param(chain, d));
    }
    if (!ok) {
        r.outcome = pr::Outcome::Refused;
        r.reason = err.message.empty() ? "refused" : err.message;
        std::printf("%s\n", pr::encode(r).c_str());
        return plugin::kHostExitRefused;
    }
    r.outcome = pr::Outcome::Ok;
    r.replicas = chain.replicas(0);
    r.latency_frames = chain.instance_latency(0);
    std::printf("%s\n", pr::encode(r).c_str());
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

void publish_latency(plugin::ChannelHeader& h, const Chain& chain, uint32_t ipc_frames) noexcept SONORA_RT {
    for (std::size_t i = 0; i < chain.size(); ++i)
        h.instances[i].latency_frames.store(chain.instance_latency(i), std::memory_order_relaxed);
    // The IPC's own delay is declared like any look-ahead (ADR-0007 §4.3).
    const uint32_t total = chain.total_latency() + ipc_frames;
    if (total != h.latency_frames.load(std::memory_order_relaxed)) {
        h.latency_frames.store(total, std::memory_order_relaxed);
        h.latency_generation.fetch_add(1, std::memory_order_release);
        // The main thread sends LatencyChanged (host_control.h). A wake only
        // on a change, which is rare; never per block.
        notify_control();
    }
}

// Idle wake period in futex mode: bounds how stale the heartbeat can be.
constexpr uint64_t kIdleWaitNs = 100'000'000;
// After a lost touch, how long the gestures it ended get to come back through
// the engine's value table before the plugin's values are compared with it.
constexpr uint64_t kRecoverDelayNs = 100'000'000;
// Timer mode (ADR-0012 §1.3). How long after a callback's start the host
// looks for its submit (placeholder), the step while that callback is late,
// and the sleep while the engine is not calling back at all.
constexpr uint64_t kTimerOffsetNs = 150'000;
constexpr uint64_t kLatePollNs = 50'000;
constexpr uint64_t kStaleSleepNs = 10'000'000;
constexpr uint64_t kParentCheckNs = 100'000'000;
// How long the main thread waits for a plugin's work() before it gives up on
// a restore or a re-activation: the engine's own bound on the answer, less
// this, so a hung worker is named in the answer instead of being found by
// the engine's SIGKILL with no reason given.
constexpr uint64_t kAnswerMarginNs = 1'000'000'000;

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

// Reconfigure's handshake with the processing thread (host_control.h): the
// main thread asks, the processing thread parks at a block boundary and
// says so, and keeps bumping heartbeat until it is let go.
struct Pause {
    std::atomic<bool> request{false};
    std::atomic<uint32_t> parked{0};  // bumped (signal) each time it parks
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
    std::atomic<bool> stop{false};  // the socket's Shutdown, or the engine's end closed
    Pause pause;
    ChainError fault;  // why process_blocks returned kHostExitFaulted
};

constexpr uint64_t kParkedPollNs = 20'000'000;

// One submitted block, from its parameters to its completion. The host's
// side of the audio thread: SONORA_RT, so the RTSan build checks it and the
// plugins it runs, as it checks the engine's callback. The waits between
// blocks are outside it, as they are outside the engine's callback. False
// when a plugin wrote outside its buffers; the caller says so and faults,
// off the RT path, since saying so allocates.
bool run_block(Session& s, uint64_t block, uint32_t frames, ChainError& cerr) noexcept SONORA_RT {
    plugin::ChannelHeader& h = s.h;
    auto deliver = [&](uint32_t i, uint32_t p, uint32_t offset, float v) noexcept SONORA_RT {
        return p == plugin::kBypassParam ? s.chain.set_bypass(i, v > 0.5f, offset)
                                         : s.chain.set_param(i, p, v, offset);
    };
    auto replace = [&](uint32_t i, uint32_t p, float v) noexcept SONORA_RT {
        return p == plugin::kBypassParam ? s.chain.set_bypass(i, v > 0.5f, 0) : s.chain.replace_param(i, p, v);
    };
    // The ring for this block and older ones, then the table if values_seq
    // moved (L1); contracts/plugin, block protocol c.
    plugin::consume_block_params(h, s.table, s.map, s.values, block, frames, deliver, replace);

    const uint64_t t0 = monotonic_ns();
    if (!s.chain.process(s.in.data(), s.out.data(), frames, &h, cerr)) return false;
    const uint64_t took = monotonic_ns() - t0;
    if (took > h.worst_block_ns.load(std::memory_order_relaxed))
        h.worst_block_ns.store(took, std::memory_order_relaxed);
    publish_latency(h, s.chain, s.ipc_frames);

    h.completed_block.store(block, std::memory_order_release);
    signal(h.done_word);
    h.heartbeat.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// The processing thread's loop, at SCHED_FIFO when allowed. Returns the exit
// code: 0 on shutdown or a gone engine, kHostExitFaulted when a plugin faulted.
int process_blocks(Session& s) {
    plugin::ChannelHeader& h = s.h;
    ChainError& cerr = s.fault;
    TimerPacer pacer;
    uint64_t last = h.completed_block.load(std::memory_order_relaxed);
    uint64_t next_parent_check = monotonic_ns() + kParentCheckNs;
    for (;;) {
        if (h.shutdown.load(std::memory_order_acquire) || s.stop.load(std::memory_order_acquire)) break;
        if (s.pause.request.load(std::memory_order_acquire)) {
            // Between blocks: the main thread re-activates the chain now.
            // Blocks submitted meanwhile wait; the engine plays them as
            // misses it does not count (host_control.h, Reconfigure).
            signal(s.pause.parked);
            while (s.pause.request.load(std::memory_order_acquire) &&
                   !h.shutdown.load(std::memory_order_acquire) && !s.stop.load(std::memory_order_acquire)) {
                sleep_until_ns(monotonic_ns() + kParkedPollNs);
                h.heartbeat.fetch_add(1, std::memory_order_relaxed);
            }
            pacer.found();
            continue;
        }
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
            if (!run_block(s, block, s.block_frames, cerr)) {
                cerr.message = "wrote outside its declared buffers while running";
                return refuse(h, cerr, plugin::HostState::Faulted);
            }
            last = block;
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

// The chain to load, from the command line or from Load.
struct ChainSpec {
    std::vector<std::string> ids;
    std::vector<std::vector<std::string>> states;  // per instance: sections, empty for none
    std::vector<pc::StateResult> state_results;
    uint32_t sidechain = 0;
    bool reactivate = false;
};

// Host side of the socket's first exchange (host_control.h, Handshake 2):
// EngineHello read before anything is sent or written. Returns -1 to go on,
// or the exit code.
int read_engine_hello(ControlLink& link) {
    std::string m;
    if (!link.next(m, uint64_t(pc::kHelloTimeoutMs) * 1000000ull)) {
        std::fprintf(stderr, "sonora-plugin-host: no EngineHello from the engine%s%s\n",
                     link.error().empty() ? "" : ": ", link.error().c_str());
        return plugin::kHostExitUsage;
    }
    uint32_t wire = 0;
    if (!pc::decode_wire_version(m, wire) || static_cast<uint8_t>(m[0]) != uint8_t(pc::Type::EngineHello)) {
        std::fprintf(stderr, "sonora-plugin-host: the engine's first message is not an EngineHello\n");
        return plugin::kHostExitUsage;
    }
    if (wire != pc::kHostWireVersion) {
        std::fprintf(stderr,
                     "sonora-plugin-host: incompatible engine: it speaks HostControl wire %u; this host "
                     "speaks wire %u\n",
                     wire, pc::kHostWireVersion);
        return plugin::kHostExitIncompatible;
    }
    pc::EngineHello hello;
    std::string why;
    if (!pc::decode(m, hello, why)) {
        std::fprintf(stderr, "sonora-plugin-host: the engine's EngineHello is malformed: %s\n", why.c_str());
        return plugin::kHostExitUsage;
    }
    if (hello.layout_version != plugin::kLayoutVersion || hello.clock_version != plugin::kClockLayoutVersion) {
        std::fprintf(stderr,
                     "sonora-plugin-host: incompatible engine: it speaks layout %u clock %u; this host "
                     "speaks layout %u clock %u\n",
                     hello.layout_version, hello.clock_version, plugin::kLayoutVersion, plugin::kClockLayoutVersion);
        return plugin::kHostExitIncompatible;
    }
    return -1;
}

// A broken contract from the engine: say so on the socket, then exit 2.
int protocol_error(ControlLink* link, const std::string& why) {
    std::fprintf(stderr, "sonora-plugin-host: HostControl: %s\n", why.c_str());
    if (link) link->send_message(pc::Fault{pc::FaultKind::Protocol, -1, 0, why.substr(0, pc::kMaxTextBytes)});
    return plugin::kHostExitUsage;
}

// Load, checked against the header the engine initialised (host_control.h:
// instances and their dense counts must be the header's).
int read_load(ControlLink& link, const plugin::ChannelHeader& h, uint32_t channels, ChainSpec& spec) {
    std::string m, why;
    if (!link.next(m, uint64_t(pc::kLoadTimeoutMs) * 1000000ull)) {
        std::fprintf(stderr, "sonora-plugin-host: no Load from the engine\n");
        return plugin::kHostExitUsage;
    }
    pc::Load load;
    if (!pc::decode(m, load, why)) return protocol_error(&link, "a malformed Load: " + why);
    if (load.instances.size() != h.instance_count)
        return protocol_error(&link, "Load has " + std::to_string(load.instances.size()) +
                                         " instances; the channel header says " + std::to_string(h.instance_count));
    if (load.key_channels >= channels)
        return protocol_error(&link, "Load asks for " + std::to_string(load.key_channels) +
                                         " key channels of " + std::to_string(channels));
    spec.sidechain = load.key_channels;
    spec.reactivate = (load.flags & pc::kLoadReactivate) != 0;
    for (std::size_t i = 0; i < load.instances.size(); ++i) {
        const pc::LoadInstance& in = load.instances[i];
        if (in.params != h.instance_params[i])
            return protocol_error(&link, "Load gives instance " + std::to_string(i) + " " +
                                             std::to_string(in.params) + " parameters; the header " +
                                             std::to_string(h.instance_params[i]));
        spec.ids.push_back(in.format == pc::Format::Vst3 ? "vst3:" + in.id : in.id);
        std::vector<std::string> sections;
        pc::StateResult result = pc::StateResult::None;
        if (!in.state.empty()) {
            pc::StateBlob blob;
            if (!pc::decode_state_blob(in.state, blob, why)) {
                std::fprintf(stderr, "sonora-plugin-host: instance %zu: a state this host cannot read: %s\n", i,
                             why.c_str());
                result = pc::StateResult::Refused;
            } else if (blob.format != in.format || blob.id != in.id) {
                std::fprintf(stderr, "sonora-plugin-host: instance %zu: the state is %s's, not this plugin's\n",
                             i, blob.id.c_str());
                result = pc::StateResult::Refused;
            } else {
                sections = std::move(blob.sections);
                result = pc::StateResult::Restored;  // unless the plugin refuses it
            }
        }
        spec.states.push_back(std::move(sections));
        spec.state_results.push_back(result);
    }
    return -1;
}

pc::Loaded loaded_message(const plugin::ChannelHeader& h, const Chain& chain, const ChainSpec& spec) {
    pc::Loaded m;
    // Region first, as host_control.h's ordering says: generation, then frames.
    m.latency_generation = h.latency_generation.load(std::memory_order_acquire);
    m.latency_frames = h.latency_frames.load(std::memory_order_relaxed);
    m.host_flags = h.host_flags.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < chain.size(); ++i) {
        pc::LoadedInstance li;
        li.latency_frames = chain.instance_latency(i);
        li.replicas = chain.replicas(i);
        li.state = spec.state_results[i];
        li.bypass = chain.designated_bypass(i) ? pc::BypassBy::Plugin : pc::BypassBy::Host;
        for (uint32_t d = 0; d < chain.param_count(i); ++d) {
            pc::ParamDesc p;
            p.key = chain.param_key(i, d);
            chain.param_range(i, d, p.min, p.max);
            p.default_value = chain.param_default(i, d);
            if (!std::isfinite(p.min) || !std::isfinite(p.max)) p.min = p.max = 0.f;
            if (!std::isfinite(p.default_value)) p.default_value = p.min;
            li.params.push_back(std::move(p));
        }
        m.instances.push_back(std::move(li));
    }
    return m;
}

int serve(const Options& o) {
    if (!die_with_parent(o.engine_pid)) {
        std::fprintf(stderr, "sonora-plugin-host: the engine (pid %lld) is not this host's parent; "
                             "it is gone, so this host exits\n",
                     static_cast<long long>(o.engine_pid));
        return plugin::kHostExitUsage;
    }
    std::unique_ptr<ControlLink> link;
    std::string err;
    if (o.control_fd >= 0) {
        link = std::make_unique<ControlLink>();
        if (!link->open(o.control_fd, err)) {
            std::fprintf(stderr, "sonora-plugin-host: --control-fd: %s\n", err.c_str());
            return plugin::kHostExitUsage;
        }
        // The wire version before anything else: a mismatch exits 3 having
        // sent nothing and written nothing.
        if (const int rc = read_engine_hello(*link); rc >= 0) return rc;
    }
    SharedRegion region, clock_region;
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
    const std::size_t want_instances = link ? h.instance_count : o.chain.size();
    if (!plugin::header_valid(h, region.size) || h.instance_count != want_instances ||
        !plugin::clock_valid(clock, clock_region.size) || !plugin::channel_matches_clock(h, clock)) {
        std::fprintf(stderr, "sonora-plugin-host: the channel or clock header is not valid for a chain of %zu\n",
                     want_instances);
        return plugin::kHostExitUsage;
    }
    // Taken once from the validated headers. Nothing below re-reads the
    // fixed parts, which a plugin in this process could overwrite.
    const plugin::Layout layout = plugin::layout_of(h);
    const plugin::ParamMap map = plugin::param_map_of(h);
    const auto mode = static_cast<plugin::WakeMode>(clock.wake_mode);
    const uint64_t period_ns = clock.period_ns;
    const uint32_t ipc_frames = plugin::kPipelineDepth * h.block_frames;

    ChainSpec spec;
    if (link) {
        // The host's first datagram (Handshake 3).
        pc::Hello hello;
        hello.pid = static_cast<uint64_t>(getpid());
        if (!link->send_message(hello)) {
            std::fprintf(stderr, "sonora-plugin-host: cannot send Hello: %s\n", link->error().c_str());
            return plugin::kHostExitUsage;
        }
        if (const int rc = read_load(*link, h, layout.channels, spec); rc >= 0) return rc;
    } else {
        spec.ids = o.chain;
        spec.states.resize(o.chain.size());
        spec.state_results.assign(o.chain.size(), pc::StateResult::None);
        spec.sidechain = o.sidechain;
        spec.reactivate = o.reactivate;
    }

    // A refusal goes to the region first, then (with a socket) the message.
    auto refused = [&](const ChainError& e) {
        const int rc = refuse(h, e, plugin::HostState::Refused);
        if (link) link->send_message(pc::Refused{e.reason, e.instance, e.message.substr(0, pc::kMaxTextBytes)});
        return rc;
    };

    // Loading runs at normal priority: plugins read files and allocate when
    // they are instantiated, and that must not happen at SCHED_FIFO.
    const uint64_t load_deadline = monotonic_ns() + uint64_t(pc::kLoadTimeoutMs) * 1'000'000ull - kAnswerMarginNs;
    Catalog catalog;
    load_bundles(catalog, o);
    ChainConfig cfg;
    cfg.sample_rate = h.sample_rate;
    cfg.max_frames = layout.max_frames;
    cfg.block_frames = h.block_frames;
    if (spec.sidechain >= layout.channels) {
        std::fprintf(stderr, "sonora-plugin-host: --sidechain %u leaves no signal channel of %u\n",
                     spec.sidechain, layout.channels);
        return plugin::kHostExitUsage;
    }
    cfg.channels = layout.channels - spec.sidechain;
    cfg.sidechain_channels = spec.sidechain;
    const int rt_priority = h.host_rt_priority;
    Chain chain(catalog, cfg);
    ChainError cerr;
    for (const std::string& id : spec.ids)
        if (!chain.add(id, cerr)) return refused(cerr);
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
            return refused(cerr);
        }
    // State before the table: the table's values win over the same
    // parameters in a state (host_control.h, ordering). A state the plugin
    // refuses is reported, not a refusal of the chain.
    for (std::size_t i = 0; i < chain.size(); ++i) {
        if (spec.states[i].empty()) continue;
        std::string why;
        if (!chain.restore_state(i, spec.states[i], load_deadline, why)) {
            std::fprintf(stderr, "sonora-plugin-host: instance %zu: state refused: %s\n", i, why.c_str());
            spec.state_results[i] = pc::StateResult::Refused;
        }
    }

    // The values the control thread seeded (contracts/plugin, §5.1), before
    // validation, so the latency reported is the one of these values.
    Session s{h, clock, chain, layout, map, mode, period_ns, plugin::value_table(region.base, layout),
              {}, {}, {}, ipc_frames, o.engine_pid, h.block_frames};
    // Atomic: the control thread's sweep after a lost touch reads it too.
    std::vector<std::atomic<float>> applied(map.param_slots);
    s.values.applied = applied.data();
    s.values.seen_seq = h.values_seq.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < map.instance_count; ++i)
        for (uint32_t k = 0; k < map.count[i]; ++k) {
            const int64_t slot = plugin::param_slot(map, i, k);
            if (slot < 0) break;
            const float v = s.table[slot].load(std::memory_order_relaxed);
            applied[std::size_t(slot)].store(v, std::memory_order_relaxed);
            if (plugin::finite_bits(v)) chain.replace_param(i, k, v);
        }
    for (const Options::Set& set : o.sets)
        if (!chain.set_control(set.instance, set.symbol, set.value))
            std::fprintf(stderr, "sonora-plugin-host: --set %u:%s: no such input control\n", set.instance,
                         set.symbol.c_str());
    if (!chain.validate(cerr)) return refused(cerr);
    if (spec.reactivate) {
        if (!chain.reactivate(load_deadline, cerr) || !chain.validate(cerr)) return refused(cerr);
    }
    // After any re-activation: the delay lines are sized from the latency
    // the plugins report now.
    chain.prepare_bypass();
    for (uint32_t i = 0; i < map.instance_count; ++i) {
        const float b = h.bypass[i].load(std::memory_order_relaxed);
        s.values.applied_bypass[i].store(b, std::memory_order_relaxed);
        if (b > 0.5f) chain.set_bypass(i, true, 0);
    }
    for (std::size_t i = 0; i < chain.size(); ++i)
        h.instances[i].replicas.store(chain.replicas(i), std::memory_order_relaxed);
    publish_latency(h, chain, ipc_frames);

    // The engine receives at most kMaxHostMessageBytes per message (M1 on
    // PR #51). Key tables that would not fit refuse the chain here, before
    // Ready, rather than make the engine refuse a Loaded.
    if (link) {
        std::string bytes;
        pc::encode(loaded_message(h, chain, spec), bytes);
        if (bytes.size() > pc::kMaxHostMessageBytes) {
            cerr.reason = plugin::Refusal::UnsupportedFeature;
            cerr.instance = -1;
            cerr.message = "the chain's parameter tables (" + std::to_string(bytes.size()) +
                           " bytes) are too large for HostControl's " +
                           std::to_string(pc::kMaxHostMessageBytes) + "-byte cap";
            return refused(cerr);
        }
    }

    // Touches from here on; held until Loaded is sent.
    TouchQueue touches;
    if (link) chain.set_touch_sink(&touches);

    uint32_t flags = 0;
    // Process-wide, so it covers the processing thread's stack too.
    if (lock_memory()) flags |= plugin::kHostMemoryLocked;

    // Signal and key in, signal out. The key channels' output planes are
    // never written: the engine zeroed the region.
    s.in.resize(layout.channels);
    s.out.resize(cfg.channels);
    for (uint32_t c = 0; c < layout.channels; ++c) s.in[c] = plugin::input_plane(region.base, layout, c);
    for (uint32_t c = 0; c < cfg.channels; ++c) s.out[c] = plugin::output_plane(region.base, layout, c);

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

    if (!link) {
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

    // --- HostControl (contracts/plugin host_control.h).
    // Loaded only once the region says Ready (ordering rule 1).
    for (;;) {
        const uint32_t seen = h.done_word.load(std::memory_order_acquire);
        if (plugin::checked_state(h.state.load(std::memory_order_acquire)) != plugin::HostState::Starting ||
            finished.load(std::memory_order_acquire))
            break;
        wait_for_change(h.done_word, seen, kIdleWaitNs);
    }
    bool engine_gone = !link->send_message(loaded_message(h, chain, spec));
    uint32_t latency_sent = h.latency_generation.load(std::memory_order_acquire);
    uint64_t lost_seen = 0;
    // A counted fault's total already said, and when last (host_control.h:
    // the first at once, the rest summed at most once per interval).
    struct Said {
        uint64_t sent = 0, at = 0;
    };
    Said lost_said;
    std::vector<Said> refused_said(chain.size()), unmapped_said(chain.size());
    // The plugin's value now, on this (its main) thread, for a gesture
    // ended without its own end.
    auto current = [&](uint32_t instance, uint32_t param, float& v) {
        return chain.current_touch_value(instance, param, v);
    };
    GestureTracker gestures;
    // After a drop, the plugin's values against the engine's last ones, once
    // the gestures end_all ended have had time to come back through the
    // table (a few blocks), so they are not recorded twice.
    uint64_t recover_due = 0;
    std::vector<TouchQueue::Event> events;
    int protocol_exit = -1;
    auto send = [&](const std::string& bytes) {
        if (!engine_gone && !link->send(bytes)) engine_gone = true;
    };
    auto send_fault = [&](pc::FaultKind kind, int32_t instance, uint64_t count, const std::string& text) {
        std::string b;
        pc::encode(pc::Fault{kind, instance, count, text.substr(0, pc::kMaxTextBytes)}, b);
        send(b);
    };
    // now == 0: the host is stopping, so whatever is left goes at once.
    auto say_counted = [&](pc::FaultKind kind, int32_t instance, uint64_t total, Said& said, uint64_t now,
                           const char* text) {
        if (total == said.sent) return;
        if (now != 0 && said.at != 0 && now - said.at < uint64_t(pc::kCountedFaultIntervalMs) * 1'000'000ull)
            return;
        send_fault(kind, instance, total - said.sent, text);
        said.sent = total;
        said.at = now;
    };
    auto say_counts = [&](uint64_t now) {
        say_counted(pc::FaultKind::EventsDropped, -1, touches.lost(), lost_said, now,
                    "parameter touches that could not be queued");
        for (std::size_t i = 0; i < chain.size(); ++i) {
            say_counted(pc::FaultKind::WorkRefused, static_cast<int32_t>(i), chain.work_refused(i),
                        refused_said[i], now, "worker messages refused because the plugin's worker queue was full");
            say_counted(pc::FaultKind::ParamUnmapped, static_cast<int32_t>(i), touches.unmapped(uint32_t(i)),
                        unmapped_said[i], now, "touches of a parameter with no dense index (read-only or unknown)");
        }
    };
    auto recover = [&] {
        const auto count = [&](uint32_t i) { return i < map.instance_count ? map.count[i] : 0u; };
        // What was applied is read before the table: a change applied
        // between the two reads then shows as in flight, never as settled
        // at a value the plugin has not been given.
        const auto engine = [&](uint32_t i, uint32_t p, float& table, float& applied) {
            if (p == plugin::kBypassParam) {
                applied = s.values.applied_bypass[i].load(std::memory_order_acquire);
                table = h.bypass[i].load(std::memory_order_relaxed);
                return true;
            }
            const int64_t slot = plugin::param_slot(map, i, p);
            if (slot < 0) return false;
            applied = s.values.applied[slot].load(std::memory_order_acquire);
            table = s.table[slot].load(std::memory_order_relaxed);
            return true;
        };
        gestures.recover(uint32_t(chain.size()), count, engine, current, send);
        recover_due = 0;
    };
    auto stop_processing = [&] {
        s.stop.store(true, std::memory_order_release);
        while (!finished.load(std::memory_order_acquire)) {
            const uint32_t seen = control_word().load(std::memory_order_acquire);
            if (finished.load(std::memory_order_acquire)) break;
            wait_for_change(control_word(), seen, kIdleWaitNs);
        }
    };
    // Reconfigure: park the processing thread, re-activate and validate on
    // this thread, re-apply bypass (prepare_bypass resets it), publish.
    auto reconfigure = [&](const pc::Reconfigure& r) {
        const uint32_t parked = s.pause.parked.load(std::memory_order_acquire);
        s.pause.request.store(true, std::memory_order_release);
        const uint64_t end = monotonic_ns() + uint64_t(pc::kReconfigureTimeoutMs) * 1000000ull;
        while (s.pause.parked.load(std::memory_order_acquire) == parked &&
               !finished.load(std::memory_order_acquire) && monotonic_ns() < end)
            wait_for_change(s.pause.parked, parked, 50'000'000);
        if (s.pause.parked.load(std::memory_order_acquire) == parked) {
            // Finished, or stuck inside a plugin: the engine's timeout and
            // the pidfd decide what happens next.
            s.pause.request.store(false, std::memory_order_release);
            return;
        }
        ChainError e;
        if (!chain.reactivate(end - kAnswerMarginNs, e) || !chain.validate(e)) {
            // A plugin that will not come back must not run inactive.
            refuse(h, e, plugin::HostState::Faulted);
            send_fault(pc::FaultKind::Plugin, e.instance, 0, e.message);
            s.stop.store(true, std::memory_order_release);
            s.pause.request.store(false, std::memory_order_release);
            protocol_exit = plugin::kHostExitFaulted;
            return;
        }
        chain.prepare_bypass();
        for (uint32_t i = 0; i < map.instance_count; ++i)
            if (s.values.applied_bypass[i].load(std::memory_order_relaxed) > 0.5f) chain.set_bypass(i, true, 0);
        publish_latency(h, chain, ipc_frames);
        const uint32_t gen = h.latency_generation.load(std::memory_order_acquire);
        const uint32_t frames = h.latency_frames.load(std::memory_order_relaxed);
        s.pause.request.store(false, std::memory_order_release);
        latency_sent = gen;
        std::string b;
        pc::encode(pc::Reconfigured{r.request, gen, frames}, b);
        send(b);
    };
    auto handle = [&](const std::string& m) {
        pc::Type t;
        std::string why;
        if (!pc::peek_type(m, t) || !pc::engine_to_host(t)) {
            protocol_exit = protocol_error(link.get(), "a message type the engine does not send");
            return;
        }
        std::string out;
        switch (t) {
        case pc::Type::GetState: {
            pc::GetState g;
            if (!pc::decode(m, g, why)) break;
            pc::State reply{g.request, g.instance, pc::StateStatus::Failed, {}, {}};
            std::vector<std::string> sections;
            if (g.instance >= chain.size()) {
                reply.text = "no instance " + std::to_string(g.instance);
            } else {
                switch (chain.save_state(g.instance, sections, reply.text)) {
                case SaveResult::Empty: reply.status = pc::StateStatus::Empty; break;
                case SaveResult::Failed: break;
                case SaveResult::Saved: {
                    const bool vst3 = spec.ids[g.instance].rfind("vst3:", 0) == 0;
                    const pc::StateBlob blob{vst3 ? pc::Format::Vst3 : pc::Format::Lv2,
                                             vst3 ? spec.ids[g.instance].substr(5) : spec.ids[g.instance],
                                             std::move(sections)};
                    if (pc::encode_state_blob(blob, reply.bytes, reply.text)) reply.status = pc::StateStatus::Ok;
                    break;
                }
                }
            }
            reply.text = reply.text.substr(0, pc::kMaxTextBytes);
            pc::encode(reply, out);
            send(out);
            return;
        }
        case pc::Type::DescribeValue: {
            pc::DescribeValue d;
            if (!pc::decode(m, d, why)) break;
            pc::ValueText reply{d.request, false, {}};
            if (d.instance < chain.size() && d.param < chain.param_count(d.instance))
                reply.ok = chain.describe_value(d.instance, d.param, d.value, reply.text);
            if (!reply.ok) reply.text = "no such parameter";
            reply.text = reply.text.substr(0, pc::kMaxValueTextBytes);
            pc::encode(reply, out);
            send(out);
            return;
        }
        case pc::Type::OpenEditor: {
            pc::OpenEditor e;
            if (!pc::decode(m, e, why)) break;
            const std::string reason = e.instance < chain.size()
                                           ? "plugin editors are not hosted by this build yet"
                                           : "no instance " + std::to_string(e.instance);
            pc::encode(pc::EditorFailed{e.request, e.instance, reason}, out);
            send(out);
            return;
        }
        case pc::Type::CloseEditor: {
            pc::CloseEditor c;
            if (!pc::decode(m, c, why)) break;
            return;  // no editor is ever open in this build
        }
        case pc::Type::Reconfigure: {
            pc::Reconfigure r;
            if (!pc::decode(m, r, why)) break;
            reconfigure(r);
            return;
        }
        case pc::Type::Shutdown: {
            pc::Shutdown x;
            if (!pc::decode(m, x, why)) break;
            s.stop.store(true, std::memory_order_release);
            return;
        }
        default:
            why = t == pc::Type::Load ? "a second Load" : "a second EngineHello";
            break;
        }
        protocol_exit = protocol_error(link.get(), why.empty() ? "an out-of-place message" : why);
    };

    // The main loop: control() for the plugins, the engine's requests, the
    // touches, and the latency notice; the processing thread and the
    // reader wake it through control_word().
    for (;;) {
        const uint32_t seen = control_word().load(std::memory_order_acquire);
        if (finished.load(std::memory_order_acquire) || protocol_exit >= 0) break;
        chain.control();
        std::string m;
        while (protocol_exit < 0 && link->next(m, 0)) handle(m);
        if (protocol_exit >= 0) break;
        if (link->closed()) {
            // The engine closed its end: a Shutdown. Or it framed wrongly,
            // and cannot be resynchronised: exit 2 (host_control.h).
            if (!link->error().empty()) {
                std::fprintf(stderr, "sonora-plugin-host: HostControl: %s\n", link->error().c_str());
                protocol_exit = plugin::kHostExitUsage;
            }
            engine_gone = true;
        }
        const uint64_t now = monotonic_ns();
        // Read before the drain: a touch lost from here on is seen next pass.
        const uint64_t lost = touches.lost();
        events.clear();
        touches.drain(events);
        for (const auto& e : events) gestures.apply(e, now, send);
        // A lost touch may have been an end: end every open gesture now,
        // with the plugin's current value (host_control.h, M3).
        if (lost != lost_seen) {
            gestures.end_all(current, send);
            lost_seen = lost;
            recover_due = now + kRecoverDelayNs;
        }
        if (recover_due && now >= recover_due) recover();
        uint64_t touch_due = gestures.flush(now, send);
        if (recover_due && (touch_due == 0 || recover_due < touch_due)) touch_due = recover_due;
        // A plugin that refills a full ring, or touches a meter, every block
        // would otherwise send a Fault every pass.
        say_counts(now);
        // Region first: the generation was bumped before this reads it.
        if (const uint32_t gen = h.latency_generation.load(std::memory_order_acquire); gen != latency_sent) {
            std::string b;
            pc::encode(pc::LatencyChanged{gen, h.latency_frames.load(std::memory_order_relaxed)}, b);
            send(b);
            latency_sent = gen;
        }
        if (engine_gone || protocol_exit >= 0 || s.stop.load(std::memory_order_acquire)) break;
        uint64_t wait = kIdleWaitNs;
        if (touch_due) wait = touch_due > now ? std::min(wait, touch_due - now) : 0;
        if (wait) wait_for_change(control_word(), seen, wait);
    }
    // Before stopping: every open gesture ends (host_control.h), then the
    // processing thread, then a fault's text if a plugin faulted.
    events.clear();
    touches.drain(events);
    for (const auto& e : events) gestures.apply(e, monotonic_ns(), send);
    gestures.end_all(current, send);
    if (recover_due || touches.lost() != lost_seen) recover();
    stop_processing();
    join_thread(processing);
    // After the last run(): no schedule or touch can come later and go unsaid.
    say_counts(0);
    if (exit_code == plugin::kHostExitFaulted) send_fault(pc::FaultKind::Plugin, s.fault.instance, 0, s.fault.message);
    link->close();
    return protocol_exit >= 0 ? protocol_exit : exit_code;
}

}  // namespace

int main(int argc, char** argv) {
    const Options o = parse(argc, argv);
    if (o.version) {
        // Read by the engine at start-up (ADR-0012 §5.5); a mismatch
        // disables plugin hosting for the run, loudly. The grammar is the
        // contract's (format_host_version_line): layout, clock and wire,
        // then the probe record's version (probe_record.h).
        std::printf("%s\n", host_version_line().c_str());
        return plugin::kHostExitClean;
    }
    // Before any plugin code loads, and inherited by the --list scan children.
    no_core_dumps();
    if (o.list) {
        if (!die_with_parent(o.engine_pid)) {
            std::fprintf(stderr, "sonora-plugin-host: the engine (pid %lld) is gone; the scan exits\n",
                         static_cast<long long>(o.engine_pid));
            return plugin::kHostExitUsage;
        }
        return list(o);
    }
    if (!o.probe.empty()) return probe(o);
    return serve(o);
}
