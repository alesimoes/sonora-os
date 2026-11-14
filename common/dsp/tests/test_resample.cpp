// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shared streaming resampler (resample.h, ADR-0180): the rate plan,
// the measured delay of the pair equals the planned latency at 44.1 and
// 48 kHz in both directions, the passband is transparent, a constant
// passes at unity, a stopband tone is gone, the output does not depend on
// how the input is split into blocks, prepare() refuses what it cannot
// hold, and a non-finite input leaves the output finite.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "check.h"
#include "sonora/dsp/resample.h"

namespace {

using sonora::dsp::plan_rates;
using sonora::dsp::prepare_pair;
using sonora::dsp::RatePlan;
using sonora::dsp::Resampler;

constexpr double kPi = 3.14159265358979323846;

std::vector<float> tones(double fs, std::size_t n, const std::vector<double>& freqs, double amp) {
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        double v = 0.0;
        for (std::size_t k = 0; k < freqs.size(); ++k)
            v += amp * std::sin(2.0 * kPi * freqs[k] * static_cast<double>(i) / fs + 0.7 * static_cast<double>(k));
        x[i] = static_cast<float>(v);
    }
    return x;
}

// One stage over a whole signal, in blocks of `block` (the last shorter).
// Fails the run (returns empty) if a call reaches max_out().
std::vector<float> run(Resampler& r, const std::vector<float>& x, std::uint32_t block, bool* ok) {
    std::vector<float> y, tmp(r.max_out());
    for (std::size_t pos = 0; pos < x.size(); pos += block) {
        const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(block, x.size() - pos));
        const std::uint32_t m = r.process(x.data() + pos, n, tmp.data(), static_cast<std::uint32_t>(tmp.size()));
        if (m >= tmp.size()) *ok = false;
        y.insert(y.end(), tmp.begin(), tmp.begin() + m);
    }
    return y;
}

std::vector<float> round_trip(double fs, double fc, const std::vector<float>& x, std::uint32_t block, RatePlan* plan,
                              bool* ok) {
    std::string err;
    Resampler up, down;
    if (!plan_rates(fs, fc, plan, &err) || !prepare_pair(*plan, block, &up, &down)) {
        *ok = false;
        return {};
    }
    const auto mid = run(up, x, block, ok);
    return run(down, mid, up.max_out(), ok);
}

int test_plans() {
    RatePlan p;
    std::string err;
    CHECK(plan_rates(44100.0, 48000.0, &p, &err) && p.resample && p.p == 160 && p.q == 147);
    std::printf("  latency, 44.1 kHz session, 48 kHz stage: %u samples\n", p.latency);
    CHECK(p.latency == 90);
    CHECK(plan_rates(48000.0, 44100.0, &p, &err) && p.p == 147 && p.q == 160);
    std::printf("  latency, 48 kHz session, 44.1 kHz stage: %u samples\n", p.latency);
    CHECK(plan_rates(48000.0, 48000.0, &p, &err) && !p.resample && p.latency == 0);
    CHECK(plan_rates(96000.0, 48000.0, &p, &err) && p.p == 1 && p.q == 2);
    CHECK(!plan_rates(44100.5, 48000.0, &p, &err) && !err.empty());
    CHECK(!plan_rates(48000.0, 47999.0, &p, &err));
    CHECK(!plan_rates(500.0, 48000.0, &p, &err));
    return 0;
}

// The measured delay: an impulse through the pair peaks exactly `latency`
// session samples later, and the response is symmetric about the peak
// (linear phase), at every block size a host might use.
int test_latency() {
    const std::pair<double, double> pairs[] = {{44100, 48000}, {48000, 44100}};
    for (const auto& pr : pairs) {
        for (std::uint32_t block : {1u, 64u, 128u, 480u, 1024u}) {
            std::vector<float> x(4096, 0.0f);
            const std::size_t at = 1000;
            x[at] = 1.0f;
            RatePlan plan;
            bool ok = true;
            const auto y = round_trip(pr.first, pr.second, x, block, &plan, &ok);
            CHECK(ok && y.size() > at + 2 * plan.latency);
            const auto peak = static_cast<std::size_t>(
                std::max_element(y.begin(), y.end(), [](float a, float b) { return std::fabs(a) < std::fabs(b); }) -
                y.begin());
            if (peak != at + plan.latency) {
                std::fprintf(stderr, "  %.0f/%.0f block %u: peak at %zu, reported %u\n", pr.first, pr.second, block,
                             peak - at, plan.latency);
            }
            CHECK(peak == at + plan.latency);
            double asym = 0.0;
            for (std::size_t k = 1; k < 60; ++k) asym = std::max(asym, std::fabs(double(y[peak + k]) - y[peak - k]));
            CHECK_LE(asym, 1e-5, "impulse response asymmetry about the peak");
        }
        RatePlan plan;
        std::string err;
        plan_rates(pr.first, pr.second, &plan, &err);
        std::printf("  %.0f -> %.0f -> %.0f: measured delay %u samples = reported, blocks 1..1024\n", pr.first,
                    pr.second, pr.first, plan.latency);
    }
    return 0;
}

int test_passband() {
    const std::pair<double, double> pairs[] = {{44100, 48000}, {48000, 44100}, {96000, 48000}, {48000, 96000}};
    for (const auto& pr : pairs) {
        const double fs = pr.first, fc = pr.second, lower = std::min(fs, fc);
        const auto x = tones(fs, static_cast<std::size_t>(0.25 * fs), {97.0, 1031.0, 7919.0, 0.4 * lower}, 0.2);
        RatePlan plan;
        bool ok = true;
        const auto y = round_trip(fs, fc, x, 128, &plan, &ok);
        CHECK(ok && y.size() + 2 >= x.size());
        double e = 0.0;
        const std::size_t skip = plan.latency + 4 * plan.delay_up / plan.p + 200;
        for (std::size_t n = skip; n + 200 < x.size() && n < y.size(); ++n)
            e = std::max(e, static_cast<double>(std::fabs(y[n] - x[n - plan.latency])));
        std::printf("  %6.0f -> %6.0f -> %6.0f: max passband error %.2e (peak 0.8)\n", fs, fc, fs, e);
        CHECK_LE(e, 1e-3, "passband error through the pair");
    }
    return 0;
}

int test_dc_and_stopband() {
    RatePlan plan;
    std::string err;
    CHECK(plan_rates(44100.0, 48000.0, &plan, &err));
    // 48 -> 44.1: a constant at unity, and a 23 kHz tone (aliasing to
    // 21.1 kHz) under the stopband.
    Resampler down;
    CHECK(down.prepare(plan.q, plan.p, plan.cut_down, plan.delay_down, 4096));
    bool ok = true;
    const auto y = run(down, std::vector<float>(48000, 0.5f), 4096, &ok);
    double e = 0.0;
    for (std::size_t n = 2000; n < y.size(); ++n) e = std::max(e, std::fabs(y[n] - 0.5));
    CHECK_LE(e, 1e-6, "a constant's error");
    down.reset();
    const auto z = run(down, tones(48000.0, 48000, {23000.0}, 0.5), 4096, &ok);
    CHECK(ok);
    double r = 0.0;
    for (std::size_t n = 4000; n < z.size(); ++n) r += static_cast<double>(z[n]) * z[n];
    const double db = 10.0 * std::log10(r / static_cast<double>(z.size() - 4000) / 0.125 + 1e-30);
    std::printf("  23 kHz at 48 kHz into 44.1 kHz: %.1f dB\n", db);
    CHECK_LE(db, -80.0, "stopband tone level, dB");
    return 0;
}

int test_block_split() {
    const auto x = tones(44100.0, 20000, {440.0, 5000.0}, 0.3);
    RatePlan plan;
    bool ok = true;
    const auto a = round_trip(44100.0, 48000.0, x, 128, &plan, &ok);
    const auto b = round_trip(44100.0, 48000.0, x, 7, &plan, &ok);
    CHECK(ok);
    const std::size_t n = std::min(a.size(), b.size());
    CHECK(n + 4 > x.size());
    for (std::size_t i = 0; i < n; ++i) CHECK(a[i] == b[i]);
    return 0;
}

int test_refusals() {
    Resampler r;
    float out[8] = {};
    const float in[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    CHECK(r.process(in, 4, out, 8) == 0);  // never prepared: silent
    CHECK(!r.prepare(0, 1, 0.9, 10, 64));
    CHECK(!r.prepare(1, 0, 0.9, 10, 64));
    CHECK(!r.prepare(sonora::dsp::kMaxRatioTerm + 1, 1, 0.9, 10, 64));
    CHECK(!r.prepare(2, 1, 0.0, 10, 64));
    CHECK(!r.prepare(2, 1, 1.5, 10, 64));
    CHECK(!r.prepare(2, 1, std::numeric_limits<double>::quiet_NaN(), 10, 64));
    CHECK(!r.prepare(2, 1, 0.9, 10, 0));
    CHECK(!r.prepare(4096, 1, 0.9, 4096u * 1024u, 64));  // table past kMaxCoefficients
    CHECK(r.prepare(2, 1, 0.9, 40, 64) && r.taps() == 41);
    RatePlan same;
    Resampler a, b;
    CHECK(!prepare_pair(same, 64, &a, &b));  // equal rates: nothing to prepare
    return 0;
}

// NaN and Inf are read as 0: the output stays finite and, once the ring
// has moved past them, equals what a clean input gives.
int test_non_finite() {
    RatePlan plan;
    std::string err;
    CHECK(plan_rates(48000.0, 44100.0, &plan, &err));
    Resampler up, down, up2, down2;
    CHECK(prepare_pair(plan, 256, &up, &down) && prepare_pair(plan, 256, &up2, &down2));
    auto x = tones(48000.0, 4096, {1000.0}, 0.5);
    auto dirty = x;
    dirty[100] = std::numeric_limits<float>::quiet_NaN();
    dirty[101] = std::numeric_limits<float>::infinity();
    dirty[102] = -std::numeric_limits<float>::infinity();
    x[100] = x[101] = x[102] = 0.0f;
    bool ok = true;
    const auto y = run(down, run(up, dirty, 256, &ok), up.max_out(), &ok);
    const auto ref = run(down2, run(up2, x, 256, &ok), up2.max_out(), &ok);
    CHECK(ok && y.size() == ref.size());
    for (std::size_t i = 0; i < y.size(); ++i) CHECK(std::isfinite(y[i]) && y[i] == ref[i]);
    std::printf("  non-finite input: output finite, NaN/Inf read as 0\n");
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_plans();
    failed += test_latency();
    failed += test_passband();
    failed += test_dc_and_stopband();
    failed += test_block_split();
    failed += test_refusals();
    failed += test_non_finite();
    if (failed == 0) std::printf("dsp resample: all passed\n");
    return failed == 0 ? 0 : 1;
}
