// SPDX-License-Identifier: GPL-3.0-or-later
//
// The amp's resampler on its own (amp_resampler.h): the rate plan, the
// pair's delay is exactly the planned latency and transparent in the
// passband, a constant passes at unity, a tone in the stopband is gone,
// and the output does not depend on how the input is split into blocks.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/native/amp_resampler.h"

namespace {

using sonora::native::amp::plan_rates;
using sonora::native::amp::RatePlan;
using sonora::native::amp::Resampler;

constexpr double kPi = 3.14159265358979323846;
int failures = 0;

void expect(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

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
std::vector<float> run(Resampler& r, const std::vector<float>& x, std::uint32_t block) {
    std::vector<float> y, tmp(r.max_out());
    for (std::size_t pos = 0; pos < x.size(); pos += block) {
        const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(block, x.size() - pos));
        const std::uint32_t m = r.process(x.data() + pos, n, tmp.data(), static_cast<std::uint32_t>(tmp.size()));
        expect(m < tmp.size(), "an output count under max_out()");
        y.insert(y.end(), tmp.begin(), tmp.begin() + m);
    }
    return y;
}

std::vector<float> round_trip(double fs, double fc, const std::vector<float>& x, std::uint32_t block, RatePlan& plan) {
    std::string err;
    expect(plan_rates(fs, fc, &plan, &err), "plan " + std::to_string(fs) + " -> " + std::to_string(fc));
    Resampler up, down;
    up.design(plan.p, plan.q, plan.cut_up, plan.delay_up, block);
    down.design(plan.q, plan.p, plan.cut_down, plan.delay_down, up.max_out());
    return run(down, run(up, x, block), up.max_out());
}

void plans() {
    RatePlan p;
    std::string err;
    expect(plan_rates(44100.0, 48000.0, &p, &err) && p.resample && p.p == 160 && p.q == 147, "44.1 -> 48: 160/147");
    std::printf("latency 44.1 kHz session, 48 kHz capture: %u samples\n", p.latency);
    expect(p.latency == 90, "44.1 / 48: 45 + 45 samples");
    expect(plan_rates(48000.0, 48000.0, &p, &err) && !p.resample && p.latency == 0, "same rate: none");
    expect(plan_rates(96000.0, 48000.0, &p, &err) && p.p == 1 && p.q == 2, "96 -> 48: 1/2");
    std::printf("latency 96 kHz session, 48 kHz capture: %u samples\n", p.latency);
    expect(!plan_rates(44100.5, 48000.0, &p, &err) && !err.empty(), "a fractional rate is refused");
    expect(!plan_rates(48000.0, 47999.0, &p, &err), "a ratio with huge terms is refused");
}

void delay_and_passband() {
    const std::pair<double, double> pairs[] = {{44100, 48000}, {48000, 44100}, {96000, 48000},
                                               {48000, 96000}, {88200, 48000}, {192000, 44100}};
    for (const auto& pr : pairs) {
        const double fs = pr.first, fc = pr.second, lower = std::min(fs, fc);
        // In the passband: up to 0.4 of the lower rate.
        const auto x = tones(fs, static_cast<std::size_t>(0.25 * fs), {97.0, 1031.0, 7919.0, 0.4 * lower}, 0.2);
        RatePlan plan;
        const auto y = round_trip(fs, fc, x, 128, plan);
        double e = 0.0;
        const std::size_t skip = plan.latency + 4 * plan.delay_up / plan.p + 200;
        for (std::size_t n = skip; n + 200 < x.size() && n < y.size(); ++n)
            e = std::max(e, static_cast<double>(std::fabs(y[n] - x[n - plan.latency])));
        std::printf("%6.0f -> %6.0f -> %6.0f: latency %u, max error %.2e (peak 0.8)\n", fs, fc, fs, plan.latency, e);
        expect(e < 1e-3, "passband transparent through the pair, " + std::to_string(fs) + "/" + std::to_string(fc));
        expect(y.size() + 2 >= x.size(), "the pair keeps up with its input");
    }
}

void dc_and_stopband() {
    RatePlan plan;
    std::string err;
    plan_rates(44100.0, 48000.0, &plan, &err);
    // 48 -> 44.1: a constant at unity, and a 23 kHz tone (aliasing to
    // 21.1 kHz) under the stopband.
    Resampler down;
    down.design(plan.q, plan.p, plan.cut_down, plan.delay_down, 4096);
    const std::vector<float> one(48000, 0.5f);
    const auto y = run(down, one, 4096);
    double e = 0.0;
    for (std::size_t n = 2000; n < y.size(); ++n) e = std::max(e, std::fabs(y[n] - 0.5));
    expect(e < 1e-6, "a constant passes at unity");
    down.reset();
    const auto hi = tones(48000.0, 48000, {23000.0}, 0.5);
    const auto z = run(down, hi, 4096);
    double r = 0.0;
    for (std::size_t n = 4000; n < z.size(); ++n) r += static_cast<double>(z[n]) * z[n];
    const double db = 10.0 * std::log10(r / static_cast<double>(z.size() - 4000) / 0.125 + 1e-30);
    std::printf("23 kHz at 48 kHz into 44.1 kHz: %.1f dB\n", db);
    expect(db < -80.0, "a stopband tone is rejected by 80 dB");
}

void block_split() {
    const auto x = tones(44100.0, 20000, {440.0, 5000.0}, 0.3);
    RatePlan plan;
    const auto a = round_trip(44100.0, 48000.0, x, 128, plan);
    const auto b = round_trip(44100.0, 48000.0, x, 7, plan);
    const std::size_t n = std::min(a.size(), b.size());
    bool same = n + 4 > x.size();
    for (std::size_t i = 0; i < n; ++i) same = same && a[i] == b[i];
    expect(same, "blocks of 128 and of 7: identical bits");
}

}  // namespace

int main() {
    plans();
    delay_and_passband();
    dc_and_stopband();
    block_split();
    if (failures == 0) std::printf("native amp resampler: all passed\n");
    return failures == 0 ? 0 : 1;
}
