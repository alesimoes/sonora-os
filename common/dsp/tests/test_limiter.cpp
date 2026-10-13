// SPDX-License-Identifier: GPL-3.0-or-later
//
// The look-ahead true-peak limiter followed by its true-peak guard, the
// chain the native limiter runs: latency measured equals latency reported,
// transparent below the ceiling, and zero true-peak overs on the stress set
// and on full-band white noise, judged both by the BS.1770 4x meter and by
// a 16x reference finer than any detector of ours (ADR-0011 §3.2,
// "Ceiling"; coordinator's decision on PR #34: a mastering limiter never
// exceeds its ceiling).
//
// Slow: ctest labels it so. With SONORA_DSP_QUICK=1 (PR CI and the
// sanitizer builds) the stress set runs at one rate, one ceiling and one
// release; the nightly workflow runs all of it.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include "check.h"
#include "sonora/dsp/fft.h"
#include "sonora/dsp/limiter.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/reference_peak.h"
#include "sonora/dsp/true_peak.h"

namespace {

using namespace sonora::dsp;

using Chans = std::vector<std::vector<float>>;


// The chain as the native limiter builds it: the look-ahead stage, then the
// guard, with the ceiling handed to both.
struct Chain {
    LookaheadLimiter lim;
    TruePeakGuard guard;
    bool use_guard = true;
    std::vector<double> ceil;

    bool prepare(double fs, std::uint32_t la, std::uint32_t guard_la) {
        ceil.assign(128, 1.0);
        return lim.prepare(fs, 2, la) && guard.prepare(fs, 2, 128, guard_la);
    }
    std::uint32_t latency() const { return lim.latency() + (use_guard ? guard.latency() : 0); }
    void set(double ceiling_db, double release) {
        lim.set_ceiling(db_to_gain(ceiling_db));
        lim.set_release(release);
        guard.set_release(release);
        for (double& c : ceil) c = db_to_gain(ceiling_db);
    }
    void run(Chans& x) {
        const std::size_t n = x[0].size();
        std::vector<float*> ptr(x.size());
        for (std::size_t pos = 0; pos < n; pos += 128) {
            const auto len = static_cast<std::uint32_t>(std::min<std::size_t>(128, n - pos));
            for (std::size_t c = 0; c < x.size(); ++c) ptr[c] = &x[c][pos];
            lim.process(ptr.data(), len);
            if (use_guard) guard.process(ptr.data(), ceil.data(), len);
        }
    }
};

std::uint32_t ms(double milliseconds, double fs) {
    return static_cast<std::uint32_t>(std::lround(milliseconds * 1e-3 * fs));
}

// The guard's look-ahead, as the native limiter sets it.
std::uint32_t guard_la(double fs) { return ms(1.0, fs); }

// What a BS.1770-5 Annex 2 meter reads: the meter's own 4x detector,
// 32 taps per phase, no refinement (loudness.h).
double meter_true_peak(const std::vector<float>& y) {
    TruePeakDetector tp;
    tp.prepare(1);
    double m = 0.0;
    for (float v : y) m = std::fmax(m, tp.process(0, v));
    for (std::uint32_t i = 0; i < 2 * tp.delay(); ++i) m = std::fmax(m, tp.process(0, 0.0));
    return m;
}

double reference(const std::vector<float>& y, double ceiling_db) {
    // Exact wherever the reconstruction comes within 1 dB of the ceiling.
    return reference_true_peak(y.data(), y.size(), db_to_gain(ceiling_db - 1.0));
}

// The reference meter evaluates its 16x reconstruction only around the
// pre-scan's peaks. Against evaluating it everywhere, on signals whose
// peaks sit where a sample-level shortcut would not look: a tone near
// Nyquist whose samples pass through zero, fs/4 at 45 degrees, noise.
int test_reference_meter_finds_every_peak() {
    const int half = 256, over = 16;
    std::vector<double> table(over * 2 * half + 1);
    for (int i = 0; i <= over * 2 * half; ++i) {
        const double t = static_cast<double>(i) / over - half;
        table[i] = (t == 0.0 ? 1.0 : std::sin(kPi * t) / (kPi * t)) * kaiser(t / half, kaiser_beta(120.0));
    }
    auto everywhere = [&](const std::vector<float>& y) {
        double pk = 0.0;
        const long n = static_cast<long>(y.size());
        for (long k = 0; k < n; ++k) {
            pk = std::fmax(pk, std::fabs(y[k]));
            for (int p = 1; p < over; ++p) {
                double acc = 0.0;
                for (int j = -half + 1; j <= half; ++j) {
                    if (k + j >= 0 && k + j < n) acc += y[k + j] * table[(half - j) * over + p];
                }
                pk = std::fmax(pk, std::fabs(acc));
            }
        }
        return pk;
    };
    std::vector<std::vector<float>> cases(3, std::vector<float>(3000));
    std::uint32_t seed = 3;
    for (std::size_t i = 0; i < 3000; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * i / 3000.0);  // no edges
        cases[0][i] = static_cast<float>(w * std::sin(kPi * 0.49 * i));
        cases[1][i] = static_cast<float>(w * std::sin(kTwoPi * i / 4.0 + kPi / 4.0));
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        cases[2][i] = static_cast<float>(w * (seed / 4294967296.0 * 2.0 - 1.0));
    }
    for (const auto& y : cases) {
        const double full = everywhere(y);
        const double fast = reference_true_peak(y.data(), y.size(), 0.5 * full);
        std::printf("reference meter: around peaks %.9f, everywhere %.9f\n", fast, full);
        CHECK(std::fabs(fast - full) <= 1e-12 * full);
    }
    return 0;
}

int test_latency_and_transparency() {
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        for (double la_ms : {0.5, 1.5, 5.0}) {
            Chain ch;
            CHECK(ch.prepare(fs, ms(la_ms, fs), guard_la(fs)));
            ch.set(-1.0, 0.05);
            Chans x(2, std::vector<float>(8192, 0.0f));
            std::uint32_t seed = 7;
            for (std::size_t i = 0; i < x[0].size(); ++i) {
                seed = seed * 1664525u + 1013904223u;
                x[0][i] = static_cast<float>(0.05 * ((seed >> 8) / 16777216.0 - 0.5));
                x[1][i] = -x[0][i];
            }
            x[0][1000] = 0.5f;  // impulse well under the ceiling
            const Chans in = x;
            ch.run(x);
            // Bit-exact delayed copy: below the ceiling both gains are exactly 1.
            const std::uint32_t L = ch.latency();
            for (std::size_t i = L; i < x[0].size(); ++i) {
                CHECK(x[0][i] == in[0][i - L]);
                CHECK(x[1][i] == in[1][i - L]);
            }
            std::size_t pk = 0;
            for (std::size_t i = 0; i < x[0].size(); ++i) {
                if (std::fabs(x[0][i]) > std::fabs(x[0][pk])) pk = i;
            }
            CHECK(pk - 1000 == L);
            CHECK(ch.guard.clamped() == 0);
        }
    }
    return 0;
}

struct Signal {
    const char* name;
    std::function<Chans(double)> make;
};

// Programme material is band-limited: most stress signals are low-passed at
// 20 kHz (Kaiser-windowed sinc, +-255 taps, 100 dB) and faded in and out
// over 10 ms, the convention Tech 3341 uses for its own true-peak signals.
// The full-band noise cases are not: content at full scale up to Nyquist,
// with an abrupt start, is the case the first stage alone cannot hold.
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
        {"full-band white noise, +6 dB", [](double fs) { return white_noise(fs, 1.0, 2.0, 99u); }},
        {"full-band white noise, +12 dB", [](double fs) { return white_noise(fs, 1.0, 4.0, 99u); }},
        {"full-band white noise, +12 dB, second seed", [](double fs) { return white_noise(fs, 1.0, 4.0, 12345u); }},
        {"full-band white noise, +24 dB", [](double fs) { return white_noise(fs, 1.0, 16.0, 7u); }},
    };
}

// Silence after the signal, so the output holds all of what the chain
// makes of it. Cut off at the input's length, the output would end in the
// middle of the delayed signal, and the reference would reconstruct that
// edge as a step with its own overshoot.
Chans with_tail(Chans x, std::uint32_t latency) {
    for (auto& c : x) c.resize(c.size() + latency + 1024, 0.0f);
    return x;
}

struct Matrix {
    std::vector<double> rates, ceilings, releases;
};

Matrix matrix() {
    if (dsp_quick()) return {{48000.0}, {-1.0}, {0.005}};
    return {{44100.0, 48000.0, 96000.0}, {-1.0, -0.1}, {0.005, 0.1}};
}

int test_no_true_peak_overs() {
    const Matrix mx = matrix();
    double worst_ref = -1e9, worst_meter = -1e9;
    int bad = 0, runs = 0;
    const std::vector<Signal> set = stress_set();
    std::vector<double> per_ref(set.size(), -1e9), per_meter(set.size(), -1e9);
    for (double fs : mx.rates) {
        for (double ceiling_db : mx.ceilings) {
            for (double release : mx.releases) {
                for (std::size_t si = 0; si < set.size(); ++si) {
                    const Signal& s = set[si];
                    Chain ch;
                    ch.prepare(fs, ms(1.5, fs), guard_la(fs));
                    ch.set(ceiling_db, release);
                    Chans x = with_tail(s.make(fs), ch.latency());
                    ch.run(x);
                    ++runs;
                    double ref = 0.0, meter = 0.0;
                    for (const auto& c : x) {
                        ref = std::fmax(ref, reference(c, ceiling_db));
                        meter = std::fmax(meter, meter_true_peak(c));
                    }
                    const double m_ref = gain_to_db(ref) - ceiling_db;
                    const double m_meter = gain_to_db(meter) - ceiling_db;
                    worst_ref = std::fmax(worst_ref, m_ref);
                    per_ref[si] = std::fmax(per_ref[si], m_ref);
                    per_meter[si] = std::fmax(per_meter[si], m_meter);
                    worst_meter = std::fmax(worst_meter, m_meter);
                    if (m_ref > 0.0 || m_meter > 0.0 || ch.guard.clamped() != 0) {
                        std::fprintf(stderr,
                                     "  over: %s, fs %g, ceiling %g, release %g: 16x %+.4f dB, "
                                     "4x meter %+.4f dB, clamped %llu\n",
                                     s.name, fs, ceiling_db, release, m_ref, m_meter,
                                     static_cast<unsigned long long>(ch.guard.clamped()));
                        ++bad;
                    }
                }
            }
        }
    }
    for (std::size_t si = 0; si < set.size(); ++si) {
        std::printf("  %-44s highest: 16x %+.4f dB, 4x meter %+.4f dB\n", set[si].name, per_ref[si], per_meter[si]);
    }
    std::printf("limiter + guard, stress set incl. full-band noise (%d runs%s): highest true peak relative "
                "to the ceiling: 16x reference %+.4f dB, BS.1770 4x meter %+.4f dB; overs %d\n",
                runs, dsp_quick() ? ", quick" : "", worst_ref, worst_meter, bad);
    CHECK(bad == 0);
    return 0;
}

// What the guard costs, on record: the first stage alone against the
// chain, on the same input. The figures are the highest over without the
// guard; with it, the deepest cut it made, the energy of the change it
// makes relative to the output (the chain against the first stage delayed
// by the guard's latency), and how much of the time it acts at all.
// Reported for the research note; the bars it must meet are the overs
// above, and the aliasing and THD+N on tones in the native limiter's suite
// (which also compares the chain with the first stage alone there).
int report_guard_cost() {
    const double fs = 48000.0, ceiling_db = -1.0, release = 0.05;
    const std::vector<Signal> set = stress_set();
    for (const Signal& s : set) {
        Chain alone, chain;
        alone.use_guard = false;
        alone.prepare(fs, ms(1.5, fs), guard_la(fs));
        chain.prepare(fs, ms(1.5, fs), guard_la(fs));
        alone.set(ceiling_db, release);
        chain.set(ceiling_db, release);
        const Chans in = with_tail(s.make(fs), chain.latency());
        Chans a = in, b = in;
        alone.run(a);
        chain.run(b);
        double over = 0.0;
        for (const auto& c : a) over = std::fmax(over, reference(c, ceiling_db));
        const std::size_t G = chain.guard.latency();
        double err = 0.0, sig = 0.0;
        std::size_t acting = 0, total = 0;
        for (std::size_t c = 0; c < 2; ++c) {
            for (std::size_t i = G; i < b[c].size(); ++i) {
                const double d = static_cast<double>(b[c][i]) - a[c][i - G];
                err += d * d;
                sig += static_cast<double>(b[c][i]) * b[c][i];
                acting += d != 0.0 ? 1 : 0;
                ++total;
            }
        }
        // The deepest cut the guard made; the rest of the change is that
        // cut, ramped, since it acts by gain alone.
        const double deepest = chain.guard.take_gain_reduction_db();
        std::printf("report: %-44s first stage alone %+.3f dB over; with the guard: deepest cut %.3f dB, "
                    "change %6.1f dB relative to the output, on %5.2f %% of samples\n",
                    s.name, gain_to_db(over) - ceiling_db, deepest,
                    err > 0.0 ? 10.0 * std::log10(err / sig) : -200.0,
                    100.0 * static_cast<double>(acting) / static_cast<double>(total));
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_reference_meter_finds_every_peak();
    failed += test_latency_and_transparency();
    failed += test_no_true_peak_overs();
    if (!dsp_quick()) failed += report_guard_cost();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp limiter: all passed\n");
    return 0;
}
