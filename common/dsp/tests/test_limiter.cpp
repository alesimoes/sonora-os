// SPDX-License-Identifier: GPL-3.0-or-later
//
// The look-ahead true-peak limiter: latency measured equals latency
// reported, transparent below the ceiling, and zero true-peak overs on a
// stress set, judged by a 16x reference meter finer than the limiter's own
// 4x detector (ADR-0011 §3.2, "Ceiling").
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "check.h"
#include "sonora/dsp/fft.h"
#include "sonora/dsp/limiter.h"
#include "sonora/dsp/math.h"

namespace {

using namespace sonora::dsp;

using Chans = std::vector<std::vector<float>>;

void run(LookaheadLimiter& lim, Chans& x) {
    const std::size_t n = x[0].size();
    std::vector<float*> ptr(x.size());
    for (std::size_t pos = 0; pos < n; pos += 128) {
        const auto len = static_cast<std::uint32_t>(std::min<std::size_t>(128, n - pos));
        for (std::size_t c = 0; c < x.size(); ++c) ptr[c] = &x[c][pos];
        lim.process(ptr.data(), len);
    }
}

// 16x band-limited reconstruction: Kaiser-windowed sinc, +-256 taps, a
// longer support than the limiter's detector as well as a finer grid.
// Evaluated only between samples where the signal is within 6 dB of the
// ceiling, since a reconstruction cannot rise 6 dB above both neighbours
// of a band-limited signal at these levels.
double reference_true_peak(const std::vector<float>& y, double near_level) {
    const int half = 256, over = 16;
    const double beta = kaiser_beta(120.0);
    std::vector<double> table(over * (2 * half) + 1);
    for (int i = 0; i <= over * 2 * half; ++i) {
        const double t = static_cast<double>(i) / over - half;
        const double s = t == 0.0 ? 1.0 : std::sin(kPi * t) / (kPi * t);
        table[i] = s * kaiser(t / half, beta);
    }
    double peak = 0.0;
    const long n = static_cast<long>(y.size());
    for (long k = 0; k + 1 < n; ++k) {
        peak = std::fmax(peak, std::fabs(y[k]));
        if (std::fmax(std::fabs(y[k]), std::fabs(y[k + 1])) < near_level) continue;
        for (int p = 1; p < over; ++p) {
            double acc = 0.0;
            for (int j = -half + 1; j <= half; ++j) {
                const long idx = k + j;
                if (idx < 0 || idx >= n) continue;
                // time of the point is k + p/over; distance to idx is p/over - j
                const int ti = (half - j) * over + p;  // (p/over - j + half) * over
                acc += y[idx] * table[ti];
            }
            peak = std::fmax(peak, std::fabs(acc));
        }
    }
    return peak;
}

int test_latency_and_transparency() {
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        for (double ms : {0.5, 1.5, 5.0}) {
            const auto la = static_cast<std::uint32_t>(std::lround(ms * 1e-3 * fs));
            LookaheadLimiter lim;
            CHECK(lim.prepare(fs, 2, la));
            lim.set_ceiling(db_to_gain(-1.0));
            lim.set_release(0.05);
            Chans x(2, std::vector<float>(8192, 0.0f));
            std::uint32_t seed = 7;
            for (std::size_t i = 0; i < x[0].size(); ++i) {
                seed = seed * 1664525u + 1013904223u;
                x[0][i] = static_cast<float>(0.05 * ((seed >> 8) / 16777216.0 - 0.5));
                x[1][i] = -x[0][i];
            }
            x[0][1000] = 0.5f;  // impulse well under the ceiling
            const Chans in = x;
            run(lim, x);
            // Bit-exact delayed copy: below the ceiling the gain is exactly 1.
            const std::uint32_t L = lim.latency();
            for (std::size_t i = L; i < x[0].size(); ++i) {
                CHECK(x[0][i] == in[0][i - L]);
                CHECK(x[1][i] == in[1][i - L]);
            }
            std::size_t pk = 0;
            for (std::size_t i = 0; i < x[0].size(); ++i) {
                if (std::fabs(x[0][i]) > std::fabs(x[0][pk])) pk = i;
            }
            CHECK(pk - 1000 == L);
        }
    }
    return 0;
}

struct Signal {
    const char* name;
    std::function<Chans(double)> make;
};

// Programme material is band-limited: the stress signals are low-passed at
// 20 kHz (Kaiser-windowed sinc, +-255 taps, 100 dB) and faded in and out
// over 10 ms, the convention Tech 3341 uses for its own true-peak signals.
// A signal with full-scale content up to Nyquist has intersample peaks no
// finite detector sees in full; that case is measured and reported by
// report_full_band_noise(), not asserted.
Chans band_limit_and_fade(Chans x, double fs) {
    const int half = 255;
    const double fc = 20000.0 / fs;  // cycles per sample
    const double beta = kaiser_beta(100.0);
    std::vector<double> h(2 * half + 1);
    for (int k = -half; k <= half; ++k) {
        const double t = 2.0 * fc * k;
        const double sinc = k == 0 ? 1.0 : std::sin(kPi * t) / (kPi * t);
        h[k + half] = 2.0 * fc * sinc * kaiser(static_cast<double>(k) / half, beta);
    }
    for (auto& c : x) {
        std::vector<float> y(c.size());
        const long n = static_cast<long>(c.size());
        for (long i = 0; i < n; ++i) {
            double acc = 0.0;
            for (int k = -half; k <= half; ++k) {
                const long j = i - k;
                if (j >= 0 && j < n) acc += h[k + half] * c[j];
            }
            y[i] = static_cast<float>(acc);
        }
        const std::size_t f = static_cast<std::size_t>(0.01 * fs);
        for (std::size_t i = 0; i < f; ++i) {
            y[i] *= static_cast<float>(i) / f;
            y[y.size() - 1 - i] *= static_cast<float>(i) / f;
        }
        c = std::move(y);
    }
    return x;
}

Chans white_noise(double fs, double seconds, double amp, std::uint32_t seed) {
    Chans x(2, std::vector<float>(static_cast<std::size_t>(seconds * fs)));
    for (std::size_t i = 0; i < x[0].size(); ++i) {
        for (auto& c : x) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            c[i] = static_cast<float>(amp * (seed / 4294967296.0 * 2.0 - 1.0));
        }
    }
    return x;
}

std::vector<Signal> stress_set() {
    return {
        {"fs/4 sine at 45 deg, +12 dB", [](double fs) {
             Chans x(2, std::vector<float>(static_cast<std::size_t>(fs)));
             for (std::size_t i = 0; i < x[0].size(); ++i) {
                 x[0][i] = x[1][i] = static_cast<float>(4.0 * std::sin(kTwoPi * i / 4.0 + kPi / 4.0));
             }
             return band_limit_and_fade(x, fs);
         }},
        {"noise to 20 kHz, +12 dB", [](double fs) { return band_limit_and_fade(white_noise(fs, 1.0, 4.0, 99u), fs); }},
        {"noise to 20 kHz, +24 dB", [](double fs) { return band_limit_and_fade(white_noise(fs, 1.0, 16.0, 5u), fs); }},
        {"square 1 kHz to 20 kHz, +6 dB", [](double fs) {
             Chans x(2, std::vector<float>(static_cast<std::size_t>(fs)));
             for (std::size_t i = 0; i < x[0].size(); ++i) {
                 const double ph = std::fmod(1000.0 * i / fs, 1.0);
                 x[0][i] = x[1][i] = ph < 0.5 ? 2.0f : -2.0f;
             }
             return band_limit_and_fade(x, fs);
         }},
        {"log sweep 20 Hz-20 kHz, +12 dB", [](double fs) {
             const double T = 2.0, f1 = 20.0, f2 = 20000.0, k = std::log(f2 / f1);
             Chans x(2, std::vector<float>(static_cast<std::size_t>(T * fs)));
             for (std::size_t i = 0; i < x[0].size(); ++i) {
                 const double t = i / fs;
                 const double ph = kTwoPi * f1 * T / k * (std::exp(t / T * k) - 1.0);
                 x[0][i] = static_cast<float>(4.0 * std::sin(ph));
                 x[1][i] = static_cast<float>(4.0 * std::cos(ph));
             }
             return band_limit_and_fade(x, fs);
         }},
        {"drum-like bursts, +18 dB", [](double fs) {
             Chans x(2, std::vector<float>(static_cast<std::size_t>(2.0 * fs)));
             std::uint32_t seed = 31;
             for (std::size_t i = 0; i < x[0].size(); ++i) {
                 const double t = std::fmod(i / fs, 0.25);
                 seed = seed * 1664525u + 1013904223u;
                 const double nz = (seed >> 8) / 16777216.0 - 0.5;
                 const double v = 8.0 * std::exp(-t * 30.0) * std::sin(kTwoPi * 60.0 * t) +
                                  8.0 * std::exp(-t * 400.0) * nz;
                 x[0][i] = static_cast<float>(v);
                 x[1][i] = static_cast<float>(0.7 * v);
             }
             return band_limit_and_fade(x, fs);
         }},
        {"clipped sine 3 kHz to 20 kHz, +6 dB", [](double fs) {
             Chans x(2, std::vector<float>(static_cast<std::size_t>(fs)));
             for (std::size_t i = 0; i < x[0].size(); ++i) {
                 const double v = 4.0 * std::sin(kTwoPi * 3000.0 * i / fs);
                 x[0][i] = x[1][i] = static_cast<float>(std::fmax(-2.0, std::fmin(2.0, v)));
             }
             return band_limit_and_fade(x, fs);
         }},
    };
}

int test_no_true_peak_overs() {
    double worst_margin_db = -1e9;
    int bad = 0;
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        for (double ceiling_db : {-1.0, -0.1}) {
            for (double release : {0.005, 0.1}) {
                for (const Signal& s : stress_set()) {
                    LookaheadLimiter lim;
                    lim.prepare(fs, 2, static_cast<std::uint32_t>(std::lround(1.5e-3 * fs)));
                    lim.set_ceiling(db_to_gain(ceiling_db));
                    lim.set_release(release);
                    Chans x = s.make(fs);
                    run(lim, x);
                    const double near = db_to_gain(ceiling_db - 6.0);
                    double tp = 0.0;
                    for (const auto& c : x) tp = std::fmax(tp, reference_true_peak(c, near));
                    const double margin = gain_to_db(tp) - ceiling_db;
                    worst_margin_db = std::fmax(worst_margin_db, margin);
                    if (margin > 0.0) {
                        std::fprintf(stderr, "  over: %s, fs %g, ceiling %g, release %g: %+.4f dB\n",
                                     s.name, fs, ceiling_db, release, margin);
                        ++bad;
                    }
                }
            }
        }
    }
    std::printf("limiter stress set, band-limited to 20 kHz (7 signals x 3 rates x 2 ceilings x 2 releases): "
                "highest 16x true peak relative to ceiling %+.4f dB, overs %d\n",
                worst_margin_db, bad);
    CHECK(bad == 0);
    return 0;
}

// Full-scale content up to Nyquist (unfiltered white noise, +12 dB): not
// programme material, and not asserted. Printed so the figure is on record.
int report_full_band_noise() {
    for (double fs : {44100.0, 48000.0}) {
        LookaheadLimiter lim;
        lim.prepare(fs, 2, static_cast<std::uint32_t>(std::lround(1.5e-3 * fs)));
        lim.set_ceiling(db_to_gain(-1.0));
        lim.set_release(0.05);
        Chans x = white_noise(fs, 1.0, 4.0, 99u);
        run(lim, x);
        double tp = 0.0;
        for (const auto& c : x) tp = std::fmax(tp, reference_true_peak(c, db_to_gain(-7.0)));
        std::printf("report only: full-band white noise +12 dB at %g: 16x true peak %+.3f dB "
                    "relative to the ceiling\n", fs, gain_to_db(tp) + 1.0);
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_latency_and_transparency();
    failed += test_no_true_peak_overs();
    failed += report_full_band_noise();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp limiter: all passed\n");
    return 0;
}
