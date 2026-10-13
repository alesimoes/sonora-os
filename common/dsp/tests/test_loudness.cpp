// SPDX-License-Identifier: GPL-3.0-or-later
//
// The loudness meter against the published compliance signals, generated
// here from their definitions (no EBU file is stored or fetched; ADR-0011
// Unverified 1 leaves their redistribution open):
// - EBU Tech 3341 v4 (2023), Table 1, minimum requirements, cases 1-6 and
//   9-23. Cases 7 and 8 are authentic programme recordings and need the
//   EBU's files.
// - EBU Tech 3342 v4 (2023), Table 1, cases 1-4. Cases 5 and 6 are
//   authentic programme.
// - ITU-R BS.1770-5 Annex 1: the K-weighting coefficients at 48 kHz.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "check.h"
#include "sonora/dsp/loudness.h"
#include "sonora/dsp/math.h"

namespace {

using namespace sonora::dsp;

// A multichannel signal as segments of 1 kHz sine at given per-channel
// peak levels (dBFS; -999 = silence), "applied in phase to all channels".
struct Seg {
    double seconds;
    double dbfs;
};

using Chans = std::vector<std::vector<float>>;

Chans tones(double fs, const std::vector<Seg>& segs, std::uint32_t channels = 2,
            const std::vector<double>& ch_offset_db = {}) {
    Chans out(channels);
    std::size_t total = 0;
    for (const Seg& s : segs) total += static_cast<std::size_t>(std::llround(s.seconds * fs));
    for (auto& c : out) c.reserve(total);
    std::size_t n = 0;
    for (const Seg& s : segs) {
        const auto len = static_cast<std::size_t>(std::llround(s.seconds * fs));
        for (std::size_t i = 0; i < len; ++i, ++n) {
            const double v = s.dbfs < -900.0 ? 0.0 : std::sin(kTwoPi * 1000.0 * n / fs);
            for (std::uint32_t c = 0; c < channels; ++c) {
                const double db = s.dbfs + (ch_offset_db.empty() ? 0.0 : ch_offset_db[c]);
                out[c].push_back(static_cast<float>(s.dbfs < -900.0 ? 0.0 : v * db_to_gain(db)));
            }
        }
    }
    return out;
}

// Feeds the meter in 128-frame blocks, calling `each_step` whenever a new
// 10 ms reading exists.
void feed(LoudnessMeter& m, const Chans& x, const std::function<void()>& each_step = {}) {
    const std::size_t n = x[0].size();
    std::vector<const float*> ptr(x.size());
    for (std::size_t pos = 0; pos < n; pos += 128) {
        const auto len = static_cast<std::uint32_t>(std::min<std::size_t>(128, n - pos));
        for (std::size_t c = 0; c < x.size(); ++c) ptr[c] = &x[c][pos];
        const std::uint64_t before = m.steps();
        m.process(ptr.data(), len);
        if (each_step && m.steps() != before) each_step();
    }
}

bool near(double v, double want, double tol, const char* what, double fs) {
    const bool ok = std::fabs(v - want) <= tol;
    std::printf("  %-44s %8.3f (want %.1f +-%.1f) at %g %s\n", what, v, want, tol, fs,
                ok ? "ok" : "FAIL");
    return ok;
}

int test_k_weighting_coefficients() {
    // BS.1770-5 Annex 1, Tables 1 and 2 (48 kHz).
    BiquadCoeffs s1, s2;
    k_weighting(48000.0, s1, s2);
    CHECK(std::fabs(s1.b0 - 1.53512485958697) < 1e-6);
    CHECK(std::fabs(s1.b1 - -2.69169618940638) < 1e-6);
    CHECK(std::fabs(s1.b2 - 1.19839281085285) < 1e-6);
    CHECK(std::fabs(s1.a1 - -1.69065929318241) < 1e-6);
    CHECK(std::fabs(s1.a2 - 0.73248077421585) < 1e-6);
    CHECK(s2.b0 == 1.0 && s2.b1 == -2.0 && s2.b2 == 1.0);
    CHECK(std::fabs(s2.a1 - -1.99004745483398) < 1e-6);
    CHECK(std::fabs(s2.a2 - 0.99007225036621) < 1e-6);
    return 0;
}

int test_tech3341_integrated(double fs) {
    struct Case {
        const char* name;
        std::vector<Seg> segs;
        double want;
    };
    const std::vector<Case> cases = {
        {"3341 #1 I, -23 dBFS", {{20.0, -23.0}}, -23.0},
        {"3341 #2 I, -33 dBFS", {{20.0, -33.0}}, -33.0},
        {"3341 #3 I", {{10.0, -36.0}, {60.0, -23.0}, {10.0, -36.0}}, -23.0},
        {"3341 #4 I", {{10.0, -72.0}, {10.0, -36.0}, {60.0, -23.0}, {10.0, -36.0}, {10.0, -72.0}}, -23.0},
        {"3341 #5 I", {{20.0, -26.0}, {20.1, -20.0}, {20.0, -26.0}}, -23.0},
    };
    int bad = 0;
    for (const Case& c : cases) {
        LoudnessMeter m;
        m.prepare(fs, 2);
        feed(m, tones(fs, c.segs));
        bad += !near(m.integrated_lufs(), c.want, 0.1, c.name, fs);
    }
    // #1 and #2 also read M and S at the level once the windows are full.
    for (double level : {-23.0, -33.0}) {
        LoudnessMeter m;
        m.prepare(fs, 2);
        feed(m, tones(fs, {{20.0, level}}));
        bad += !near(m.momentary_lufs(), level, 0.1, "3341 #1/#2 M", fs);
        bad += !near(m.short_term_lufs(), level, 0.1, "3341 #1/#2 S", fs);
    }
    {
        // #6: 5.0 channels, L R C Ls Rs; surrounds weighted 1.41.
        const double w[5] = {1.0, 1.0, 1.0, 1.41, 1.41};
        LoudnessMeter m;
        m.prepare(fs, 5, w);
        feed(m, tones(fs, {{20.0, 0.0}}, 5, {-28.0, -28.0, -24.0, -30.0, -30.0}));
        bad += !near(m.integrated_lufs(), -23.0, 0.1, "3341 #6 I, 5.0", fs);
    }
    CHECK(bad == 0);
    return 0;
}

int test_tech3341_short_term_and_momentary(double fs) {
    int bad = 0;
    {
        // #9: S constant at -23 after 3 s.
        std::vector<Seg> segs;
        for (int i = 0; i < 5; ++i) {
            segs.push_back({1.34, -20.0});
            segs.push_back({1.66, -30.0});
        }
        LoudnessMeter m;
        m.prepare(fs, 2);
        double lo = 1e9, hi = -1e9;
        feed(m, tones(fs, segs), [&] {
            if (m.steps() > 300) {
                lo = std::fmin(lo, m.short_term_lufs());
                hi = std::fmax(hi, m.short_term_lufs());
            }
        });
        bad += !near(lo, -23.0, 0.1, "3341 #9 S min after 3 s", fs);
        bad += !near(hi, -23.0, 0.1, "3341 #9 S max after 3 s", fs);
    }
    {
        // #12: M constant at -23 after 1 s.
        std::vector<Seg> segs;
        for (int i = 0; i < 25; ++i) {
            segs.push_back({0.18, -20.0});
            segs.push_back({0.22, -30.0});
        }
        LoudnessMeter m;
        m.prepare(fs, 2);
        double lo = 1e9, hi = -1e9;
        feed(m, tones(fs, segs), [&] {
            if (m.steps() > 100) {
                lo = std::fmin(lo, m.momentary_lufs());
                hi = std::fmax(hi, m.momentary_lufs());
            }
        });
        bad += !near(lo, -23.0, 0.1, "3341 #12 M min after 1 s", fs);
        bad += !near(hi, -23.0, 0.1, "3341 #12 M max after 1 s", fs);
    }
    // #10 (file-based, S) and #13 (file-based, M): each segment measured
    // from a reset meter.
    double worst10 = 0.0, worst13 = 0.0;
    for (int i = 0; i < 20; ++i) {
        LoudnessMeter m;
        m.prepare(fs, 2);
        feed(m, tones(fs, {{i * 0.15, -999.0}, {3.0, -23.0}, {1.0, -999.0}}));
        worst10 = std::fmax(worst10, std::fabs(m.max_short_term_lufs() + 23.0));
        LoudnessMeter m2;
        m2.prepare(fs, 2);
        feed(m2, tones(fs, {{i * 0.02, -999.0}, {0.4, -23.0}, {1.0, -999.0}}));
        worst13 = std::fmax(worst13, std::fabs(m2.max_momentary_lufs() + 23.0));
    }
    bad += !near(-23.0 + worst10, -23.0, 0.1, "3341 #10 max S, worst of 20", fs);
    bad += !near(-23.0 + worst13, -23.0, 0.1, "3341 #13 max M, worst of 20", fs);
    {
        // #11 (live, S) and #14 (live, M): one stream, successive maxima.
        std::vector<Seg> s11, s14;
        for (int i = 0; i < 20; ++i) {
            s11.push_back({i * 0.15, -999.0});
            s11.push_back({3.0, -38.0 + i});
            s11.push_back({3.0 - i * 0.15, -999.0});
            s14.push_back({i * 0.02, -999.0});
            s14.push_back({0.4, -38.0 + i});
            s14.push_back({0.4 - i * 0.02, -999.0});
        }
        LoudnessMeter m;
        m.prepare(fs, 2);
        std::vector<double> max_s(20, -1e9);
        feed(m, tones(fs, s11), [&] {
            // Segment i spans [6 i, 6 i + 6) s; S is attributed to the
            // segment its window ends in.
            const auto seg = static_cast<std::size_t>((m.steps() - 1) / 600);
            if (seg < 20) max_s[seg] = std::fmax(max_s[seg], m.short_term_lufs());
        });
        double w11 = 0.0;
        for (int i = 0; i < 20; ++i) w11 = std::fmax(w11, std::fabs(max_s[i] - (-38.0 + i)));
        bad += !near(-23.0 + w11, -23.0, 0.1, "3341 #11 successive max S, worst dev", fs);

        LoudnessMeter m2;
        m2.prepare(fs, 2);
        std::vector<double> max_m(20, -1e9);
        feed(m2, tones(fs, s14), [&] {
            // Each segment of #14 lasts 0.82 s = 82 steps.
            const auto seg = static_cast<std::size_t>((m2.steps() - 1) / 82);
            if (seg < 20) max_m[seg] = std::fmax(max_m[seg], m2.momentary_lufs());
        });
        double w14 = 0.0;
        for (int i = 0; i < 20; ++i) w14 = std::fmax(w14, std::fabs(max_m[i] - (-38.0 + i)));
        bad += !near(-23.0 + w14, -23.0, 0.1, "3341 #14 successive max M, worst dev", fs);
    }
    CHECK(bad == 0);
    return 0;
}

// #15-#19: sines at fs/k with a given phase, tapered with 10 ms fades.
Chans tp_sine(double fs, double div, double amp, double phase_deg) {
    const std::size_t n = static_cast<std::size_t>(fs);  // 1 s
    const std::size_t fade = static_cast<std::size_t>(0.01 * fs);
    Chans out(2, std::vector<float>(n));
    for (std::size_t i = 0; i < n; ++i) {
        double g = 1.0;
        if (i < fade) g = static_cast<double>(i) / fade;
        if (n - 1 - i < fade) g = static_cast<double>(n - 1 - i) / fade;
        const double v = amp * g * std::sin(kTwoPi * i / div + phase_deg * kPi / 180.0);
        out[0][i] = out[1][i] = static_cast<float>(v);
    }
    return out;
}

// #20-#23: synthesised at 4 fs, low-passed, decimated with an offset.
Chans tp_burst(double fs, int offset) {
    const double f4 = 4.0 * fs;
    const std::size_t n4 = static_cast<std::size_t>(f4 * 0.5);
    std::vector<double> x(n4, 0.0);
    // fs/6 at 0.5, with one period of fs/4 at 1.0 inserted at a positive
    // zero crossing, so the phase runs on continuously either side.
    const double p6 = 24.0;  // samples per fs/6 period at 4 fs
    const double p4 = 16.0;  // samples per fs/4 period at 4 fs
    const std::size_t start = static_cast<std::size_t>(p6) * 400;  // a zero crossing
    for (std::size_t i = 0; i < n4; ++i) {
        double v;
        if (i < start) v = 0.5 * std::sin(kTwoPi * i / p6);
        else if (i < start + 16) v = 1.0 * std::sin(kTwoPi * (i - start) / p4);
        else v = 0.5 * std::sin(kTwoPi * (i - start - 16) / p6);
        x[i] = v;
    }
    // Fades of 10 ms.
    const std::size_t fade = static_cast<std::size_t>(0.01 * f4);
    for (std::size_t i = 0; i < fade; ++i) {
        x[i] *= static_cast<double>(i) / fade;
        x[n4 - 1 - i] *= static_cast<double>(i) / fade;
    }
    // Anti-alias low-pass at 4 fs: Kaiser-windowed sinc, cut-off 0.45 fs.
    const int half = 256;
    std::vector<double> h(2 * half + 1);
    double sum = 0.0;
    for (int k = -half; k <= half; ++k) {
        const double t = 2.0 * 0.45 / 4.0 * k;  // cut-off / (4 fs) * 2
        const double sinc = k == 0 ? 1.0 : std::sin(kPi * t) / (kPi * t);
        const double r = static_cast<double>(k) / half;
        const double w = std::cos(0.5 * kPi * r) * std::cos(0.5 * kPi * r);  // Hann
        h[k + half] = sinc * w;
        sum += sinc * w;
    }
    for (double& v : h) v /= sum;
    const std::size_t n = n4 / 4;
    Chans out(2, std::vector<float>(n));
    for (std::size_t j = 0; j < n; ++j) {
        const long c = static_cast<long>(4 * j + offset);
        double acc = 0.0;
        for (int k = -half; k <= half; ++k) {
            const long idx = c - k;
            if (idx >= 0 && idx < static_cast<long>(n4)) acc += h[k + half] * x[idx];
        }
        out[0][j] = out[1][j] = static_cast<float>(acc);
    }
    return out;
}

int test_tech3341_true_peak(double fs) {
    struct Case {
        const char* name;
        Chans x;
        double want;
    };
    const std::vector<Case> cases = {
        {"3341 #15 fs/4, 0.50, 0 deg", tp_sine(fs, 4.0, 0.50, 0.0), -6.0},
        {"3341 #16 fs/4, 0.50, 45 deg", tp_sine(fs, 4.0, 0.50, 45.0), -6.0},
        {"3341 #17 fs/6, 0.50, 60 deg", tp_sine(fs, 6.0, 0.50, 60.0), -6.0},
        {"3341 #18 fs/8, 0.50, 67.5 deg", tp_sine(fs, 8.0, 0.50, 67.5), -6.0},
        {"3341 #19 fs/4, 1.41, 45 deg", tp_sine(fs, 4.0, 1.41, 45.0), 3.0},
        {"3341 #20 burst, offset 0", tp_burst(fs, 0), 0.0},
        {"3341 #21 burst, offset 1", tp_burst(fs, 1), 0.0},
        {"3341 #22 burst, offset 2", tp_burst(fs, 2), 0.0},
        {"3341 #23 burst, offset 3", tp_burst(fs, 3), 0.0},
    };
    int bad = 0;
    for (const Case& c : cases) {
        LoudnessMeter m;
        m.prepare(fs, 2);
        feed(m, c.x);
        const double v = m.true_peak_dbtp();
        // Tolerance +0.2 / -0.4 dB.
        const bool ok = v <= c.want + 0.2 && v >= c.want - 0.4;
        std::printf("  %-44s %8.3f (want %.1f +0.2/-0.4) at %g %s\n", c.name, v, c.want, fs,
                    ok ? "ok" : "FAIL");
        bad += !ok;
    }
    CHECK(bad == 0);
    return 0;
}

int test_tech3342_lra(double fs) {
    struct Case {
        const char* name;
        std::vector<Seg> segs;
        double want;
    };
    const std::vector<Case> cases = {
        {"3342 #1 LRA, -20/-30", {{20.0, -20.0}, {20.0, -30.0}}, 10.0},
        {"3342 #2 LRA, -20/-15", {{20.0, -20.0}, {20.0, -15.0}}, 5.0},
        {"3342 #3 LRA, -40/-20", {{20.0, -40.0}, {20.0, -20.0}}, 20.0},
        {"3342 #4 LRA, 5 tones", {{20.0, -50.0}, {20.0, -35.0}, {20.0, -20.0}, {20.0, -35.0}, {20.0, -50.0}}, 15.0},
    };
    int bad = 0;
    for (const Case& c : cases) {
        LoudnessMeter m;
        m.prepare(fs, 2);
        feed(m, tones(fs, c.segs));
        bad += !near(m.loudness_range_lu(), c.want, 1.0, c.name, fs);
    }
    CHECK(bad == 0);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_k_weighting_coefficients();
    // Slow (ctest label): one rate under SONORA_DSP_QUICK=1, all three nightly.
    const std::vector<double> rates = dsp_quick() ? std::vector<double>{48000.0}
                                                  : std::vector<double>{44100.0, 48000.0, 96000.0};
    for (double fs : rates) {
        failed += test_tech3341_integrated(fs);
        failed += test_tech3341_short_term_and_momentary(fs);
        failed += test_tech3341_true_peak(fs);
        failed += test_tech3342_lra(fs);
    }
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp loudness: all passed\n");
    return 0;
}
