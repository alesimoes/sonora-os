// SPDX-License-Identifier: GPL-3.0-or-later
//
// The cabinet against its bars: the impulse response it renders equals the
// response loaded, at every session rate (the resampler, then the
// convolver); a response longer than the limit is cut and said so;
// latency 0; a change of response with no click and the new response
// exact afterwards; the handoff under a loader thread; and the common checks.
//
// The test response is analytic, a sum of damped resonances, so the
// expected output at any rate is the same function sampled there and the
// resampler is held to it. Nothing here is a recording of a real cabinet.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/cab.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

// Resonances of a 4x12-ish closed back, from the literature's usual
// picture: a low-end body near 110 Hz, a presence bump near 2.5 kHz, some
// fizz at 5 kHz and a slow room-ish tail at 400 Hz.
double ir_at(double t) {
    if (t < 0.0) return 0.0;
    const double w = 2.0 * dsp::kPi;
    // The smooth onset keeps the response band-limited, so the resampler is
    // held to what it can do (a slope jump at t = 0 is not representable).
    const double u = t / 0.0005;
    const double onset = 1.0 - std::exp(-u * u * u * u);
    return onset * (0.5 * std::exp(-t / 0.020) * std::sin(w * 110.0 * t) +
           0.7 * std::exp(-t / 0.004) * std::sin(w * 2500.0 * t) +
           0.3 * std::exp(-t / 0.0015) * std::sin(w * 5000.0 * t) +
           0.2 * std::exp(-t / 0.050) * std::sin(w * 400.0 * t));
}

// A second, different response for the change test: a delayed, inverted copy.
double ir2_at(double t) { return -0.8 * ir_at(t - 0.0007); }

std::vector<float> make_ir(double (*f)(double), double fs, double seconds) {
    std::vector<float> h(static_cast<std::size_t>(std::lround(seconds * fs)));
    for (std::size_t i = 0; i < h.size(); ++i) h[i] = static_cast<float>(f(static_cast<double>(i) / fs));
    return h;
}

bool g_unit_impulse = false;

constexpr double kIrRate = 48000.0;
constexpr double kIrSeconds = 0.3;

std::unique_ptr<Cab> cab_with(double (*f)(double)) {
    auto c = make_cab();
    const auto h = make_ir(f, kIrRate, kIrSeconds);
    c->load_ir(h.data(), h.size(), kIrRate);
    return c;
}

// Renders by hand so a control-thread action can run between blocks.
template <class Hook>
std::vector<float> run_blocks(Processor& p, const std::vector<float>& x, Hook hook) {
    std::vector<float> y(x.size());
    const std::uint32_t block = 128;
    for (std::size_t pos = 0; pos < x.size(); pos += block) {
        const auto m = static_cast<std::uint32_t>(std::min<std::size_t>(block, x.size() - pos));
        hook(pos / block);
        const float* in[1] = {x.data() + pos};
        float* out[1] = {y.data() + pos};
        AudioBuffers io{in, out, nullptr, 1, 0, m};
        p.process(io, nullptr, 0, Transport{});
    }
    return y;
}

void response_equals_loaded(Suite& s) {
    for (double fs : measure::kRates) {
        auto p = s.make();
        p->prepare(fs, 128, 2);
        Chans x = measure::silence(fs, kIrSeconds + 0.15);
        x[0][4096] = x[1][4096] = 1.0f;
        const Chans y = measure::render(*p, x);
        double err = 0.0, peak = 0.0;
        for (std::size_t i = 0; i < y[0].size(); ++i) {
            const double want = i < 4096 || i - 4096 >= static_cast<std::size_t>(kIrSeconds * fs) ? 0.0 : ir_at(static_cast<double>(i - 4096) / fs);
            err = std::max(err, static_cast<double>(std::fabs(y[0][i] - want)));
            peak = std::max(peak, std::fabs(want));
        }
        const std::string n = std::to_string(static_cast<int>(fs)) + " Hz";
        s.check_le("impulse response vs the loaded response sampled at the session rate, " + n + ": max error / peak",
                   err / peak, fs == kIrRate ? s.bar("cab.exact_error") : s.bar("cab.resampled_error"), "linear");
        double d = 0.0;
        for (std::size_t i = 0; i < y[0].size(); ++i) d = std::max(d, static_cast<double>(std::fabs(y[0][i] - y[1][i])));
        s.check_le("both channels the same, " + n, d, 0.0, "linear");
    }
}

// The longest the processor takes at the highest rate, and one past it.
void length_limit(Suite& s) {
    const double fs = 96000.0;
    auto p = s.make();
    p->prepare(fs, 128, 1);
    auto* c = dynamic_cast<Cab*>(p.get());
    const auto h = make_ir(ir_at, fs, 1.0);
    const Cab::LoadResult full = c->load_ir(h.data(), h.size(), fs);
    s.check("a response of exactly kMaxSeconds is taken whole", full == Cab::LoadResult::Ok ? 0.0 : 1.0, 0.0,
            full == Cab::LoadResult::Ok, "");
    // Let the first load be taken over before the next.
    run_blocks(*p, std::vector<float>(2048, 0.0f), [](std::size_t) {});
    const auto longer = make_ir(ir_at, fs, 1.5);
    const Cab::LoadResult cut = c->load_ir(longer.data(), longer.size(), fs);
    s.check("a longer response is cut and reported", cut == Cab::LoadResult::Truncated ? 0.0 : 1.0, 0.0,
            cut == Cab::LoadResult::Truncated, "");
    std::vector<float> x(static_cast<std::size_t>(1.2 * fs), 0.0f);
    x[0] = 1.0f;
    const auto y = run_blocks(*p, x, [](std::size_t) {});
    // The cut response ends in a 10 ms fade to zero.
    double tail = 0.0;
    for (std::size_t i = static_cast<std::size_t>(0.99 * fs); i < static_cast<std::size_t>(fs); ++i)
        tail = std::max(tail, static_cast<double>(std::fabs(y[i])));
    double after = 0.0;
    for (std::size_t i = static_cast<std::size_t>(fs) + 8; i < y.size(); ++i)
        after = std::max(after, static_cast<double>(std::fabs(y[i])));
    s.check_le("the cut response ends at kMaxSeconds: nothing after it (FFT rounding only)", after, 1e-9, "linear");
    s.info("the cut response's last 10 ms, peak", tail, "linear");

    const std::vector<float> bad = {0.5f, std::nanf(""), 0.1f};
    const Cab::LoadResult r = c->load_ir(bad.data(), bad.size(), fs);
    s.check("a non-finite response is refused", r == Cab::LoadResult::Invalid ? 0.0 : 1.0, 0.0,
            r == Cab::LoadResult::Invalid, "");
    const Cab::LoadResult r2 = c->load_ir(h.data(), 0, fs);
    s.check("an empty response is refused", r2 == Cab::LoadResult::Invalid ? 0.0 : 1.0, 0.0,
            r2 == Cab::LoadResult::Invalid, "");
}

void change_of_response(Suite& s) {
    const double fs = 48000.0;
    const auto b = make_ir(ir2_at, kIrRate, kIrSeconds);
    Chans sine = measure::sine(fs, 1000.0, -6.0, 1.0, 1);
    const std::vector<float>& x = sine[0];

    auto steady = [&](double (*f)(double)) {
        auto c = cab_with(f);
        c->prepare(fs, 128, 1);
        return run_blocks(*c, x, [](std::size_t) {});
    };
    const auto ya = steady(ir_at), yb = steady(ir2_at);
    auto maxstep = [](const std::vector<float>& v, std::size_t from, std::size_t to) {
        double m = 0.0;
        for (std::size_t i = from + 1; i < to; ++i) m = std::max(m, static_cast<double>(std::fabs(v[i] - v[i - 1])));
        return m;
    };
    const double ref_step = std::max(maxstep(ya, 20000, 40000), maxstep(yb, 20000, 40000));

    auto c = cab_with(ir_at);
    c->prepare(fs, 128, 1);
    Cab::LoadResult first = Cab::LoadResult::Invalid, second = Cab::LoadResult::Ok, third = Cab::LoadResult::Invalid;
    const std::size_t at = 200;  // block
    const auto y = run_blocks(*c, x, [&](std::size_t blk) {
        if (blk == at) {
            first = c->load_ir(b.data(), b.size(), kIrRate);
            // The audio thread has not taken the first one over yet.
            second = c->load_ir(b.data(), b.size(), kIrRate);
        }
        if (blk == at + 20) third = c->load_ir(b.data(), b.size(), kIrRate);
    });
    s.check("a load is accepted", first == Cab::LoadResult::Ok ? 0.0 : 1.0, 0.0, first == Cab::LoadResult::Ok, "");
    s.check("a second load before the first is taken over is Busy", second == Cab::LoadResult::Busy ? 0.0 : 1.0, 0.0,
            second == Cab::LoadResult::Busy, "");
    s.check("a load after the change completed is accepted", third == Cab::LoadResult::Ok ? 0.0 : 1.0, 0.0,
            third == Cab::LoadResult::Ok, "");

    const std::size_t from = at * 128;
    double before = 0.0, after = 0.0;
    for (std::size_t i = 0; i < from; ++i) before = std::max(before, static_cast<double>(std::fabs(y[i] - ya[i])));
    // The second change (third load) is to the same response: the wet path
    // dips and returns, the sound afterwards is B.
    for (std::size_t i = (at + 40) * 128; i < y.size(); ++i)
        after = std::max(after, static_cast<double>(std::fabs(y[i] - yb[i])));
    s.check_le("before the change: identical to the old response alone", before, 0.0, "linear");
    s.check_le("after the change: identical to the new response alone", after, s.bar("cab.exact_error") * 4, "linear",
               "response-independent convolver state: no residue of the old response");
    const double step = maxstep(y, from, y.size());
    s.check_le("largest sample step across the change / steady sine's", step / ref_step, s.bar("cab.change_step_ratio"),
               "ratio", "no click");
}

void concurrent_loads(Suite& s) {
    const double fs = 48000.0;
    auto c = cab_with(ir_at);
    c->prepare(fs, 128, 2);
    const auto a = make_ir(ir_at, kIrRate, kIrSeconds), b = make_ir(ir2_at, kIrRate, kIrSeconds);
    double l1 = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) l1 = std::max({l1, 0.0});
    double la = 0.0, lb = 0.0;
    for (float v : a) la += std::fabs(v);
    for (float v : b) lb += std::fabs(v);
    l1 = std::max(la, lb);
    const int wanted = measure::quick() ? 15 : 80;
    std::atomic<int> loaded{0};
    std::atomic<bool> done{false};
    std::thread loader([&] {
        const auto t0 = std::chrono::steady_clock::now();
        while (loaded < wanted && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(40)) {
            const auto& h = loaded % 2 == 0 ? b : a;
            if (c->load_ir(h.data(), h.size(), kIrRate) == Cab::LoadResult::Ok) ++loaded;
            else std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
        done = true;
    });
    const Chans x = measure::noise(fs, -6.0, 1.0, 11u);
    double peak = 0.0;
    bool finite = true;
    std::uint64_t blocks = 0;
    measure::RenderOptions opt;
    while (!done) {
        const Chans y = measure::render(*c, x, {}, opt);
        for (const auto& ch : y) {
            peak = std::max(peak, measure::peak(ch));
            for (float v : ch) finite = finite && std::isfinite(v);
        }
        blocks += x[0].size() / 128;
    }
    loader.join();
    s.check("loads under a running audio thread all land", static_cast<double>(loaded), wanted, loaded == wanted, "loads");
    s.check("output stays finite while responses are swapped", finite ? 0.0 : 1.0, 0.0, finite, "");
    // Every output sample is one kernel's convolution times a gain of at
    // most 1 (dry is 0 at the default mix): bounded by its L1 norm x peak.
    s.check_le("output peak under the swaps <= L1 norm x input peak", peak, l1 * 0.5 * 1.01, "linear");
    s.info("blocks rendered during the loads", static_cast<double>(blocks), "blocks");
}

// The harness's check_latency cross-correlates, and a response has its own
// group delay (about 20 samples for this one), which PDC must not
// compensate. So the latency is measured on the unit impulse the cabinet is
// before any response is loaded, where the only delay left is the
// convolver's: reported 0, measured 0, at every rate and block size.
void latency(Suite& s) {
    for (double fs : measure::kRates) {
        for (std::uint32_t block : {32u, 128u, 480u}) {
            auto p = make_cab();
            p->prepare(fs, block, 2);
            measure::RenderOptions opt;
            opt.block = block;
            const Chans x = measure::noise(fs, -6.0, 0.5, 21u);
            const Chans y = measure::render(*p, x, {}, opt);
            const long lag = measure::xcorr_lag(x[0], y[0], 64);
            s.check("latency, " + std::to_string(static_cast<int>(fs)) + " Hz, block " + std::to_string(block) +
                        ": measured - reported",
                    static_cast<double>(lag - static_cast<long>(p->latency())), 0.0,
                    lag == static_cast<long>(p->latency()), "samples", "reported " + std::to_string(p->latency()));
            double d = 0.0;
            for (std::size_t i = 0; i < x[0].size(); ++i) d = std::max(d, static_cast<double>(std::fabs(x[0][i] - y[0][i])));
            s.check_le("unit impulse response: output = input, " + std::to_string(static_cast<int>(fs)) + " Hz, block " +
                           std::to_string(block),
                       d, 1e-6, "linear");
        }
    }
}

void dry_path(Suite& s) {
    const double fs = 48000.0;
    auto p = s.make();
    measure::apply(*p, {{"mix", 0.0f}});
    p->prepare(fs, 128, 2);
    const Chans x = measure::noise(fs, -6.0, 0.5, 5u);
    const Chans y = measure::render(*p, x);
    double d = 0.0;
    for (std::size_t i = 0; i < x[0].size(); ++i) d = std::max(d, static_cast<double>(std::fabs(x[0][i] - y[0][i])));
    s.check("mix 0: the output is the input", d, 0.0, d == 0.0, "linear");
}

// The cost, reported against the class E budget it is a share of (ADR-0011
// section 3.5, Amendment 1): class E is an amp capture plus this cabinet,
// p99.9 <= 250 us and max <= 500 us. Never asserted here.
void class_e_share(Suite& s, const Config& golden) {
    if (!s.args().cpu) return;
    const Chans in = measure::cpu_programme(48000.0, s.args().cpu_seconds, 0.0);
    std::vector<double> best;
    for (int run = 0; run < 5; ++run) {
        auto p = s.make();
        measure::apply(*p, golden);
        p->prepare(48000.0, 128, 2);
        std::vector<double> us;
        measure::RenderOptions opt;
        opt.block_us = &us;
        measure::render(*p, in, {}, opt);
        if (best.empty()) best = us;
        for (std::size_t i = 0; i < best.size() && i < us.size(); ++i) best[i] = std::min(best[i], us[i]);
    }
    std::sort(best.begin(), best.end());
    const double p999 = best[static_cast<std::size_t>(0.999 * (best.size() - 1))], mx = best.back();
    s.info("cabinet alone (0.3 s response): p99.9 per block, share of class E's 250 us", p999, "us",
           std::to_string(100.0 * p999 / 250.0) + " %, per-block minimum of 5 runs");
    s.info("cabinet alone: max per block, share of class E's 500 us", mx, "us", std::to_string(100.0 * mx / 500.0) + " %");
}

}  // namespace

measure::Config cab_golden_config() { return {{"output", -2.0f}, {"mix", 85.0f}}; }

int cab(const measure::Args& args) {
    Suite s("cab", [] { return g_unit_impulse ? std::unique_ptr<Processor>(make_cab()) : std::unique_ptr<Processor>(cab_with(ir_at)); }, args);
    response_equals_loaded(s);
    length_limit(s);
    change_of_response(s);
    concurrent_loads(s);
    dry_path(s);
    const Config golden = cab_golden_config();
    latency(s);
    measure::check_nonfinite(s, golden);
    measure::check_reset(s, golden);
    measure::check_denormals(s, golden);
    measure::check_zipper(s, {}, "");
    measure::check_golden(s, golden);
    // Reported, not held: the class E bars are for the amp and the cabinet
    // together, and this machine is not the reference machine.
    measure::check_cpu(s, golden, "", false);
    class_e_share(s, golden);
    return s.finish();
}

}  // namespace sonora::native::suites
