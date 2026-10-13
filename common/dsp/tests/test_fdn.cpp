// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FDN core against the properties its design rests on (fdn.h): the
// output vectors, distinct prime lengths, a lossless network at freeze
// (Schlecht and Habets), absorbent filters that never pass unity while
// T60 is finite, the decay of the whole network against the set T60, and
// a direct term that puts the first wet sample at time zero.
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "check.h"
#include "sonora/dsp/biquad.h"
#include "sonora/dsp/fdn.h"
#include "sonora/dsp/math.h"

namespace {

using namespace sonora::dsp;

const double kRates[] = {44100.0, 48000.0, 96000.0};

// xorshift32 in [-1, 1).
struct Noise {
    std::uint32_t s = 0x9e3779b9u;
    double next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<double>(s) / 2147483648.0 - 1.0;
    }
};

int test_output_signs() {
    const signed char* l = Fdn::output_signs(0);
    const signed char* r = Fdn::output_signs(1);
    int dot = 0;
    for (std::uint32_t i = 0; i < Fdn::kLines; ++i) {
        CHECK(l[i] == 1 || l[i] == -1);
        CHECK(r[i] == 1 || r[i] == -1);
        dot += l[i] * r[i];
    }
    CHECK(dot == 0);
    // Injection: +-1, orthogonal, and a dot product of 2 with each output
    // (the 1/8 direct term).
    const signed char* il = Fdn::input_signs(0);
    const signed char* ir = Fdn::input_signs(1);
    int ii = 0, ll = 0, lr = 0, rl = 0, rr = 0;
    for (std::uint32_t i = 0; i < Fdn::kLines; ++i) {
        CHECK(il[i] == 1 || il[i] == -1);
        CHECK(ir[i] == 1 || ir[i] == -1);
        ii += il[i] * ir[i];
        ll += il[i] * l[i];
        lr += il[i] * r[i];
        rl += ir[i] * l[i];
        rr += ir[i] * r[i];
    }
    CHECK(ii == 0 && ll == 2 && lr == 2 && rl == 2 && rr == 2);
    // Walsh-Hadamard spectra: every coefficient +-4 (bent), so through the
    // Hadamard matrix each line reaches an output with the same weight.
    for (const signed char* v : {l, r}) {
        for (std::uint32_t k = 0; k < Fdn::kLines; ++k) {
            int acc = 0;
            for (std::uint32_t j = 0; j < Fdn::kLines; ++j) {
                int bits = 0;
                for (std::uint32_t t = k & j; t != 0; t &= t - 1) ++bits;
                acc += (bits % 2 == 0 ? 1 : -1) * v[j];
            }
            CHECK(acc == 4 || acc == -4);
        }
    }
    return 0;
}

int test_lengths() {
    for (double fs : kRates) {
        std::uint32_t prev_max = 0;
        for (int step = 0; step <= 100; ++step) {
            std::uint32_t m[Fdn::kLines];
            Fdn::lengths_for(step / 100.0, fs, m);
            std::uint32_t top = 0;
            for (std::uint32_t i = 0; i < Fdn::kLines; ++i) {
                for (std::uint32_t d = 2; d * d <= m[i]; ++d) CHECK(m[i] % d != 0);
                for (std::uint32_t j = 0; j < i; ++j) CHECK(m[i] != m[j]);
                if (m[i] > top) top = m[i];
            }
            CHECK(top >= prev_max);  // size grows the network
            CHECK(top <= std::lround(Fdn::kMaxSeconds * fs * 1.01) + 2);
            prev_max = top;
        }
    }
    return 0;
}

// Every line's filter, gain * low shelf * high shelf, stays below unity at
// every frequency for every finite T60 the reverb allows (0.1 s to 40 s,
// the decay's range times its band ratios), at every size: the loop gain
// of an orthogonal matrix times these is under 1, so the network is
// stable whatever the settings. Also reports how far the matched shelves
// are from the analog attenuation they target.
int test_line_filters_stable() {
    double worst_margin = -1e9;  // the largest |H| in dB, over every case
    double worst_dev = 0.0;
    char where[160] = "";
    for (double fs : kRates) {
        for (double size : {0.0, 0.5, 1.0}) {
            std::uint32_t m[Fdn::kLines];
            Fdn::lengths_for(size, fs, m);
            for (double t60 : {0.2, 1.0, 5.0, 20.0}) {
                for (double lr : {0.5, 1.0, 2.0}) {
                    for (double hr : {0.1, 0.5, 1.0}) {
                        for (double lf : {50.0, 250.0, 1000.0}) {
                            for (double hf : {1000.0, 5000.0, 16000.0}) {
                                const DecaySpec spec{t60, lr, lf, hr, hf};
                                for (std::uint32_t i = 0; i < Fdn::kLines; i += 5) {
                                    const LineFilter f = design_line(spec, fs, m[i]);
                                    double peak = -1e9;
                                    for (int k = 0; k <= 400; ++k) {
                                        const double fr = 10.0 * std::pow(0.4999 * fs / 10.0, k / 400.0);
                                        const double h = f.gain * std::abs(response(f.low, fr, fs)) *
                                                         std::abs(response(f.high, fr, fs));
                                        const double db = 20.0 * std::log10(h);
                                        peak = std::fmax(peak, db);
                                        if (fr <= 20000.0) {
                                            const double want = line_attenuation_db(spec, fr, fs, m[i]);
                                            // Relative to the attenuation: that is the T60 error.
                                            const double dev = std::fabs(db - want) / std::fabs(want);
                                            if (dev > worst_dev) {
                                                worst_dev = dev;
                                                std::snprintf(where, sizeof where, "fs %g, t60 %g, low x%g at %g Hz, high x%g at %g Hz, at %.0f Hz",
                                                              fs, t60, lr, lf, hr, hf, fr);
                                            }
                                        }
                                    }
                                    if (!(peak < 0.0)) {
                                        std::fprintf(stderr, "unstable line: fs %g size %g t60 %g lr %g hr %g lf %g "
                                                     "hf %g m %u: peak %g dB\n", fs, size, t60, lr, hr, lf, hf, m[i], peak);
                                    }
                                    CHECK(peak < 0.0);
                                    worst_margin = std::fmax(worst_margin, peak);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    std::printf("line filters: largest |H| %.4f dB (below 0: stable); worst deviation from the analog "
                "attenuation %.1f %% of it (%s)\n", worst_margin, 100.0 * worst_dev, where);
    return 0;
}

// Freeze: an orthogonal matrix and no absorption keep the energy in the
// lines constant. Lines hold float, so each write rounds; the energy may
// wander by that, never grow or decay.
int test_freeze_is_lossless() {
    const double fs = 48000.0;
    Fdn f;
    std::uint32_t m[Fdn::kLines];
    Fdn::lengths_for(0.7, fs, m);
    f.prepare(fs, m[0] * 4, true);
    f.set_lengths(m, 0, true);
    f.set_decay({2.0, 1.0, 250.0, 0.5, 5000.0}, 0);
    f.reset();
    Noise n;
    double l, r;
    for (int i = 0; i < 24000; ++i) f.tick(n.next(), n.next(), 0.0, l, r);
    for (int i = 0; i < 100; ++i) f.tick(0.0, 0.0, 1.0, l, r);
    const double e0 = f.state_energy();
    for (int i = 0; i < 10 * 48000; ++i) f.tick(n.next(), n.next(), 1.0, l, r);  // input ignored
    const double e1 = f.state_energy();
    const double drift = std::fabs(e1 / e0 - 1.0);
    std::printf("freeze: line energy after 10 s, relative drift %.2e\n", drift);
    CHECK(e0 > 1.0);
    CHECK_LE(drift, 1e-4, "frozen energy drift over 10 s");
    return 0;
}

// The network alone, flat T60: the broadband energy decay curve
// (Schroeder backward integration) falls at 60 dB per T60.
int test_decay_rate() {
    double worst = 0.0;
    for (double fs : kRates) {
        for (double t60 : {0.5, 2.0}) {
            Fdn f;
            std::uint32_t m[Fdn::kLines];
            Fdn::lengths_for(0.6, fs, m);
            f.prepare(fs, m[0] * 4, true);
            f.set_lengths(m, 0, true);
            f.set_decay({t60, 1.0, 250.0, 1.0, 5000.0}, 0);
            f.reset();
            const std::size_t n = static_cast<std::size_t>(1.6 * t60 * fs);
            std::vector<double> e(n);
            double l, r;
            for (std::size_t i = 0; i < n; ++i) {
                f.tick(i == 0 ? 1.0 : 0.0, i == 0 ? 1.0 : 0.0, 0.0, l, r);
                e[i] = l * l + r * r;
            }
            for (std::size_t i = n - 1; i-- > 0;) e[i] += e[i + 1];
            // Least squares on the EDC between -5 and -35 dB.
            double sx = 0, sy = 0, sxx = 0, sxy = 0, cnt = 0;
            for (std::size_t i = 0; i < n; ++i) {
                const double db = 10.0 * std::log10(e[i] / e[0]);
                if (db > -5.0 || db < -35.0) continue;
                const double t = static_cast<double>(i) / fs;
                sx += t;
                sy += db;
                sxx += t * t;
                sxy += t * db;
                cnt += 1;
            }
            const double slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
            const double t30 = -60.0 / slope;
            std::printf("network decay, %g Hz, T60 %.1f s: T30 %.4f s (%+.2f %%)\n", fs, t60, t30,
                        100.0 * (t30 / t60 - 1.0));
            worst = std::fmax(worst, std::fabs(t30 / t60 - 1.0));
        }
    }
    CHECK_LE(worst, 0.02, "broadband T30 relative error");
    return 0;
}

int test_direct_term_and_fade() {
    const double fs = 48000.0;
    for (bool stereo : {true, false}) {
        Fdn f;
        std::uint32_t m[Fdn::kLines], m2[Fdn::kLines];
        Fdn::lengths_for(0.3, fs, m);
        Fdn::lengths_for(0.9, fs, m2);
        f.prepare(fs, m2[0] * 4, stereo);
        f.set_lengths(m, 0, true);
        f.set_decay({1.0, 1.0, 250.0, 1.0, 5000.0}, 0);
        f.reset();
        double l, r;
        // The direct term: 1/8 of each input in each output, exactly.
        f.tick(0.75, stereo ? -0.5 : 0.0, 0.0, l, r);
        CHECK(l == (stereo ? 0.03125 : 0.09375));
        CHECK(r == l);
        // A change of lengths: a crossfade, then the new lengths.
        f.set_lengths(m2, 1440, false);
        CHECK(f.fading());
        Noise n;
        double peak = 0.0;
        for (int i = 0; i < 48000; ++i) {
            f.tick(0.1 * n.next(), 0.1 * n.next(), 0.0, l, r);
            CHECK(std::isfinite(l) && std::isfinite(r));
            peak = std::fmax(peak, std::fabs(l));
        }
        CHECK(!f.fading());
        for (std::uint32_t i = 0; i < Fdn::kLines; ++i) CHECK(f.lengths()[i] == m2[i]);
        CHECK(peak < 10.0);
    }
    return 0;
}

int test_allpass() {
    SchroederAllpass a;
    a.prepare(113);
    const double g = 0.6;
    // First output sample: -g x, so a chain never delays an onset.
    CHECK(a.tick(1.0, g) == -g);
    a.reset();
    // Allpass: the energy of a long noise burst plus its tail is preserved.
    Noise n;
    double ein = 0.0, eout = 0.0;
    for (int i = 0; i < 200000; ++i) {
        const double x = i < 100000 ? n.next() : 0.0;
        const double y = a.tick(x, g);
        ein += x * x;
        eout += y * y;
    }
    CHECK_LE(std::fabs(eout / ein - 1.0), 1e-9, "allpass energy ratio error");
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_output_signs();
    failed += test_lengths();
    failed += test_line_filters_stable();
    failed += test_freeze_is_lossless();
    failed += test_decay_rate();
    failed += test_direct_term_and_fade();
    failed += test_allpass();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp fdn: all passed\n");
    return 0;
}
