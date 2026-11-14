// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-graph-bench (ADR-0173): Session::process() at quantum 128 over 18
// tracks of 4-plugin chains, beside a 2048-frame chain and a dry track.
//
//   sonora-graph-bench --fake [--callbacks N] [--quantum 128] [--rate 48000]
//                      [--strict-time] [--inject-overrun K]
//   sonora-graph-bench --host PATH --bundle DIR... --chain URI x4
//                      [--aligned URI[:symbol=value]]... [options above]
//
// --fake runs the chains in-process (FakeChainSource): the smoke test's
// mode, deterministic counts. Without it every chain is a real
// sonora-plugin-host, and the driver paces the callbacks at the period, as
// a backend would. The driver times each process() call into an array
// sized before the first one; process() itself reads no clock for it.
// Prints one RESULT line (bench_report.h) and exits 0 on PASS, 1 on FAIL,
// 2 on a usage or setup error, 77 when a plugin of a real run is missing.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "bench_host.h"
#include "bench_report.h"
#include "bench_session.h"
#include "heap_counter.h"

using namespace sonora::bench;

namespace {

// ctest's SKIP_RETURN_CODE: a plugin the real run needs is not installed.
constexpr int kSkip = 77;

struct Options {
    BenchConfig cfg;
    bool fake = false;
    bool strict_time = false;
    HostArgs host;
};

uint64_t steady_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

// "URI" or "URI:symbol=value": a plugin and one parameter override.
HostPlugin parse_plugin(const std::string& s) {
    HostPlugin p;
    const std::size_t eq = s.rfind('=');
    const std::size_t colon = eq == std::string::npos ? std::string::npos : s.rfind(':', eq);
    if (colon == std::string::npos) {
        p.id = s;
        return p;
    }
    p.id = s.substr(0, colon);
    p.set.push_back({s.substr(colon + 1, eq - colon - 1), std::strtof(s.c_str() + eq + 1, nullptr)});
    return p;
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        const bool has_value = i + 1 < argc;
        if (k == "--fake") o.fake = true;
        else if (k == "--strict-time") o.strict_time = true;
        else if (k == "--callbacks" && has_value) o.cfg.callbacks = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        else if (k == "--quantum" && has_value) o.cfg.quantum = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        else if (k == "--rate" && has_value) o.cfg.rate = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        else if (k == "--inject-overrun" && has_value) o.cfg.inject_overrun_at = std::strtoll(argv[++i], nullptr, 10);
        else if (k == "--host" && has_value) o.host.host = argv[++i];
        else if (k == "--bundle" && has_value) o.host.bundles.push_back(argv[++i]);
        else if (k == "--chain" && has_value) o.host.load_chain.push_back(parse_plugin(argv[++i]));
        else if (k == "--aligned" && has_value) o.host.aligned_chain.push_back(parse_plugin(argv[++i]));
        else {
            std::fprintf(stderr, "unknown or incomplete argument %s\n", k.c_str());
            return false;
        }
    }
    if (o.cfg.quantum == 0 || o.cfg.rate == 0 || o.cfg.quantum > 1024) {
        std::fprintf(stderr, "--quantum must be 1..1024 and --rate nonzero\n");
        return false;
    }
    if (!o.fake && (o.host.host.empty() || o.host.load_chain.empty())) {
        std::fprintf(stderr, "a real run needs --host and --chain (or --fake)\n");
        return false;
    }
    if (!o.fake && o.cfg.inject_overrun_at >= 0) {
        std::fprintf(stderr, "--inject-overrun is the fake runner's\n");
        return false;
    }
    if (!o.fake) o.cfg.plugins_per_chain = uint32_t(o.host.load_chain.size());
    return true;
}

// The driver loop: `callbacks` process() calls, each timed into `times`.
// Paced (a real run): each callback starts on its period, as a backend's
// would, so the hosts get the period to finish in; the wait is here,
// between callbacks, never inside one.
void drive(BenchRig& rig, const BenchConfig& cfg, bool paced, std::vector<uint64_t>& times) {
    const FakeClock* fake = cfg.fake_clock;
    const auto now_ns = [fake] { return fake ? fake->ns : steady_ns(); };
    const uint64_t period = cfg.budget_ns();
    const uint64_t start = now_ns() + period;
    for (uint64_t k = 0; k < times.size(); ++k) {
        const uint64_t due = start + k * period;
        if (paced) {
            const uint64_t now = now_ns();
            if (now < due) std::this_thread::sleep_for(std::chrono::nanoseconds(due - now));
        }
        rig.prepare(paced ? due : now_ns());
        const uint64_t t0 = now_ns();
        rig.process();
        times[k] = now_ns() - t0;
        rig.capture(k);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) {
        std::fprintf(stderr, "usage: %s --fake | --host PATH --bundle DIR --chain URI... [--callbacks N] "
                             "[--quantum 128] [--rate 48000] [--strict-time] [--inject-overrun K]\n",
                     argv[0]);
        return 2;
    }
    FakeClock fake_clock;
    if (o.fake) o.cfg.fake_clock = &fake_clock;
    const BenchConfig& cfg = o.cfg;
    std::unique_ptr<ChainSource> source;
    if (o.fake) {
        source = std::make_unique<FakeChainSource>(cfg);
    } else {
        auto host = std::make_unique<HostChainSource>(cfg, o.host);
        if (!host->ok()) {
            std::fprintf(stderr, "setup: %s\n", host->why().c_str());
            return 2;
        }
        source = std::move(host);
    }
    std::printf("sonora-graph-bench: %u tracks x %u plugins, a %u-frame chain beside a dry track, "
                "quantum %u at %u Hz, %u callbacks, %s\n",
                cfg.load_tracks, cfg.plugins_per_chain, source->latency(cfg.aligned_track()), cfg.quantum, cfg.rate,
                cfg.callbacks, o.fake ? "fake runner" : "real hosts");
    // The rig after the source: its session goes first, while the hosts
    // its callbacks talked to still exist.
    BenchRig rig(cfg, *source);
    if (!rig.ready()) {
        std::fprintf(stderr, "setup: the plan did not publish, or the session could not compensate it\n");
        return 2;
    }
    if (cfg.callbacks < rig.capture_from() + cfg.capture_blocks) {
        std::fprintf(stderr, "--callbacks must be at least %llu to fill the alignment window\n",
                     static_cast<unsigned long long>(rig.capture_from() + cfg.capture_blocks));
        return 2;
    }

    std::vector<uint64_t> times(cfg.callbacks, 0);
    reset_heap_calls();
    drive(rig, cfg, !o.fake, times);

    BenchCounts c;
    c.callbacks = cfg.callbacks;
    c.budget_ns = cfg.budget_ns();
    c.misses = over_budget(times, c.budget_ns);
    c.host_misses = source->host_misses(cfg.callbacks);
    c.drops = rig.drops();
    c.allocs = uint64_t(heap_calls());
    c.compensation = rig.compensation().compensation_frames;
    const BenchRig::Alignment a = rig.alignment();
    c.aligned_compared = a.compared;
    c.lag = a.lag;
    c.align_error = a.error;
    // Bit for bit: both chains of the pair are the identity on this signal.
    const Verdict v = verdict(c, o.strict_time, 0.0);
    std::printf("heap counter: %s\n", heap_counter_scope());
    if (v.exit_code != 0) std::printf("FAIL: %s\n", v.reasons.c_str());
    if (c.misses != 0)
        std::printf("%llu callbacks over the %.1f us budget%s\n", static_cast<unsigned long long>(c.misses),
                    double(c.budget_ns) / 1000.0, o.strict_time ? "" : " (a verdict only under --strict-time)");
    std::printf("%s\n", result_line(c, percentiles(times), v).c_str());
    return v.exit_code;
}
