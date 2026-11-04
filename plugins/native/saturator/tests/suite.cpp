// SPDX-License-Identifier: GPL-3.0-or-later
//
// The saturator against its bars (quality.json).
// - Small-signal gain: unity at every drive, shape and frequency (the drive
//   is divided out), for the saturate mode and the exciter.
// - The curve itself: the output's harmonics against the Fourier series of
//   the stated static function, computed here in double (the reference is
//   the function, not a circuit), for each shape at three levels.
// - The exciter: the low band untouched, the added harmonics against the
//   curve's series at the high-passed level (the split's exact response).
// - The tone shelf against its analytic first-order response.
// - Aliasing per quality and shape, at tones from 1 to 19 kHz.
// - Hot input, and the common checks (latency in every configuration,
//   non-finite, denormals, zipper, golden files, CPU).
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/saturator.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;
using cplx = std::complex<double>;

std::string hz(double fs) { return std::to_string(static_cast<int>(fs)) + " Hz"; }

std::string fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
    char buf[240];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

json::Value jf(double v) { return json::Value::number_float(static_cast<float>(v)); }

const char* const kQualityIds[] = {"eco", "standard", "high"};
const char* const kShapeIds[] = {"soft", "warm", "hard"};

std::unique_ptr<Processor> measuring(const Suite& s, double fs, std::uint32_t channels, const Config& c) {
    auto p = s.make();
    // Plain wet signal unless asked: no tone, full mix, unity output.
    measure::apply(*p, {{"tone", 0.0f}, {"mix", 100.0f}, {"output", 0.0f}});
    measure::apply(*p, c);
    p->prepare(fs, 128, channels);
    return p;
}

Chans aligned(Processor& p, const Chans& x) {
    const std::uint32_t lat = p.latency();
    Chans xp = x;
    for (auto& c : xp) c.resize(c.size() + lat, 0.0f);
    Chans y = measure::render(p, xp);
    for (auto& c : y) c.erase(c.begin(), c.begin() + lat);
    return y;
}

// ---- the stated functions (independent of the processor's source) -------------------

double curve(int shape, double u) {
    switch (shape) {
        case 1: {
            const double t = std::tanh(saturator::kWarmBias);
            return (std::tanh(u + saturator::kWarmBias) - t) / (1.0 - t * t);
        }
        case 2: {
            const double c = std::fmax(-1.0, std::fmin(1.0, u));
            return c - c * c * c / 3.0;
        }
        default:
            return std::tanh(u);
    }
}

// Amplitude of harmonic h of curve(shape, a sin(theta)), by the trapezoid
// rule over one period (spectrally exact for a periodic smooth function;
// the hard curve's kinks are resolved by the point count).
double curve_harmonic(int shape, double a, int h) {
    const int n = 1 << 16;
    double re = 0.0, im = 0.0;
    for (int i = 0; i < n; ++i) {
        const double th = dsp::kTwoPi * i / n;
        const double v = curve(shape, a * std::sin(th));
        re += v * std::cos(h * th);
        im += v * std::sin(h * th);
    }
    return 2.0 * std::hypot(re, im) / n;
}

// Amplitude of the component at f in y from `from`, Hann-windowed so a
// non-integer number of periods does not leak.
double tone_amp(const std::vector<float>& y, std::size_t from, double fs, double f) {
    const std::size_t n = y.size() - from;
    double re = 0.0, im = 0.0, wsum = 0.0;
    const double w0 = dsp::kTwoPi * f / fs;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(dsp::kTwoPi * static_cast<double>(i) / static_cast<double>(n));
        const double ph = w0 * static_cast<double>(from + i);
        re += w * y[from + i] * std::cos(ph);
        im -= w * y[from + i] * std::sin(ph);
        wsum += w;
    }
    return 2.0 * std::hypot(re, im) / wsum;
}

double db(double lin) { return 20.0 * std::log10(std::fmax(lin, 1e-30)); }

// ---- small-signal gain --------------------------------------------------------------

void small_signal(Suite& s) {
    const double fs = 48000.0;
    const double freqs[] = {100.0, 1000.0, 5000.0, 10000.0, 16000.0};
    double worst = 0.0, worst_f = 0.0;
    int worst_shape = 0;
    float worst_drive = 0.0f;
    for (int sh = 0; sh < 3; ++sh) {
        for (float drive : {0.0f, 12.0f, 36.0f}) {
            for (int mode = 0; mode < 2; ++mode) {
                for (double f : freqs) {
                    if (measure::quick() && mode == 1 && f != 1000.0 && f != 10000.0) continue;
                    auto p = measuring(s, fs, 1, {{"shape", static_cast<float>(sh)}, {"drive", drive},
                                                  {"mode", static_cast<float>(mode)}, {"quality", 1.0f}});
                    const double a = -80.0;
                    const Chans y = aligned(*p, measure::sine(fs, f, a, 0.5, 1));
                    const double g = db(tone_amp(y[0], 4800, fs, f)) - a;
                    if (std::fabs(g) > std::fabs(worst)) {
                        worst = g;
                        worst_f = f;
                        worst_shape = sh;
                        worst_drive = drive;
                    }
                }
            }
        }
    }
    s.check_le("small-signal gain at -80 dBFS, 100 Hz-16 kHz, every shape, drive 0/12/36 dB, both modes: worst |dB|",
               std::fabs(worst), s.bar("small_signal_db"), "dB",
               fmt("%+.4f dB at %.0f Hz, ", worst, worst_f) + kShapeIds[worst_shape] + fmt(", drive %.0f dB", worst_drive));
    // Output and mix are plain gains.
    {
        auto p = measuring(s, fs, 1, {{"drive", 0.0f}, {"output", 6.0f}});
        const Chans y = aligned(*p, measure::sine(fs, 1000.0, -60.0, 0.5, 1));
        const double g = db(tone_amp(y[0], 4800, fs, 1000.0)) + 60.0;
        s.check_le("output +6 dB: gain error", std::fabs(g - 6.0), s.bar("small_signal_db"), "dB");
    }
    {
        // Mix 50 % of a unity-gain wet and the dry: still unity.
        auto p = measuring(s, fs, 1, {{"drive", 12.0f}, {"mix", 50.0f}});
        const Chans y = aligned(*p, measure::sine(fs, 1000.0, -80.0, 0.5, 1));
        const double g = db(tone_amp(y[0], 4800, fs, 1000.0)) + 80.0;
        s.check_le("mix 50 %, small signal: gain error", std::fabs(g), s.bar("small_signal_db"), "dB");
    }
}

// ---- the curve --------------------------------------------------------------------

struct Level {
    float drive;
    double dbfs;
};

void harmonics(Suite& s) {
    const double f0 = 1000.0;
    // Mild, bending, and well into the curve (u peak 0.4, 1.0, 4.0).
    const Level levels[] = {{12.0f, -20.0}, {6.0f, -6.0}, {24.0f, -12.0}};
    const std::vector<double> rates =
        measure::quick() ? std::vector<double>{48000.0} : std::vector<double>{44100.0, 48000.0, 96000.0};
    json::Value rows = json::Value::array();
    for (int sh = 0; sh < 3; ++sh) {
        double worst = 0.0, h2_ref = -400.0, h3_ref = -400.0;
        int count = 0;
        for (const Level& lv : levels) {
            for (double fs : rates) {
                auto p = measuring(s, fs, 1, {{"shape", static_cast<float>(sh)}, {"drive", lv.drive}, {"quality", 1.0f}});
                const Chans y = aligned(*p, measure::sine(fs, f0, lv.dbfs, 1.0, 1));
                const double u = std::pow(10.0, (lv.drive + lv.dbfs) / 20.0);
                const double ref1 = curve_harmonic(sh, u, 1);
                const double m1 = tone_amp(y[0], static_cast<std::size_t>(0.2 * fs), fs, f0);
                for (int h = 2; h <= 9; ++h) {
                    const double rdb = db(curve_harmonic(sh, u, h) / ref1);
                    const double mdb = db(tone_amp(y[0], static_cast<std::size_t>(0.2 * fs), fs, h * f0) / m1);
                    json::Value r = json::Value::object();
                    r.set("shape", json::Value::integer(sh));
                    r.set("drive", jf(lv.drive));
                    r.set("dbfs", jf(lv.dbfs));
                    r.set("rate", jf(fs));
                    r.set("h", json::Value::integer(h));
                    r.set("ref_dbc", jf(rdb));
                    r.set("meas_dbc", jf(mdb));
                    rows.push(r);
                    if (fs == 48000.0 && lv.drive == 12.0f && h == 2) h2_ref = rdb;
                    if (fs == 48000.0 && lv.drive == 12.0f && h == 3) h3_ref = rdb;
                    // Held where the reference stands above the floor the
                    // measurement can resolve.
                    if (rdb < s.bar("harmonics.floor_dbc")) continue;
                    ++count;
                    worst = std::fmax(worst, std::fabs(mdb - rdb));
                }
            }
        }
        s.check_le(std::string("harmonics 2-9 of 1 kHz vs the curve's Fourier series, ") + kShapeIds[sh] +
                       ", 3 levels, standard: worst |dB|",
                   worst, s.bar("harmonics.db"), "dB", fmt("%.0f harmonics held", count));
        s.info(std::string(kShapeIds[sh]) + ": H2 and H3 of the reference at drive 12 dB, -20 dBFS", h2_ref, "dBc",
               fmt("H3 %.1f dBc", h3_ref));
    }
    s.data().set("harmonics", rows);
    // The family each shape belongs to: soft and hard are odd-symmetric (no
    // even harmonics at all), warm leads with the second.
    for (int sh : {0, 2}) {
        auto p = measuring(s, 48000.0, 1, {{"shape", static_cast<float>(sh)}, {"drive", 18.0f}});
        const Chans y = aligned(*p, measure::sine(48000.0, 1000.0, -12.0, 1.0, 1));
        const double m1 = tone_amp(y[0], 9600, 48000.0, 1000.0);
        const double h2 = db(tone_amp(y[0], 9600, 48000.0, 2000.0) / m1);
        s.check_le(std::string(kShapeIds[sh]) + ": second harmonic of an odd curve", h2, s.bar("harmonics.odd_h2_dbc"), "dBc");
    }
    {
        auto p = measuring(s, 48000.0, 1, {{"shape", 1.0f}, {"drive", 12.0f}});
        const Chans y = aligned(*p, measure::sine(48000.0, 1000.0, -30.0, 1.0, 1));
        const double m1 = tone_amp(y[0], 9600, 48000.0, 1000.0);
        const double h2 = db(tone_amp(y[0], 9600, 48000.0, 2000.0) / m1);
        const double h3 = db(tone_amp(y[0], 9600, 48000.0, 3000.0) / m1);
        s.check("warm: the second harmonic leads the third at -30 dBFS, drive 12 dB (H2 - H3)", h2 - h3, 0.0, h2 > h3, "dB");
    }
}

// ---- the exciter ----------------------------------------------------------------

// The split's high-pass at f for focus fc: two cascaded 1 - LP, LP the
// one-pole of pole p, evaluated on the unit circle.
double split_gain(double f, double fc, double fs) {
    const double p = std::exp(-dsp::kTwoPi * fc / fs);
    const cplx z1 = std::polar(1.0, -dsp::kTwoPi * f / fs);
    const cplx lp = (1.0 - p) / (1.0 - p * z1);
    return std::abs((1.0 - lp) * (1.0 - lp));
}

void exciter(Suite& s) {
    const double fs = 48000.0;
    {
        // Below the split the signal is untouched: a 100 Hz tone at -6 dBFS
        // and drive 24 dB comes out as the input, delayed.
        auto p = measuring(s, fs, 1, {{"mode", 1.0f}, {"drive", 24.0f}, {"quality", 1.0f}});
        const Chans x = measure::sine(fs, 100.0, -6.0, 1.0, 1);
        const Chans y = aligned(*p, x);
        double worst = 0.0;
        for (std::size_t i = 4800; i < x[0].size(); ++i) worst = std::fmax(worst, std::fabs(y[0][i] - x[0][i]));
        s.check_le("exciter: 100 Hz at -6 dBFS, drive 24 dB, focus 3 kHz: max |out - in|", worst,
                   s.bar("exciter.low_band_fs"), "FS");
    }
    // The added harmonics of a 3 kHz tone against the curve's series at the
    // level the split passes: |F_h(U)| / drive, U = A drive |HP(f0)|.
    const double f0 = 3000.0, fc = 3000.0;
    json::Value rows = json::Value::array();
    for (int sh = 0; sh < 3; ++sh) {
        double worst = 0.0;
        int count = 0;
        for (float drive : {12.0f, 24.0f}) {
            auto p = measuring(s, fs, 1, {{"shape", static_cast<float>(sh)}, {"mode", 1.0f}, {"drive", drive},
                                          {"focus", static_cast<float>(fc)}, {"quality", 1.0f}});
            const double dbfs = -12.0;
            const Chans y = aligned(*p, measure::sine(fs, f0, dbfs, 1.0, 1));
            const double g = std::pow(10.0, drive / 20.0);
            const double u = std::pow(10.0, dbfs / 20.0) * g * split_gain(f0, fc, fs);
            for (int h = 2; h <= 6; ++h) {
                const double ref = curve_harmonic(sh, u, h) / g;
                const double meas = tone_amp(y[0], 9600, fs, h * f0);
                json::Value r = json::Value::object();
                r.set("shape", json::Value::integer(sh));
                r.set("drive", jf(drive));
                r.set("h", json::Value::integer(h));
                r.set("ref_dbfs", jf(db(ref)));
                r.set("meas_dbfs", jf(db(meas)));
                rows.push(r);
                if (db(ref) < s.bar("exciter.floor_dbfs")) continue;
                ++count;
                worst = std::fmax(worst, std::fabs(db(meas) - db(ref)));
            }
        }
        s.check_le(std::string("exciter: harmonics 2-6 of 3 kHz vs the curve at the split level, ") + kShapeIds[sh] +
                       ": worst |dB|",
                   worst, s.bar("exciter.harmonic_db"), "dB", fmt("%.0f harmonics held", count));
    }
    s.data().set("exciter", rows);
    {
        // The mix is the amount of what the exciter adds: half the mix, half
        // the harmonic.
        double full = 0.0, half = 0.0;
        for (float mix : {100.0f, 50.0f}) {
            auto p = measuring(s, fs, 1, {{"mode", 1.0f}, {"drive", 18.0f}, {"mix", mix}, {"focus", 3000.0f}});
            const Chans y = aligned(*p, measure::sine(fs, f0, -12.0, 1.0, 1));
            (mix == 100.0f ? full : half) = tone_amp(y[0], 9600, fs, 3.0 * f0);
        }
        s.check_le("exciter: mix 50 % halves the added 3rd harmonic: error", std::fabs(db(half / full) + 6.0206),
                   s.bar("exciter.mix_db"), "dB");
    }
}

// ---- tone -------------------------------------------------------------------------

void tone(Suite& s) {
    const double fs = 48000.0;
    const double p = std::exp(-dsp::kTwoPi * saturator::kToneHz / fs);
    double worst = 0.0;
    for (float t : {-12.0f, -6.0f, 6.0f, 12.0f}) {
        for (double f : {100.0, 1000.0, 5000.0, 10000.0}) {
            auto proc = measuring(s, fs, 1, {{"tone", t}, {"drive", 0.0f}, {"quality", 1.0f}});
            const double a = -80.0;
            const Chans y = aligned(*proc, measure::sine(fs, f, a, 0.5, 1));
            const double meas = db(tone_amp(y[0], 4800, fs, f)) - a;
            const cplx z1 = std::polar(1.0, -dsp::kTwoPi * f / fs);
            const cplx lp = (1.0 - p) / (1.0 - p * z1);
            const double g = std::pow(10.0, t / 20.0);
            const double expect = db(std::abs(lp + g * (1.0 - lp)));
            worst = std::fmax(worst, std::fabs(meas - expect));
        }
    }
    s.check_le("tone +-6, +-12 dB at 0.1-10 kHz vs the first-order shelf: worst |dB|", worst, s.bar("tone_db"), "dB");
}

// ---- aliasing ---------------------------------------------------------------------

void aliasing(Suite& s) {
    const double tones[] = {997.0, 2503.0, 5011.0, 7993.0, 10007.0, 14009.0, 19001.0};
    struct Lv {
        const char* name;
        float drive;
        double dbfs;
    };
    // Nominal is the default drive with a -18 dBFS tone (EBU R68's alignment
    // level, as the tube's); ADR-0011 §3.2 bars it over the whole 1-19 kHz
    // sweep. The hot level is barred to 8 kHz, per shape and quality, at the
    // figures measured here; the last is reported.
    const Lv levels[] = {
        {"nominal (drive 12 dB, -18 dBFS)", 12.0f, -18.0},
        {"hot (drive 18 dB, -6 dBFS)", 18.0f, -6.0},
        {"drive 36 dB, -6 dBFS", 36.0f, -6.0},
    };
    const char* nominal_bars[3] = {"aliasing.nominal.eco_dbc", "aliasing.nominal.standard_dbc",
                                   "aliasing.nominal.high_dbc"};
    const std::vector<double> rates =
        measure::quick() ? std::vector<double>{44100.0} : std::vector<double>{44100.0, 48000.0, 96000.0};
    const char* bars[3][3] = {{"aliasing.soft.eco_dbc", "aliasing.soft.standard_dbc", "aliasing.soft.high_dbc"},
                              {"aliasing.warm.eco_dbc", "aliasing.warm.standard_dbc", "aliasing.warm.high_dbc"},
                              {"aliasing.hard.eco_dbc", "aliasing.hard.standard_dbc", "aliasing.hard.high_dbc"}};
    json::Value rows = json::Value::array();
    for (int sh = 0; sh < 3; ++sh) {
        for (int q = 0; q < 3; ++q) {
            if (measure::quick() && q == 1) continue;
            for (double fs : rates) {
                for (std::size_t lv = 0; lv < (measure::quick() ? 2u : std::size(levels)); ++lv) {
                    double worst = -400.0, worst_f = 0.0, worst_all = -400.0, worst_all_f = 0.0;
                    for (double f0 : tones) {
                        if (f0 > 0.45 * fs) continue;
                        if (measure::quick() && f0 != 997.0 && f0 != 7993.0 && f0 != 19001.0) continue;
                        auto p = measuring(s, fs, 1, {{"shape", static_cast<float>(sh)}, {"quality", static_cast<float>(q)},
                                                      {"drive", levels[lv].drive}});
                        const Chans y = aligned(*p, measure::sine(fs, f0, levels[lv].dbfs, 1.5, 1));
                        const double a = measure::alias_dbc(y[0], static_cast<std::size_t>(0.6 * fs), fs, f0);
                        if (a > worst_all) {
                            worst_all = a;
                            worst_all_f = f0;
                        }
                        if (f0 <= 8000.0 && a > worst) {
                            worst = a;
                            worst_f = f0;
                        }
                        json::Value r = json::Value::object();
                        r.set("shape", json::Value::integer(sh));
                        r.set("quality", json::Value::integer(q));
                        r.set("rate", jf(fs));
                        r.set("tone", jf(f0));
                        r.set("level", json::Value::integer(static_cast<std::int64_t>(lv)));
                        r.set("alias_dbc", jf(a));
                        rows.push(r);
                    }
                    const std::string base = std::string("aliasing, ") + kShapeIds[sh] + ", " + kQualityIds[q] + ", " +
                                             hz(fs) + ", " + levels[lv].name;
                    if (lv == 0 && q == 0) {
                        // Eco meets ADR-0011 3.2's -70 dBc to 8 kHz. Above it a
                        // 2x oversampler folds the 4th and 5th harmonics of a
                        // 10-19 kHz tone into the band; that is measured, held
                        // to a ceiling of its own and reported in the note.
                        s.check_le(base + ", tones 1-8 kHz: worst non-harmonic", worst, s.bar(nominal_bars[0]), "dBc",
                                   fmt("at %.0f Hz", worst_f));
                        s.check_le(base + ", tones 1-19 kHz: worst non-harmonic", worst_all,
                                   s.bar("aliasing.nominal.eco_all_dbc"), "dBc", fmt("at %.0f Hz", worst_all_f));
                    } else if (lv == 0) {
                        s.check_le(base + ", tones 1-19 kHz: worst non-harmonic", worst_all, s.bar(nominal_bars[q]), "dBc",
                                   fmt("at %.0f Hz", worst_all_f));
                    } else if (lv == 1) {
                        s.check_le(base + ", tones 1-8 kHz: worst non-harmonic", worst, s.bar(bars[sh][q]), "dBc",
                                   fmt("at %.0f Hz", worst_f));
                        s.info(base + ", tones 1-19 kHz: worst non-harmonic (not barred)", worst_all, "dBc",
                               fmt("at %.0f Hz", worst_all_f));
                    } else {
                        s.info(base + ", tones 1-19 kHz: worst non-harmonic (not barred)", worst_all, "dBc",
                               fmt("at %.0f Hz", worst_all_f));
                    }
                }
            }
        }
    }
    s.data().set("aliasing", rows);
}

// ---- hot input ----------------------------------------------------------------------

void hot_input(Suite& s) {
    const double fs = 48000.0;
    for (int sh = 0; sh < 3; ++sh) {
        for (int mode = 0; mode < 2; ++mode) {
            double worst = 0.0;
            bool finite = true;
            for (double dbfs : {0.0, 12.0, 24.0}) {
                auto p = measuring(s, fs, 2, {{"shape", static_cast<float>(sh)}, {"mode", static_cast<float>(mode)},
                                              {"drive", 36.0f}, {"quality", 0.0f}});
                Chans x = measure::silence(fs, 0.3);
                const auto amp = static_cast<float>(std::pow(10.0, dbfs / 20.0));
                for (auto& ch : x) {
                    for (std::size_t i = 0; i < ch.size(); ++i) {
                        const double ph = std::fmod(220.0 * static_cast<double>(i) / fs, 1.0);
                        ch[i] = ph < 0.5 ? amp : -amp;
                    }
                }
                const Chans y = measure::render(*p, x);
                for (const auto& ch : y) {
                    for (float v : ch) {
                        finite &= std::isfinite(v);
                        worst = std::fmax(worst, std::fabs(v));
                    }
                }
            }
            // Saturate mode is bounded by the curve's range over the drive.
            // The exciter passes the dry signal (+24 dBFS at most) and
            // replaces its high part with one the oversampler has band-
            // limited: the difference rings at a full-scale square's edges
            // (Gibbs, about 19 % measured), so the bound grows by 25 %.
            const double bound = mode == 0 ? s.bar("hot.max_abs") : s.bar("hot.max_abs") + 1.25 * std::pow(10.0, 24.0 / 20.0);
            s.check(std::string("hot squares 0, +12, +24 dBFS, drive 36 dB, ") + kShapeIds[sh] +
                        (mode == 0 ? ", saturate" : ", exciter") + ": output finite and bounded",
                    worst, bound, finite && worst <= bound, "FS");
        }
    }
}

void cpu_qualities(Suite& s, const Config& golden) {
    if (!s.args().cpu) return;
    const double fs = 48000.0;
    const Chans programme = measure::cpu_programme(fs, s.args().cpu_seconds, 0.0);
    const Chans hot = measure::cpu_programme(fs, s.args().cpu_seconds, 12.0);
    const char* names[] = {", eco", ", standard (4x)", ", high (8x)"};
    const json::Value* held = s.quality("cpu.held");
    for (int q = 0; q < 3; ++q) {
        bool assert_bar = false;
        if (held != nullptr && held->is_array())
            for (const json::Value& h : held->items()) assert_bar |= h.text() == kQualityIds[q];
        Config c = golden;
        c.push_back({"quality", static_cast<float>(q)});
        Config full = c;
        full.push_back({"drive", 36.0f});
        full.push_back({"mode", 1.0f});
        measure::check_cpu_on(s, c, programme, names[q], assert_bar);
        measure::check_cpu_on(s, full, hot, std::string(names[q]) + ", programme +12 dB, drive 36 dB, exciter", assert_bar);
    }
}

}  // namespace

measure::Config saturator_golden_config() {
    // No quality: it reconfigures, so the golden renders at the default
    // (standard) and only the host fixture's reconfigure case moves it.
    return {{"drive", 15.0f}, {"shape", 1.0f}, {"tone", 3.0f}, {"mix", 80.0f}, {"output", -2.0f}};
}

int saturator(const measure::Args& args) {
    Suite s("saturator", [] { return make_saturator(); }, args);
    small_signal(s);
    harmonics(s);
    exciter(s);
    tone(s);
    aliasing(s);
    hot_input(s);
    const Config golden = saturator_golden_config();
    std::vector<std::pair<std::string, Config>> latency;
    for (int q = 0; q < 3; ++q) {
        // Quick mode: the first and last qualities.
        if (measure::quick() && q == 1) continue;
        const float qf = static_cast<float>(q);
        const std::string n = kQualityIds[q];
        latency.push_back({n + ", saturate", {{"quality", qf}, {"drive", 0.0f}}});
        latency.push_back({n + ", exciter", {{"quality", qf}, {"mode", 1.0f}, {"drive", 0.0f}}});
    }
    measure::check_latency(s, latency);
    measure::check_nonfinite(s, golden);
    measure::check_reset(s, golden);
    Config denormal = golden;
    if (measure::quick()) denormal.push_back({"quality", 0.0f});
    measure::check_denormals(s, denormal);
    measure::check_zipper(s, {{"drive", 12.0f}}, "saturate");
    measure::check_zipper(s, {{"drive", 12.0f}, {"mode", 1.0f}, {"focus", 500.0f}}, "exciter");
    measure::check_golden(s, golden);
    cpu_qualities(s, golden);
    return s.finish();
}

}  // namespace sonora::native::suites
