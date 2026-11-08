// SPDX-License-Identifier: GPL-3.0-or-later
//
// The timbre pass (ADR-0066 §4): the measurer against noise through a known
// EQ, the matcher against hand-made curves, the stock EQ's parameter ranges.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/analyze/timbre_match.h"
#include "sonora/dsp/biquad.h"
#include "timbre_fit.h"

using namespace sonora;
using namespace sonora::analyze;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)
#define CHECK_NEAR(a, b, tol)                                                                                 \
    do {                                                                                                      \
        const double va = (a), vb = (b);                                                                      \
        if (!(std::fabs(va - vb) <= (tol))) {                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %.4f, %s = %.4f\n", __FILE__, __LINE__, #a, \
                         va, #b, vb);                                                                         \
            ++g_failures;                                                                                     \
        }                                                                                                     \
    } while (0)

constexpr double kRate = 44100.0;
using Planar = std::vector<std::vector<float>>;

// Pink noise: Paul Kellett's economy filter over xorshift white noise.
std::vector<float> pink_noise(std::size_t n, std::uint32_t seed) {
    std::vector<float> out(n);
    double b0 = 0, b1 = 0, b2 = 0;
    std::uint32_t s = seed;
    for (auto& v : out) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        const double w = (static_cast<double>(s) / 4294967296.0 - 0.5) * 2.0;
        b0 = 0.99765 * b0 + w * 0.0990460;
        b1 = 0.96300 * b1 + w * 0.2965164;
        b2 = 0.57000 * b2 + w * 1.0526913;
        v = static_cast<float>(0.1 * (b0 + b1 + b2 + w * 0.1848));
    }
    return out;
}

struct BellSpec {
    double freq, gain_db, q;
};

std::vector<float> through_bells(const std::vector<float>& x, const std::vector<BellSpec>& bells) {
    std::vector<float> y(x);
    for (const BellSpec& b : bells) {
        dsp::FilterSpec spec;
        spec.shape = dsp::Shape::Peak;
        spec.freq_hz = b.freq;
        spec.q = b.q;
        spec.gain_db = b.gain_db;
        const auto c = dsp::design_matched(spec, kRate);
        dsp::BiquadState st;
        for (auto& v : y) v = static_cast<float>(st.tick(c, v));
    }
    return y;
}

TimbreCurve measure(const std::vector<float>& mono) {
    Planar p{mono};
    MemorySource src(p, kRate);
    WelchTimbreMeasurer m;
    RunOptions run;
    TimbreCurve c;
    std::string err;
    CHECK(m.measure(src, run, c, err));
    return c;
}

bool in_eq_ranges(const EqBand& b, EqBandKind k) {
    if (std::fabs(b.gain_db) > kEqGainLimitDb) return false;
    switch (k) {
        case EqBandKind::LowShelf:
            return b.freq_hz >= kEqLowShelfFreqMin && b.freq_hz <= kEqLowShelfFreqMax && b.q >= kEqShelfQMin &&
                   b.q <= kEqShelfQMax;
        case EqBandKind::HighShelf:
            return b.freq_hz >= kEqHighShelfFreqMin && b.freq_hz <= kEqHighShelfFreqMax && b.q >= kEqShelfQMin &&
                   b.q <= kEqShelfQMax;
        default:
            return b.freq_hz >= kEqBellFreqMin && b.freq_hz <= kEqBellFreqMax && b.q >= kEqBellQMin &&
                   b.q <= kEqBellQMax;
    }
}

void check_ranges(const TimbreFit& fit) {
    CHECK(fit.result.eq_bands.size() == fit.kinds.size());
    for (std::size_t i = 0; i < fit.result.eq_bands.size(); ++i)
        CHECK(in_eq_ranges(fit.result.eq_bands[i], fit.kinds[i]));
}

TimbreMatchParams eight_bands() {
    TimbreMatchParams p;
    p.max_bands = 8;
    return p;
}

// --- tests --------------------------------------------------------------------

void recovers_three_band_eq() {
    const auto ref = pink_noise(static_cast<std::size_t>(kRate * 20), 7);
    const std::vector<BellSpec> bells = {{200.0, 6.0, 1.2}, {1000.0, -5.0, 1.2}, {4000.0, 4.0, 1.2}};
    const auto tgt_audio = ref;
    // The target lacks what the reference has: ref = target through the EQ.
    const auto ref_audio = through_bells(tgt_audio, bells);
    const TimbreCurve rc = measure(ref_audio), tc = measure(tgt_audio);
    TimbreFit fit;
    std::string err;
    CHECK(match_timbre_detailed(rc, tc, eight_bands(), fit, err));
    CHECK(fit.result.eq_bands.size() <= 8);
    CHECK(!fit.result.eq_bands.empty());
    check_ranges(fit);
    for (const BellSpec& b : bells) {
        const double got = eq_bands_response_db(fit.result.eq_bands, fit.kinds, b.freq);
        CHECK_NEAR(got, b.gain_db, 1.5);
    }
    // And the whole curve, not only the three centres.
    double worst = 0.0;
    for (int i = 0; i < kTimbreBands; ++i) {
        if (!fit.valid[i]) continue;
        double truth = 0.0;
        for (const BellSpec& b : bells) {
            dsp::FilterSpec s;
            s.freq_hz = b.freq;
            s.q = b.q;
            s.gain_db = b.gain_db;
            truth += 20.0 * std::log10(std::abs(dsp::analog_response(s, kThirdOctaveCentreHz[i])));
        }
        worst = std::max(worst, std::fabs(eq_bands_response_db(fit.result.eq_bands, fit.kinds, kThirdOctaveCentreHz[i]) - truth));
    }
    std::printf("three-band recovery: %zu bands, residual %.2f dB rms, worst centre error %.2f dB\n",
                fit.result.eq_bands.size(), fit.residual_rms_db, worst);
    CHECK(worst < 1.5);
    CHECK(fit.residual_rms_db < 1.0);
}

void identical_and_level_only_are_flat() {
    const auto x = pink_noise(static_cast<std::size_t>(kRate * 10), 3);
    std::vector<float> quiet(x);
    for (auto& v : quiet) v *= 0.1f;  // -20 dB
    const TimbreCurve a = measure(x), b = measure(x), q = measure(quiet);
    for (const TimbreCurve* other : {&b, &q}) {
        TimbreFit fit;
        std::string err;
        CHECK(match_timbre_detailed(a, *other, eight_bands(), fit, err));
        CHECK(fit.result.eq_bands.empty());
        for (int i = 0; i < kTimbreBands; ++i) CHECK_NEAR(fit.result.curve_db[i], 0.0, 0.05);
    }
}

void shelf_and_clamp() {
    TimbreCurve ref, tgt;
    for (int i = 0; i < kTimbreBands; ++i) {
        ref.valid[i] = tgt.valid[i] = kThirdOctaveCentreHz[i] * 1.13 < kRate / 2.0;
        tgt.gain_db[i] = 0.0;
        ref.gain_db[i] = ref.valid[i] ? 30.0 / (1.0 + std::exp(-(std::log2(kThirdOctaveCentreHz[i] / 4000.0)) * 3.0)) : 0.0;
    }
    TimbreFit fit;
    std::string err;
    CHECK(match_timbre_detailed(ref, tgt, eight_bands(), fit, err));
    check_ranges(fit);
    bool has_high_shelf = false;
    for (std::size_t i = 0; i < fit.kinds.size(); ++i) {
        has_high_shelf = has_high_shelf || fit.kinds[i] == EqBandKind::HighShelf;
        CHECK(std::fabs(fit.result.eq_bands[i].gain_db) <= 12.0 + 1e-9);
    }
    CHECK(has_high_shelf);
    for (int i = 0; i < kTimbreBands; ++i) CHECK(std::fabs(fit.result.curve_db[i]) <= 12.0 + 1e-9);

    TimbreMatchParams two = eight_bands();
    two.max_bands = 2;
    CHECK(match_timbre_detailed(ref, tgt, two, fit, err));
    CHECK(fit.result.eq_bands.size() <= 2);
}

void errors() {
    TimbreCurve none, some;
    some.valid[10] = true;
    TimbreFit fit;
    std::string err;
    CHECK(!match_timbre_detailed(none, some, eight_bands(), fit, err));
    TimbreMatchParams bad = eight_bands();
    bad.max_bands = 9;
    CHECK(!match_timbre_detailed(some, some, bad, fit, err));
    bad.max_bands = 0;
    CHECK(!match_timbre_detailed(some, some, bad, fit, err));

    // The contract entry point is the same matcher.
    TimbreMatchResult r;
    CHECK(match_timbre(some, some, eight_bands(), r, err));
    CHECK(r.eq_bands.empty());

    Planar short_audio{std::vector<float>(1000, 0.1f)};
    MemorySource src(short_audio, kRate);
    WelchTimbreMeasurer m;
    TimbreCurve c;
    CHECK(!m.measure(src, RunOptions{}, c, err));
    Planar silent{std::vector<float>(40000, 0.0f)};
    MemorySource ssrc(silent, kRate);
    CHECK(!m.measure(ssrc, RunOptions{}, c, err));
    std::atomic<bool> cancel{true};
    RunOptions run;
    run.cancel = &cancel;
    Planar noise{pink_noise(40000, 1)};
    MemorySource nsrc(noise, kRate);
    CHECK(!m.measure(nsrc, run, c, err));
    CHECK(err == "cancelled");
}

void edges_follow_sample_rate() {
    // At 44.1 kHz the 20 kHz band's upper edge is past Nyquist.
    const TimbreCurve c = measure(pink_noise(static_cast<std::size_t>(kRate * 3), 5));
    CHECK(!c.valid[kTimbreBands - 1]);
    CHECK(c.valid[kTimbreBands - 3]);  // 12.5 kHz
}

}  // namespace

int main() {
    recovers_three_band_eq();
    identical_and_level_only_are_flat();
    shelf_and_clamp();
    errors();
    edges_follow_sample_rate();
    if (g_failures == 0) std::puts("test_analyze_timbre: ok");
    return g_failures == 0 ? 0 : 1;
}
