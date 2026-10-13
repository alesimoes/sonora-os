// SPDX-License-Identifier: GPL-3.0-or-later
//
// Plays the engine's side of the draft plugin boundary (contracts/plugin)
// against a real sonora-plugin-host process and real LV2 and VST3 plugins, until
// engine/plugin_client exists (M4+). Linux-only, like the host.
//
// The "callback" here is a periodic SCHED_FIFO loop (when the container
// allows it) at 128 frames / 48 kHz, the same shape as the measurement
// harness in brain/research/adr-0007-measurements.md. It follows the rules
// the engine will: never write a slot the host may be reading, never wait
// on the host at all, drop and count instead.
//
//   plugin_driver --host EXE --bundle DIR... --scenario NAME [options]
//
// Scenarios: gain, latency, latency_change, crash, deadline, nonfinite,
// refusals (--format lv2|vst3), rtt, params and list_json (VST3).
// --mode pipelined (pipeline_depth 1, the default) or poll (depth 0,
// poll-only). Neither waits on the host; rtt is a measurement and does.
// Exit: 0 pass, 1 fail, 77 a required plugin is not installed (a failure in
// ctest unless SONORA_PLUGIN_TESTS_ALLOW_SKIP is ON).

#include "ipc.h"
#include "sonora/plugin/protocol.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace sonora;
using namespace sonora::plugin_host;

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kBlock = 128;
constexpr uint32_t kMaxFrames = 1024;
constexpr uint64_t kPeriodNs = uint64_t(kBlock) * 1000000000ull / kRate;  // 2 666 666
// The engine's budget for the host: what is left of the period after the
// engine's own work. 75 % is an assumption for this driver, not a decision.
constexpr uint64_t kDeadlineNs = kPeriodNs * 3 / 4;
// A host that misses this many blocks in a row leaves the plan.
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

struct Args {
    std::string host, scenario, mode = "pipelined", format = "lv2";
    std::vector<std::string> bundles, chain;
    uint32_t channels = 1;
    uint64_t blocks = 0;
    // gain: the parameter to move, and whether it takes a linear factor
    // (VST3 AGain, normalised 0..1) rather than dB (LV2 eg-amp).
    uint32_t gain_param = 0;
    bool gain_linear = false;
    uint32_t expect_replicas = 0;
    // latency_change: the parameter of chain instance 0 to move, and to what.
    bool has_change = false;
    uint32_t change_param = 0;
    float change_value = 0.f;
};

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
// The engine's half of one host process.

class Host {
public:
    plugin::ChannelHeader* h = nullptr;
    std::vector<std::string> chain;

    bool start(const Args& a, const std::vector<std::string>& uris, uint32_t channels,
               uint32_t pipeline_depth) {
        chain = uris;
        depth_ = pipeline_depth;
        // The engine's own copy: plane addresses never come from the region.
        layout_ = plugin::make_layout(channels, kMaxFrames);
        std::string err;
        if (!create_region(layout_.region_size, region_, err)) {
            std::printf("  cannot create the channel: %s\n", err.c_str());
            return false;
        }
        std::memset(region_.base, 0, region_.size);
        h = plugin::init_header(region_.base, layout_, kRate, kBlock, pipeline_depth,
                                static_cast<uint32_t>(uris.size()), 70);
        const std::string engine_pid = std::to_string(getpid());
        pid_ = fork();
        if (pid_ < 0) return false;
        if (pid_ == 0) {
            // The descriptor becomes fd 3 in the host, without CLOEXEC.
            if (region_.handle == 3) fcntl(3, F_SETFD, 0);
            else dup2(region_.handle, 3);
            const std::string chain_arg = join(uris, ',');
            std::vector<const char*> argv = {a.host.c_str(), "--shm-fd", "3", "--chain",
                                             chain_arg.c_str(), "--engine-pid", engine_pid.c_str()};
            for (const std::string& b : a.bundles) {
                argv.push_back("--bundle");
                argv.push_back(b.c_str());
            }
            argv.push_back(nullptr);
            execv(a.host.c_str(), const_cast<char* const*>(argv.data()));
            _exit(127);
        }
        return true;
    }

    // Not RT: the engine's control thread waits for the host to come up.
    plugin::HostState wait_ready(uint64_t timeout_ns) {
        const uint64_t end = monotonic_ns() + timeout_ns;
        for (;;) {
            const uint32_t seen = h->done_word.load(std::memory_order_acquire);
            const auto s = plugin::checked_state(h->state.load(std::memory_order_acquire));
            if (s != plugin::HostState::Starting) return s;
            if (!alive() || monotonic_ns() > end) return plugin::HostState::Starting;
            wait_for_change(h->done_word, seen, 20'000'000);
        }
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

    void stop() {
        if (h && !reaped_) {
            h->shutdown.store(1, std::memory_order_release);
            signal(h->request_word);
            const uint64_t end = monotonic_ns() + 2'000'000'000ull;
            while (alive() && monotonic_ns() < end) usleep(5000);
            kill_now();
        }
        release_region(region_);
        h = nullptr;
    }

    ~Host() { stop(); }

    // ---- the RT half: what engine/plugin_client's submit/collect would do.
    // Nothing here waits on the host (protocol.h, block protocol).

    bool idle() const {
        return h->completed_block.load(std::memory_order_acquire) == block_;
    }

    void submit(const float* const* in, uint32_t frames, uint64_t callback) {
        for (uint32_t c = 0; c < layout_.channels; ++c)
            std::memcpy(plugin::input_plane(region_.base, layout_, c), in[c], frames * sizeof(float));
        ++block_;
        submitted_at_ = callback;
        h->submitted_frames.store(frames, std::memory_order_relaxed);
        h->submitted_block.store(block_, std::memory_order_release);
        signal(h->request_word);
    }

    void read_output(float* const* out, uint32_t frames) {
        for (uint32_t c = 0; c < layout_.channels; ++c)
            std::memcpy(out[c], plugin::output_plane(region_.base, layout_, c), frames * sizeof(float));
    }

    // One callback. Returns the callback whose input produced `out`, or -1
    // when nothing was collected (the block is dropped).
    //
    // Depth 1: collect the block submitted in the previous callback if it is
    // already complete, then submit this one. A block that completed later
    // than that is discarded rather than played a block late, which would
    // break the declared latency.
    //
    // Depth 0, poll-only: submit, let the rest of the graph run (here: sleep
    // to `collect_at`, standing for the engine's other tracks), then look
    // once. Nothing waits on the host.
    int64_t run(const float* const* in, float* const* out, uint32_t frames, uint64_t callback,
                uint64_t collect_at) {
        // Still on an older block: never touch its slots.
        if (!idle()) return -1;
        if (depth_ == 1) {
            int64_t got = -1;
            if (block_ > 0 && submitted_at_ + 1 == callback) {
                read_output(out, frames);
                got = int64_t(submitted_at_);
            }
            submit(in, frames, callback);
            return got;
        }
        submit(in, frames, callback);
        sleep_until(collect_at);
        if (!idle()) return -1;
        read_output(out, frames);
        return int64_t(callback);
    }

    // The callback of the most recent submit.
    uint64_t submitted_at() const { return submitted_at_; }

    // MEASUREMENT HARNESS ONLY (scenario rtt): submit, then sleep on
    // done_word until the block is back or the deadline passes, to time the
    // IPC round trip. The engine's audio thread never does this; a timed wait
    // there is exposed to the scheduler tail (5.9 ms late seen, container).
    bool round_trip(const float* const* in, float* const* out, uint32_t frames, uint64_t callback,
                    uint64_t deadline, uint64_t& rtt_ns) {
        if (!idle()) return false;
        const uint64_t t0 = monotonic_ns();
        submit(in, frames, callback);
        for (;;) {
            const uint32_t seen = h->done_word.load(std::memory_order_acquire);
            const bool done = h->completed_block.load(std::memory_order_acquire) == block_;
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
            wait_for_change(h->done_word, seen, deadline - now);
        }
    }

    // round_trip blocks given up because the deadline had passed when the
    // driver looked, whether or not the host had finished.
    uint64_t late() const { return late_; }

private:
    static void sleep_until(uint64_t at_ns) {
        timespec ts{time_t(at_ns / 1000000000ull), long(at_ns % 1000000000ull)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
        }
    }

    uint64_t late_ = 0;
    SharedRegion region_;
    plugin::Layout layout_;
    pid_t pid_ = -1;
    int status_ = 0;
    bool reaped_ = false;
    uint32_t depth_ = plugin::kDefaultPipelineDepth;
    uint64_t block_ = 0;
    uint64_t submitted_at_ = 0;
};

// The periodic "audio callback". Measures how long each callback ran from
// its wake-up, which is the number that must stay under the period whatever
// the host does.
struct LoopStats {
    uint64_t callbacks = 0, worst_callback_ns = 0, over_period = 0;
};

template <class F>
LoopStats run_periodic(uint64_t blocks, F&& callback) {
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
        const uint64_t deadline = woke + kDeadlineNs;
        const bool keep_going = callback(i, deadline);
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

// Start a host and wait for it. Returns kSkip when a plugin is not
// installed; ctest treats that as a failure unless skips were allowed
// (SONORA_PLUGIN_TESTS_ALLOW_SKIP), because CI must not go green without
// having run the test.
int start_ready(Host& host, const Args& a, const std::vector<std::string>& chain,
                uint32_t channels, uint32_t depth) {
    if (!host.start(a, chain, channels, depth)) return 1;
    const plugin::HostState s = host.wait_ready(30'000'000'000ull);
    if (s == plugin::HostState::Ready) {
        const uint32_t flags = host.h->host_flags.load();
        std::printf("  host ready: depth %u, chain latency %u frames, host %s, memory %s, %s\n", depth,
                    plugin::checked_latency(host.h->latency_frames.load()),
                    (flags & plugin::kHostRealtime) ? "SCHED_FIFO 70" : "SCHED_OTHER (no RT allowed)",
                    (flags & plugin::kHostMemoryLocked) ? "locked" : "not locked",
                    (flags & plugin::kHostFlushDenormals) ? "FTZ/DAZ" : "denormals not flushed");
        for (std::size_t i = 0; i < chain.size(); ++i)
            std::printf("    [%zu] %s: latency %u, replicas %u\n", i, chain[i].c_str(),
                        plugin::checked_latency(host.h->instances[i].latency_frames.load()),
                        host.h->instances[i].replicas.load());
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

void report_loop(const LoopStats& st) {
    std::printf("  driver: %llu callbacks, worst callback %.1f us, %llu over the %.0f us period\n",
                (unsigned long long)st.callbacks, st.worst_callback_ns / 1e3,
                (unsigned long long)st.over_period, kPeriodNs / 1e3);
}

uint32_t depth_of(const Args& a) { return a.mode == "poll" ? 0 : 1; }

const char* chain_name(const std::vector<std::string>& chain, int32_t i) {
    return i >= 0 && i < int32_t(chain.size()) ? chain[size_t(i)].c_str() : "?";
}

// ---------------------------------------------------------------------------
// Scenarios

// A gain plugin changes the level by exactly what was asked, on every
// channel, and a parameter pushed through the ring in callback p lands in
// the first block submitted at or after p, not before and not later.
int scenario_gain(const Args& a) {
    const std::vector<std::string> chain = a.chain.empty() ? std::vector<std::string>{kEgAmp} : a.chain;
    std::printf("gain: %s, %u channel(s), %s\n", join(chain, ',').c_str(), a.channels, a.mode.c_str());
    Host host;
    if (int r = start_ready(host, a, chain, a.channels, depth_of(a))) return r;
    if (a.channels > 1) {
        const uint32_t want = a.expect_replicas ? a.expect_replicas : a.channels;
        check(host.h->instances[0].replicas.load() == want,
              want == 1 ? "the plugin took the chain's arrangement: one instance"
                        : "a mono plugin is replicated per channel");
    }

    // eg-amp: port 0 is "gain" in dB (lv2info http://lv2plug.in/plugins/eg-amp).
    // AGain (VST3): ParamID 0 is a linear factor, normalised 0..1, so it can
    // only cut. Neither smooths, so every block shows its value.
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
    const LoopStats st = run_periodic(total, [&](uint64_t i, uint64_t deadline) {
        int p = 0;
        for (int k = 0; k < 3; ++k)
            if (i >= phases[k].from) p = k;
        if (i == phases[p].from && i > 0)
            plugin::push_param(*host.h, {0, a.gain_param, value_of(phases[p].db), 0});
        sig.sine(i, 997.0, 0.1f);
        for (uint32_t c = 0; c < a.channels; ++c)
            for (uint32_t s = 0; s < kBlock; ++s) in_e[i] += double(sig.buf[c][s]) * sig.buf[c][s];
        const int64_t from = host.run(sig.in.data(), sig.out.data(), kBlock, i, deadline);
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
        char what[160];
        std::snprintf(what, sizeof what,
                      "%+.0f dB pushed in callback %llu lands in block %llu, the first submitted after it",
                      phases[k].db, (unsigned long long)phases[k].from, (unsigned long long)land);
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

DelayFit fit_delay(const std::vector<float>& in, const std::vector<float>& out,
                   const std::vector<char>& valid, uint32_t L) {
    // residual(d): worst |wet - dry delayed by d| over comparable samples.
    auto residual = [&](uint32_t d) {
        double worst = 0;
        uint64_t n = 0;
        for (uint64_t t = d; t < out.size(); ++t)
            if (valid[t]) {
                worst = std::max(worst, std::fabs(double(out[t]) - double(in[t - d])));
                ++n;
            }
        return n > 4 * kBlock ? worst : 1e9;
    };
    DelayFit f;
    double best_r = 1e9;
    for (uint32_t d = 0; d <= L + 4 * kBlock; ++d) {
        const double r = residual(d);
        if (r < best_r) best_r = r, f.best = d;
    }
    f.at = residual(L);
    f.before = L ? residual(L - 1) : 1e9;
    f.after = residual(L + 1);
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

// A latency-reporting chain: what it reports is what it does, to the sample,
// and PDC built on the report nulls the wet path against the dry one. In
// pipelined mode the report includes the block of IPC delay.
int scenario_latency(const Args& a) {
    const std::vector<std::string> chain = a.chain.empty() ? std::vector<std::string>{kDpl} : a.chain;
    const uint32_t depth = depth_of(a);
    std::printf("latency: %s, mono, %s\n", join(chain, ',').c_str(), a.mode.c_str());
    Host host;
    if (int r = start_ready(host, a, chain, 1, depth)) return r;
    const uint32_t gen0 = host.h->latency_generation.load();

    const uint64_t total = a.blocks ? a.blocks : 900;
    std::vector<float> in(total * kBlock), out(total * kBlock, 0.f);
    std::vector<char> got(total, 0);
    uint32_t lcg = 12345;
    for (float& s : in) {
        lcg = lcg * 1664525u + 1013904223u;
        s = 0.1f * (float(lcg >> 8) / float(1u << 24) * 2.f - 1.f);  // -20 dBFS peak noise
    }
    Signal sig(1);
    const LoopStats st = run_periodic(total, [&](uint64_t i, uint64_t deadline) {
        std::memcpy(sig.buf[0].data(), &in[i * kBlock], kBlock * sizeof(float));
        // Stored by the callback that played it, so the IPC block shows up
        // as delay exactly like a plugin's look-ahead.
        if (host.run(sig.in.data(), sig.out.data(), kBlock, i, deadline) >= 0) {
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

    // A sample is comparable when its output block arrived and no dropped
    // block lies within L + 2 blocks before it (a drop is a gap in the
    // plugin's own input history).
    std::vector<char> valid(total * kBlock, 0);
    const uint64_t first = (L + kBlock - 1) / kBlock + 16, reach = (L + kBlock - 1) / kBlock + 2;
    uint64_t dropped = 0, last_drop = 0;
    bool any_drop = false;
    for (uint64_t b = 0; b < total; ++b) {
        if (!got[b] && !(depth == 1 && b == 0)) {
            ++dropped;
            any_drop = true;
            last_drop = b;
        }
        const bool ok = got[b] && b >= first && (!any_drop || b > last_drop + reach);
        std::fill(valid.begin() + std::ptrdiff_t(b * kBlock), valid.begin() + std::ptrdiff_t((b + 1) * kBlock), ok ? 1 : 0);
    }
    std::printf("  dropped %llu of %llu blocks\n", (unsigned long long)dropped, (unsigned long long)total);

    check_delay(fit_delay(in, out, valid, L), L);
    check(dropped * 20 < total, "fewer than 5 % of blocks dropped");
    return 0;
}

// A plugin whose latency changes while running (VST3: the LSP limiter's
// look-ahead, reported by restartComponent(kLatencyChanged)): the host
// publishes the new value with a new generation, and the new value is what
// the plugin then does, to the sample. The blocks between the change and the
// report are counted, not compared: that lag is how long PDC is wrong.
int scenario_latency_change(const Args& a) {
    if (a.chain.empty() || !a.has_change) {
        std::printf("latency_change: needs --chain and --change-param/--change-value\n");
        return 1;
    }
    const uint32_t depth = depth_of(a);
    std::printf("latency_change: %s, mono, %s, param %u := %g at block 300\n", join(a.chain, ',').c_str(),
                a.mode.c_str(), a.change_param, double(a.change_value));
    Host host;
    if (int r = start_ready(host, a, a.chain, 1, depth)) return r;
    const uint32_t L0 = plugin::checked_latency(host.h->latency_frames.load());
    const uint32_t gen0 = host.h->latency_generation.load();

    const uint64_t total = a.blocks ? a.blocks : 1200, change_at = 300;
    std::vector<float> in(total * kBlock), out(total * kBlock, 0.f);
    std::vector<char> got(total, 0);
    std::vector<uint32_t> reported(total, plugin::kLatencyInvalid);
    uint32_t lcg = 12345;
    for (float& s : in) {
        lcg = lcg * 1664525u + 1013904223u;
        s = 0.1f * (float(lcg >> 8) / float(1u << 24) * 2.f - 1.f);
    }
    Signal sig(1);
    const LoopStats st = run_periodic(total, [&](uint64_t i, uint64_t deadline) {
        if (i == change_at) plugin::push_param(*host.h, {0, a.change_param, a.change_value, 0});
        std::memcpy(sig.buf[0].data(), &in[i * kBlock], kBlock * sizeof(float));
        if (host.run(sig.in.data(), sig.out.data(), kBlock, i, deadline) >= 0) {
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

    auto window = [&](uint64_t from, uint64_t to, uint32_t L) {
        std::vector<char> valid(total * kBlock, 0);
        const uint64_t reach = (L + kBlock - 1) / kBlock + 2;
        uint64_t last_drop = 0;
        bool any_drop = false;
        for (uint64_t b = 0; b < to; ++b) {
            if (!got[b] && !(depth == 1 && b == 0)) any_drop = true, last_drop = b;
            const bool ok = got[b] && b >= from && (!any_drop || b > last_drop + reach);
            std::fill(valid.begin() + std::ptrdiff_t(b * kBlock),
                      valid.begin() + std::ptrdiff_t((b + 1) * kBlock), ok ? 1 : 0);
        }
        return valid;
    };
    std::printf("  before the change:\n");
    check_delay(fit_delay(in, out, window((L0 + kBlock - 1) / kBlock + 16, change_at, L0), L0), L0);
    std::printf("  after the report settled:\n");
    const uint64_t from1 = settled + (std::max(L0, L1) + kBlock - 1) / kBlock + 16;
    check_delay(fit_delay(in, out, window(from1, total, L1), L1), L1);
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
    std::printf("crash: %s, %s\n", join(chain, ',').c_str(), a.mode.c_str());
    Host host;
    if (int r = start_ready(host, a, chain, 1, depth_of(a))) return r;
    const uint64_t total = 400, trigger = 100;
    uint64_t gone_at = 0, completed_after = 0;
    int32_t culprit = -2;
    Signal sig(1);
    const LoopStats st = run_periodic(total, [&](uint64_t i, uint64_t deadline) {
        if (i == trigger) plugin::push_param(*host.h, {1, 0, 1.f, 0});  // crash := 1
        sig.sine(i, 440.0, 0.1f);
        const bool ok = host.run(sig.in.data(), sig.out.data(), kBlock, i, deadline) >= 0;
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
    std::printf("deadline: %s, %s, sleep 20 ms from block 100\n", join(chain, ',').c_str(),
                a.mode.c_str());
    Host host;
    if (int r = start_ready(host, a, chain, 1, depth_of(a))) return r;
    const uint64_t total = 300, trigger = 100;
    int streak = 0;
    int32_t culprit = -2;
    uint64_t removed_at = 0, misses_before = 0;
    Signal sig(1);
    const LoopStats st = run_periodic(total, [&](uint64_t i, uint64_t deadline) {
        if (i == trigger) plugin::push_param(*host.h, {1, 0, 20.f, 0});  // sleep_ms := 20
        sig.sine(i, 440.0, 0.1f);
        if (removed_at) return true;  // out of the plan: the track plays dry
        if (host.run(sig.in.data(), sig.out.data(), kBlock, i, deadline) >= 0) {
            streak = 0;
            return true;
        }
        // Depth 1 has nothing to collect in callback 0; that is not a miss.
        if (depth_of(a) == 1 && i == 0) return true;
        if (i < trigger) ++misses_before;
        // Remember who was running during the streak. At depth 1 the last
        // miss can fall just after the slow block finished (20 ms sleep, 8
        // misses = 21.3 ms), when current_instance is already -1: reading it
        // only at removal named nobody.
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
    std::printf("nonfinite: %s, %s, NaN/Inf from block 100 to 200\n", join(chain, ',').c_str(),
                a.mode.c_str());
    Host host;
    if (int r = start_ready(host, a, chain, 1, depth_of(a))) return r;
#if defined(__x86_64__) || defined(__i386__) || defined(__aarch64__)
    check(host.h->host_flags.load() & plugin::kHostFlushDenormals,
          "the host set FTZ/DAZ on its processing thread");
#endif
    const uint64_t total = 300, on = 100, off = 200;
    uint64_t nonfinite_out = 0, zeroed = 0, bad_blocks = 0, clean_after = 0, compared_after = 0;
    std::vector<std::vector<float>> inputs(total, std::vector<float>(kBlock));
    Signal sig(1);
    const LoopStats st = run_periodic(total, [&](uint64_t i, uint64_t deadline) {
        if (i == on) plugin::push_param(*host.h, {0, 0, 1.f, 0});
        if (i == off) plugin::push_param(*host.h, {0, 0, 0.f, 0});
        sig.sine(i, 440.0, 0.1f);
        std::copy(sig.buf[0].begin(), sig.buf[0].begin() + kBlock, inputs[i].begin());
        const int64_t from = host.run(sig.in.data(), sig.out.data(), kBlock, i, deadline);
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
    check(bad_blocks >= 50 && counted0 >= zeroed && counted0 % (kBlock / 4) == 0,
          "every NaN/Inf was replaced by 0 and counted against the plugin that wrote it");
    check(counted1 == 0, "the plugin after it saw only finite input and is not blamed");
    check(compared_after > 50 && clean_after == compared_after,
          "once the plugin stops, the chain passes the input unchanged");
    return 0;
}

// What a plugin declares is checked against what it does before the engine
// is offered the chain.
int scenario_refusals(const Args& a) {
    struct Case {
        const char* what;
        std::vector<std::string> chain;
        uint32_t channels;
        plugin::Refusal want;
        int instance;
    };
    const std::vector<Case> lv2_cases = {
        {"declares 2 channels, writes 8: refused", {"urn:sonora:test#liar"}, 2,
         plugin::Refusal::WroteOutsideBuffers, 0},
        {"stereo-only plugin on a mono chain: refused", {"urn:sonora:test#liar"}, 1,
         plugin::Refusal::ChannelMismatch, 0},
        {"unknown URI: refused, and named", {"urn:sonora:test#crash", "urn:sonora:test#absent"}, 1,
         plugin::Refusal::NotFound, 1},
    };
    // tests/vst3/sonora_test_vst3.cpp
    const std::vector<Case> vst3_cases = {
        {"VST3 stereo-only plugin on a mono chain: refused", {kVst3StereoOnly}, 1,
         plugin::Refusal::ChannelMismatch, 0},
        {"VST3 plugin that says yes to mono and stays stereo: refused", {kVst3YesMan}, 1,
         plugin::Refusal::ChannelMismatch, 0},
        {"VST3 plugin that declares 2 channels and writes 8: refused", {kVst3Liar}, 2,
         plugin::Refusal::WroteOutsideBuffers, 0},
        // Once read 8 channels for a 2-channel chain and indexed past it.
        {"VST3 plugin that takes stereo, then answers 8 channels: refused", {kVst3Turncoat}, 2,
         plugin::Refusal::ChannelMismatch, 0},
        {"unknown VST3 class: refused, and named", {kVst3Crash, "vst3:00000000000000000000000000000000"},
         1, plugin::Refusal::NotFound, 1},
    };
    const std::vector<Case>& cases = a.format == "vst3" ? vst3_cases : lv2_cases;
    std::printf("refusals (%s)\n", a.format.c_str());
    for (const Case& c : cases) {
        Host host;
        if (!host.start(a, c.chain, c.channels, plugin::kDefaultPipelineDepth)) return 1;
        const plugin::HostState s = host.wait_ready(30'000'000'000ull);
        const auto reason = static_cast<plugin::Refusal>(host.h->refusal.load());
        const int inst = plugin::checked_instance(host.h->refusal_instance.load(), uint32_t(c.chain.size()));
        usleep(100000);
        host.alive();
        std::printf("  [%s] state %s, refusal %u, instance %d, host %s\n", join(c.chain, ',').c_str(),
                    state_name(uint32_t(s)), unsigned(reason), inst, host.death().c_str());
        check(s == plugin::HostState::Refused && reason == c.want && inst == c.instance, c.what);
        check(host.reaped() && WIFEXITED(host.status()) && WEXITSTATUS(host.status()) == 3,
              "  ...and the host exited with code 3");
    }
    return 0;
}

// As many distinct parameter changes in one block as the ring holds all reach
// the plugin in that block, and one the host cannot apply is counted, not
// lost silently. The params test plugin writes how many changes it saw into
// sample 0 and their sum into sample 1.
int scenario_params(const Args& a) {
    const std::vector<std::string> chain = {kVst3Params};
    std::printf("params: %s, %s\n", join(chain, ',').c_str(), a.mode.c_str());
    Host host;
    if (int r = start_ready(host, a, chain, 1, depth_of(a))) return r;
    // One slot of the ring is left for the change the host must reject.
    const uint32_t distinct = plugin::kParamRingCapacity - 1;
    const float value = 0.25f;
    const uint64_t total = 300, at = 100;
    uint32_t pushed = 0;
    int64_t seen_from = -1;
    float seen_count = -1, seen_sum = -1;
    Signal sig(1);
    const LoopStats st = run_periodic(total, [&](uint64_t i, uint64_t deadline) {
        if (i == at) {
            for (uint32_t id = 0; id < distinct; ++id)
                pushed += plugin::push_param(*host.h, {0, id, value, 0}) ? 1 : 0;
            pushed += plugin::push_param(*host.h, {0, 9999, value, 0}) ? 1 : 0;  // no such ParamID
        }
        sig.sine(i, 440.0, 0.1f);
        const int64_t from = host.run(sig.in.data(), sig.out.data(), kBlock, i, deadline);
        if (from >= int64_t(at) && seen_from < 0 && sig.obuf[0][0] > 0.f) {
            seen_from = from;
            seen_count = sig.obuf[0][0];
            seen_sum = sig.obuf[0][1];
        }
        return true;
    });
    report_loop(st);
    std::printf("  pushed %u changes in callback %llu; the plugin saw %.0f distinct (sum %.2f) in the "
                "block from callback %lld; host rejected %llu\n",
                pushed, (unsigned long long)at, double(seen_count), double(seen_sum), (long long)seen_from,
                (unsigned long long)host.h->params_rejected.load());
    check(pushed == distinct + 1, "the ring took every change");
    check(seen_count == float(distinct), "every distinct change reached the plugin in one block");
    check(std::fabs(seen_sum - float(distinct) * value) < 1e-3f, "with the value pushed");
    check(host.h->params_rejected.load() == 1, "the change for an unknown ParamID was counted as rejected");
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

// Round trip per block: a measurement of the IPC, not a protocol mode. It
// sleeps on done_word, which the engine's audio thread never does.
int scenario_rtt(const Args& a) {
    const std::vector<std::string> chain = a.chain.empty() ? std::vector<std::string>{kEgAmp} : a.chain;
    const uint64_t total = a.blocks ? a.blocks : 3000;
    std::printf("rtt: %s, %u channel(s), %llu blocks of %u at %u Hz, measurement wait, deadline %.0f us\n",
                join(chain, ',').c_str(), a.channels, (unsigned long long)total, kBlock, kRate,
                kDeadlineNs / 1e3);
    Host host;
    if (int r = start_ready(host, a, chain, a.channels, 0)) return r;
    std::vector<double> rtt_us;
    rtt_us.reserve(total);
    uint64_t dropped = 0;
    Signal sig(a.channels);
    const LoopStats st = run_periodic(total, [&](uint64_t i, uint64_t deadline) {
        sig.sine(i, 997.0, 0.1f);
        uint64_t rtt = 0;
        if (host.round_trip(sig.in.data(), sig.out.data(), kBlock, i, deadline, rtt))
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

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i], v = argv[i + 1];
        if (k == "--host") a.host = v;
        else if (k == "--bundle") a.bundles.push_back(v);
        else if (k == "--format") a.format = v;
        else if (k == "--gain-param") a.gain_param = uint32_t(std::atoi(v.c_str()));
        else if (k == "--gain-linear") a.gain_linear = v == "1";
        else if (k == "--expect-replicas") a.expect_replicas = uint32_t(std::atoi(v.c_str()));
        else if (k == "--change-param") a.change_param = uint32_t(std::strtoul(v.c_str(), nullptr, 10)), a.has_change = true;
        else if (k == "--change-value") a.change_value = std::strtof(v.c_str(), nullptr);
        else if (k == "--scenario") a.scenario = v;
        else if (k == "--mode") {
            if (v != "pipelined" && v != "poll") {
                std::fprintf(stderr, "plugin_driver: --mode is pipelined or poll\n");
                std::exit(2);
            }
            a.mode = v;
        }
        else if (k == "--chain") a.chain = split(v, ',');
        else if (k == "--channels") a.channels = uint32_t(std::atoi(v.c_str()));
        else if (k == "--blocks") a.blocks = uint64_t(std::atoll(v.c_str()));
        else {
            std::fprintf(stderr, "plugin_driver: unknown argument %s\n", k.c_str());
            std::exit(2);
        }
    }
    if (a.host.empty() || a.bundles.empty() || a.scenario.empty()) {
        std::fprintf(stderr, "usage: plugin_driver --host EXE --bundle DIR... --scenario NAME "
                             "[--chain ID,...] [--channels N] [--mode pipelined|poll] [--blocks N]\n"
                             "       [--format lv2|vst3] [--gain-param ID] [--gain-linear 1] "
                             "[--expect-replicas N]\n");
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
    else if (a.scenario == "refusals") r = scenario_refusals(a);
    else if (a.scenario == "rtt") r = scenario_rtt(a);
    else if (a.scenario == "params") r = scenario_params(a);
    else if (a.scenario == "list_json") r = scenario_list_json(a);
    else std::fprintf(stderr, "plugin_driver: unknown scenario %s\n", a.scenario.c_str());

    if (r != 0) return r;
    std::printf("RESULT %s: %s\n", a.scenario.c_str(), failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
