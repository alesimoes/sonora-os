// SPDX-License-Identifier: GPL-3.0-or-later
//
// A stand-in for sonora-plugin-host's scan modes, for the scan worker's
// tests (test_plugin_scanner): --version, --list --json (--bundles-only,
// --format, --only-given) and --probe, answering from a world file instead
// of lilv, so every rule is testable where no plugin SDK is installed.
//
// SONORA_FAKE_HOST_WORLD names the world, one line each:
//   version <the whole --version line>
//   plugin <uri> <bundle> <lv2|vst3> <in> <out> <behaviour> [ms]
//   listfail <bundle> <reason...>          (a VST3 bundle whose scan crashed)
// behaviours: ok, refuse, crash, hang, sleep (then ok after `ms`).
// SONORA_FAKE_HOST_LOG, if set, gets one line per run, appended:
//   <mode> <args...> pid=<pid> nice=<n> io=<class>
// which is how the tests count probes and check their priority.
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <sys/resource.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "sonora/plugin/probe_record.h"

namespace pr = sonora::plugin::probe;

namespace {

struct Plugin {
    std::string uri, bundle, format, behaviour;
    uint32_t in = 1, out = 1, ms = 0;
};

struct World {
    std::string version = "sonora-plugin-host layout 1 clock 1 wire 1 probe 1";
    std::vector<Plugin> plugins;
    std::vector<std::pair<std::string, std::string>> listfail;
};

World read_world() {
    World w;
    const char* path = std::getenv("SONORA_FAKE_HOST_WORLD");
    if (!path) return w;
    std::ifstream f(path);
    for (std::string line; std::getline(f, line);) {
        std::istringstream in(line);
        std::string kind;
        in >> kind;
        if (kind == "version") {
            std::getline(in, w.version);
            while (!w.version.empty() && w.version.front() == ' ') w.version.erase(0, 1);
        } else if (kind == "plugin") {
            Plugin p;
            in >> p.uri >> p.bundle >> p.format >> p.in >> p.out >> p.behaviour >> p.ms;
            w.plugins.push_back(p);
        } else if (kind == "listfail") {
            std::string b, reason;
            in >> b;
            std::getline(in, reason);
            while (!reason.empty() && reason.front() == ' ') reason.erase(0, 1);
            w.listfail.emplace_back(b, reason);
        }
    }
    return w;
}

void log_run(int argc, char** argv) {
    const char* path = std::getenv("SONORA_FAKE_HOST_LOG");
    if (!path) return;
    std::string line;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--engine-pid") {
            ++i;
            continue;
        }
        line += (line.empty() ? "" : " ") + a;
    }
    int io = -1;
#ifdef __linux__
    io = int(::syscall(SYS_ioprio_get, 1, 0) >> 13);
#endif
    line += " pid=" + std::to_string(::getpid()) + " nice=" + std::to_string(::getpriority(PRIO_PROCESS, 0)) +
            " io=" + std::to_string(io) + "\n";
    // One write, O_APPEND: lines from concurrent probes do not interleave.
    if (std::FILE* f = std::fopen(path, "a")) {
        std::fputs(line.c_str(), f);
        std::fclose(f);
    }
}

std::string esc(const std::string& s) {
    std::string o;
    for (char c : s) o += (c == '"' || c == '\\') ? std::string("\\") + c : std::string(1, c);
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    // A "crash" below must not leave a core file in the build tree.
    struct rlimit no_core {};
    ::setrlimit(RLIMIT_CORE, &no_core);
    log_run(argc, argv);
    const World w = read_world();
    bool list = false, bundles_only = false, only_given = false, version = false;
    std::string format, probe;
    uint32_t channels = 0;
    std::set<std::string> given;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--version") version = true;
        else if (a == "--list") list = true;
        else if (a == "--bundles-only") bundles_only = true;
        else if (a == "--only-given") only_given = true;
        else if (a == "--format" && i + 1 < argc) format = argv[++i];
        else if (a == "--probe" && i + 1 < argc) probe = argv[++i];
        else if (a == "--channels" && i + 1 < argc) channels = uint32_t(std::atoi(argv[++i]));
        else if (a == "--bundle" && i + 1 < argc) given.insert(argv[++i]);
        else if (a == "--engine-pid" && i + 1 < argc) ++i;
    }
    if (version) {
        std::printf("%s\n", w.version.c_str());
        return 0;
    }
    if (list) {
        std::set<std::string> seen;
        for (const Plugin& p : w.plugins) {
            if (!format.empty() && p.format != format) continue;
            if (only_given && p.format == "vst3" && !given.count(p.bundle)) continue;
            if (bundles_only) {
                if (seen.insert(p.bundle).second)
                    std::printf("{\"bundle\":\"%s\",\"format\":\"%s\"}\n", esc(p.bundle).c_str(), p.format.c_str());
                continue;
            }
            bool failed = false;
            for (const auto& [b, why] : w.listfail) failed = failed || b == p.bundle;
            if (failed) continue;
            std::printf("{\"uri\":\"%s\",\"format\":\"%s\",\"name\":\"%s\",\"audio_in\":%u,\"audio_out\":%u,"
                        "\"usable\":true,\"bundle\":\"%s\",\"ports\":[]}\n",
                        esc(p.uri).c_str(), p.format.c_str(), esc(p.uri).c_str(), p.in, p.out, esc(p.bundle).c_str());
        }
        if (!bundles_only && (format.empty() || format == "vst3"))
            for (const auto& [b, why] : w.listfail)
                if (!only_given || given.count(b))
                    std::printf("{\"bundle\":\"%s\",\"format\":\"vst3\",\"usable\":false,\"reason\":\"%s\"}\n",
                                esc(b).c_str(), esc(why).c_str());
        return 0;
    }
    if (probe.empty()) return 2;
    pr::Record r;
    r.host = w.version;
    r.uri = probe;
    r.format = probe.rfind("vst3:", 0) == 0 ? "vst3" : "lv2";
    r.channels = channels;
    r.sample_rate = 48000;
    const Plugin* found = nullptr;
    for (const Plugin& p : w.plugins)
        if (p.uri == probe) found = &p;
    if (!found) {
        r.outcome = pr::Outcome::NotInstalled;
        r.reason = "not installed";
        std::printf("%s\n", pr::encode(r).c_str());
        return 5;
    }
    r.bundle = found->bundle;
    r.name = "Fake " + found->uri;
    r.vendor = "Sonora tests";
    r.version = "1.0";
    r.categories = {"Utility"};
    r.audio_in = found->in;
    r.audio_out = found->out;
    pr::Param g;
    g.key = "gain";
    g.symbol = "gain";
    g.name = "Gain";
    g.min = -90.f;
    g.max = 24.f;
    g.default_value = 0.f;
    g.flags = pr::kParamAutomatable;
    g.unit = "dB";
    r.params = {g};
    // SIGKILL, not SIGSEGV: a sanitizer build catches SIGSEGV and exits
    // with a code instead, which is not what a crashing plugin looks like.
    if (found->behaviour == "crash") ::kill(::getpid(), SIGKILL);
    if (found->behaviour == "hang")
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    if (found->behaviour == "sleep") std::this_thread::sleep_for(std::chrono::milliseconds(found->ms));
    if (found->behaviour == "refuse") {
        r.outcome = pr::Outcome::Refused;
        r.reason = "it wrote outside its buffers";
        std::printf("%s\n", pr::encode(r).c_str());
        return 5;
    }
    const bool fits = (r.audio_in == channels && r.audio_out == channels) || (r.audio_in == 1 && r.audio_out == 1);
    if (!fits) {
        r.outcome = pr::Outcome::Refused;
        r.reason = "it declares " + std::to_string(r.audio_in) + " in / " + std::to_string(r.audio_out) +
                   " out; the chain carries " + std::to_string(channels) + " channels";
        std::printf("%s\n", pr::encode(r).c_str());
        return 5;
    }
    r.outcome = pr::Outcome::Ok;
    r.replicas = r.audio_in == channels ? 1 : channels;
    std::printf("%s\n", pr::encode(r).c_str());
    return 0;
}
