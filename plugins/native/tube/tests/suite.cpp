// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tube drive against its bars (quality.json):
// - the triode stage against ngspice on the same netlist (tests/spice/,
//   made by the SPICE reference harness, measure/spice.h): operating
//   point, fundamental, and every harmonic in the audio band, complex, at
//   each drive and frequency of the reference, at the oversampled rates
//   the processor runs at;
// - the processor's harmonic profile against the same reference, through
//   the oversampler, at 44.1, 48 and 96 kHz;
// - aliasing: non-harmonic energy in the band on tones from 1 to 19 kHz at
//   the nominal drive, per quality, at 44.1 and 96 kHz (and 48);
// - unity small-signal gain with auto level, the Newton solver's
//   iteration bound at the drive extremes, and the common checks
//   (latency per quality and rate, bypass, non-finite input, denormals,
//   zipper, golden files, CPU);
// - modelled noise and hum (slice 5): the stage's noise density, the
//   supply's ripple and the reservoir-to-load transfer against ngspice on
//   the same circuits (tests/spice-noise/, measure/spice_noise.h), then
//   the processor's noise spectrum band by band and its hum harmonics
//   against those references; digitally silent at 0 %, deterministic at
//   100 %, and each level's contribution in dBFS reported.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/dsp/fft.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/supply.h"
#include "sonora/dsp/triode.h"
#include "sonora/measure/measure.h"
#include "sonora/measure/spice.h"
#include "sonora/measure/spice_noise.h"
#include "sonora/native/tube.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Harmonics;
using measure::SpiceCase;
using measure::Suite;

std::string hz(double fs) { return std::to_string(static_cast<int>(fs)) + " Hz"; }

std::string fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

double db(double x) { return 20.0 * std::log10(std::fmax(x, 1e-300)); }

std::string spice_dir(const Suite& s) { return s.args().golden_dir + "/../spice"; }

// Harmonics within the audio band whose reference level is at or above
// `floor_dbc`: those are held to the per-harmonic bar; the rest reported.
struct Compare {
    double h1_db = 0.0;         // fundamental, model over reference
    double worst_dbc = 0.0;     // largest |level difference| of a held harmonic
    double error_db = -400.0;   // |model - reference| over the band, over |H1|
    int held = 0;
};

Compare compare(const Harmonics& m, const Harmonics& r, double f0, double floor_dbc, bool compare_phase) {
    Compare c;
    const double r1 = std::abs(r[0]), m1 = std::abs(m[0]);
    c.h1_db = db(m1 / r1);
    double err = 0.0;
    for (std::uint32_t k = 0; k < measure::kSpiceHarmonics; ++k) {
        if ((k + 1) * f0 > 20000.0) break;
        if (compare_phase) err += std::norm(m[k] - r[k]);
        if (k == 0) continue;
        const double rl = db(std::abs(r[k]) / r1);
        if (rl < floor_dbc) continue;
        const double ml = db(std::abs(m[k]) / m1);
        c.worst_dbc = std::fmax(c.worst_dbc, std::fabs(ml - rl));
        ++c.held;
    }
    c.error_db = compare_phase ? db(std::sqrt(err) / r1) : -400.0;
    return c;
}

// The stage on its own, at the oversampled rates the processor runs it
// at, against ngspice.
void spice_stage(Suite& s, const measure::SpiceReference& ref) {
    const dsp::TriodeStageCircuit circuit;
    const dsp::TriodeDevice device;
    // The reference was made from these values, or it is stale.
    const bool same = ref.circuit == measure::circuit_json(circuit) && ref.device == measure::device_json(device);
    s.check("SPICE reference made from the model's circuit and device values", same ? 1.0 : 0.0, 1.0, same, "bool",
            "regenerate with --spice-generate in a deliberate commit");
    s.info("SPICE reference: " + ref.ngspice, 0.0, "");

    const double floor_dbc = s.bar("spice.floor_dbc");
    const double bar_h1 = s.bar("spice.h1_db");
    const double bar_h = s.bar("spice.harmonic_db");
    const double bar_err = s.bar("spice.error_db");
    // 4x at each base rate (the default quality); 2x at 44.1 kHz, the
    // lowest rate any quality runs the stage at, is reported.
    const double held_rates[] = {176400.0, 192000.0, 384000.0};
    json::Value rows = json::Value::array();
    for (std::size_t i = 0; i < ref.cases.size(); ++i) {
        const SpiceCase& c = ref.cases[i];
        const std::string name = measure::spice_case_name(c);
        if (i == 0 || ref.cases[i - 1].law != c.law) {
            // The operating point: the DC solution of the same circuit.
            dsp::TriodeStage st;
            st.prepare(192000.0, c.law, circuit, device);
            const double mine[4] = {st.op_voltage(dsp::TriodeStage::kA), st.op_voltage(dsp::TriodeStage::kG),
                                    st.op_voltage(dsp::TriodeStage::kK), st.op_voltage(dsp::TriodeStage::kP)};
            double d = 0.0;
            for (int k = 0; k < 4; ++k) d = std::fmax(d, std::fabs(mine[k] - ref.op[i][static_cast<std::size_t>(k)]));
            s.check_le(std::string("operating point, ") + measure::law_name(c.law) + ": max |V - V(SPICE)| (plate " +
                           fmt("%.3f V, cathode %.4f V)", mine[3], mine[2]),
                       d, s.bar("spice.op_v"), "V");
        }
        const bool quick_skip = measure::quick() && c.freq_hz != 1000.0;
        for (double rate : held_rates) {
            if (quick_skip && rate != 192000.0) continue;
            std::uint32_t iters = 0;
            const Harmonics m = measure::model_harmonics(c, ref.timing, rate, circuit, device, true, &iters);
            const Compare cmp = compare(m, ref.harmonics[i], c.freq_hz, floor_dbc, true);
            const std::string at = name + " at " + hz(rate) + ": ";
            s.check_le(at + "|fundamental - SPICE|", std::fabs(cmp.h1_db), bar_h1, "dB");
            s.check_le(at + "worst harmonic level - SPICE (" + std::to_string(cmp.held) + " held)", cmp.worst_dbc,
                       bar_h, "dB");
            s.check_le(at + "waveform error to 20 kHz, |model - SPICE| / |H1|", cmp.error_db, bar_err, "dB");
            json::Value row = json::Value::object();
            row.set("case", json::Value::string(name));
            row.set("rate", json::Value::number_float(static_cast<float>(rate)));
            json::Value lm = json::Value::array(), ls = json::Value::array();
            for (std::uint32_t k = 0; k < measure::kSpiceHarmonics; ++k) {
                lm.push(json::Value::number_float(static_cast<float>(db(std::abs(m[k]) / std::abs(m[0])))));
                ls.push(json::Value::number_float(
                    static_cast<float>(db(std::abs(ref.harmonics[i][k]) / std::abs(ref.harmonics[i][0])))));
            }
            row.set("model_dbc", lm);
            row.set("spice_dbc", ls);
            row.set("h1_v", json::Value::number_float(static_cast<float>(std::abs(ref.harmonics[i][0]))));
            row.set("iterations_max", json::Value::integer(iters));
            rows.push(row);
        }
        if (!quick_skip) {
            // Eco at 44.1 kHz and the exact (untabulated) law: reported.
            const Harmonics eco = measure::model_harmonics(c, ref.timing, 88200.0, circuit, device);
            const Compare ce = compare(eco, ref.harmonics[i], c.freq_hz, floor_dbc, true);
            s.info(name + " at 88200 Hz (eco at 44.1 kHz): waveform error to 20 kHz", ce.error_db, "dB",
                   fmt("worst held harmonic %.3f dB", ce.worst_dbc));
            if (c.law == dsp::TriodeLaw::Dempwolf && c.freq_hz == 1000.0) {
                const Harmonics ex = measure::model_harmonics(c, ref.timing, 192000.0, circuit, device, false);
                const Harmonics tb = measure::model_harmonics(c, ref.timing, 192000.0, circuit, device, true);
                double d = 0.0;
                for (std::uint32_t k = 0; k < measure::kSpiceHarmonics; ++k) d = std::fmax(d, std::abs(ex[k] - tb[k]));
                s.check_le(name + " at 192000 Hz: tabulated law against the exact one, max |dH| / |H1|",
                           db(d / std::abs(ex[0])), -120.0, "dB");
            }
        }
    }
    s.data().set("spice_stage", rows);
}

// The processor: drive and input set so the stage sees each reference
// amplitude; harmonic levels at the base rate against the reference.
void spice_processor(Suite& s, const measure::SpiceReference& ref) {
    const double floor_dbc = s.bar("spice.floor_dbc");
    const double bar_h = s.bar("spice.processor_harmonic_db");
    json::Value rows = json::Value::array();
    const std::vector<double> rates = measure::quick() ? std::vector<double>{48000.0}
                                                       : std::vector<double>{44100.0, 48000.0, 96000.0};
    for (std::size_t i = 0; i < ref.cases.size(); ++i) {
        const SpiceCase& c = ref.cases[i];
        if (c.law != dsp::TriodeLaw::Dempwolf) continue;
        for (double fs : rates) {
            auto p = s.make();
            // -6 dBFS in; drive makes it the case's amplitude at vin.
            const float drive = static_cast<float>(db(c.amplitude_v / 0.5));
            measure::apply(*p, {{"drive", drive}, {"auto_level", 1.0f}, {"mix", 100.0f}});
            p->prepare(fs, 128, 1);
            const double secs = ref.timing.settle_s + ref.timing.window_s + 0.01;
            Chans x(1, std::vector<float>(static_cast<std::size_t>(secs * fs)));
            for (std::size_t n = 0; n < x[0].size(); ++n)
                x[0][n] = static_cast<float>(0.5 * std::sin(dsp::kTwoPi * c.freq_hz * static_cast<double>(n) / fs));
            const Chans y = measure::render(*p, x);
            const std::vector<double> yd(y[0].begin(), y[0].end());
            const auto from = static_cast<std::size_t>(std::llround(ref.timing.settle_s * fs));
            const auto count = static_cast<std::size_t>(std::llround(ref.timing.window_s * fs));
            const Harmonics m = measure::sampled_harmonics(yd, fs, c.freq_hz, from, count);
            const Compare cmp = compare(m, ref.harmonics[i], c.freq_hz, floor_dbc, false);
            s.check_le(measure::spice_case_name(c) + ", processor at " + hz(fs) + fmt(" (drive %+.1f dB)", drive) +
                           ": worst harmonic level - SPICE (" + std::to_string(cmp.held) + " held)",
                       cmp.worst_dbc, bar_h, "dB");
            json::Value row = json::Value::object();
            row.set("case", json::Value::string(measure::spice_case_name(c)));
            row.set("rate", json::Value::number_float(static_cast<float>(fs)));
            json::Value lm = json::Value::array();
            for (std::uint32_t k = 0; k < measure::kSpiceHarmonics; ++k)
                lm.push(json::Value::number_float(static_cast<float>(db(std::abs(m[k]) / std::abs(m[0])))));
            row.set("processor_dbc", lm);
            rows.push(row);
        }
    }
    s.data().set("spice_processor", rows);
}

void spice(Suite& s) {
    measure::SpiceReference ref;
    std::string err;
    if (!measure::read_spice_reference(spice_dir(s) + "/reference.json", ref, err)) {
        s.fail("SPICE reference: " + err);
        return;
    }
    spice_stage(s, ref);
    spice_processor(s, ref);
}

// Non-harmonic energy in the band on steady tones. The frequencies are
// prime-ish so no alias lands on a harmonic of the tone at any rate.
//
// "Nominal drive" (ADR-0011 §3.2's aliasing row) is the default drive with
// a tone at -18 dBFS, the alignment level of EBU R68 (0 VU): 0.25 V peak
// at the stage, H2 about -39 dBc. That is the asserted bar. Two hotter
// levels are reported beside it: the same drive at -6 dBFS (1 V, H2 about
// -25 dBc), and +18 dB of drive at -6 dBFS (4 V, deep into grid current).
void aliasing(Suite& s) {
    const double tones[] = {997.0, 2503.0, 5011.0, 7993.0, 10007.0, 14009.0, 19001.0};
    const char* bars[] = {"aliasing.eco_dbc", "aliasing.standard_dbc", "aliasing.high_dbc"};
    struct Level {
        const char* name;
        float drive;  // NaN: the default
        double dbfs;
    };
    const Level levels[] = {{"nominal (default drive, -18 dBFS)", std::nanf(""), -18.0},
                            {"default drive, -6 dBFS", std::nanf(""), -6.0},
                            {"drive +18 dB, -6 dBFS", 18.0f, -6.0}};
    const std::vector<double> rates = measure::quick() ? std::vector<double>{44100.0}
                                                       : std::vector<double>{44100.0, 48000.0, 96000.0};
    json::Value rows = json::Value::array();
    for (int q = 0; q < 3; ++q) {
        const std::string qn = std::string(q == 0 ? "eco (2x)" : (q == 1 ? "standard (4x)" : "high (8x)"));
        for (double fs : rates) {
            for (int lv = 0; lv < 3; ++lv) {
                double worst = -400.0, worst_f = 0.0;
                for (double f0 : tones) {
                    if (f0 > 0.45 * fs) continue;
                    auto p = s.make();
                    measure::apply(*p, {{"quality", static_cast<float>(q)}, {"mix", 100.0f}});
                    if (!std::isnan(levels[lv].drive)) measure::set(*p, "drive", levels[lv].drive);
                    p->prepare(fs, 128, 1);
                    const Chans y = measure::render(*p, measure::sine(fs, f0, levels[lv].dbfs, 1.0, 1));
                    const double a = measure::alias_dbc(y[0], static_cast<std::size_t>(0.2 * fs), fs, f0);
                    if (a > worst) {
                        worst = a;
                        worst_f = f0;
                    }
                    json::Value row = json::Value::object();
                    row.set("quality", json::Value::integer(q));
                    row.set("rate", json::Value::number_float(static_cast<float>(fs)));
                    row.set("tone", json::Value::number_float(static_cast<float>(f0)));
                    row.set("level", json::Value::integer(lv));
                    row.set("alias_dbc", json::Value::number_float(static_cast<float>(a)));
                    rows.push(row);
                }
                const std::string what = "aliasing, " + qn + ", " + hz(fs) + ", " + levels[lv].name +
                                         ", tones 1-19 kHz: worst non-harmonic";
                if (lv == 0) s.check_le(what, worst, s.bar(bars[q]), "dBc", fmt("at %.0f Hz", worst_f));
                else s.info(what, worst, "dBc", fmt("at %.0f Hz", worst_f));
            }
        }
    }
    s.data().set("aliasing", rows);
}

// Auto level and drive 0 dB: a quiet 1 kHz tone comes out at its level,
// and the small-signal response across the band (the Miller roll-off and
// the coupling capacitors) is reported.
void small_signal(Suite& s) {
    for (double fs : measure::kRates) {
        auto p = s.make();
        measure::apply(*p, {{"drive", 0.0f}});
        p->prepare(fs, 128, 1);
        const Chans y = measure::render(*p, measure::sine(fs, 1000.0, -60.0, 1.0, 1));
        const auto from = static_cast<std::size_t>(0.5 * fs);
        const double g = measure::to_db(measure::rms(y[0], from) / (std::pow(10.0, -3.0) / std::sqrt(2.0)));
        s.check_le("small-signal gain at 1 kHz, drive 0 dB, auto level, " + hz(fs) + ": |gain|", std::fabs(g),
                   s.bar("small_signal_db"), "dB");
        for (double f : {20.0, 100.0, 10000.0, 20000.0}) {
            if (f > 0.45 * fs) continue;
            auto q = s.make();
            measure::apply(*q, {{"drive", 0.0f}});
            q->prepare(fs, 128, 1);
            const Chans z = measure::render(*q, measure::sine(fs, f, -60.0, 2.0, 1));
            const auto fr = static_cast<std::size_t>(1.0 * fs);
            s.info("small-signal response at " + fmt("%.0f Hz, ", f) + hz(fs),
                   measure::to_db(measure::rms(z[0], fr) / (std::pow(10.0, -3.0) / std::sqrt(2.0))), "dB");
        }
    }
}

// Regression (the RT audit of PR #52, high 1 and 2): squares at 0, +12 and
// +24 dBFS, full drive, auto level off (the processor's output is then the
// stage's own swing, unnormalised by the drive), every quality, stereo.
// Before the fix the stage diverged at +12 dBFS and 4x: the plugin output
// reached 1e34 and blocks took 6.5 ms. The output is held to the stage's
// physical range (the load voltage within twice the supply, over the
// small-signal gain) in every build, and the worst block (minimum of three
// runs) to the 2.67 ms period in an uninstrumented one; under a sanitizer
// that figure is reported, not held (it read 3097.7 us under ASan and UBSan
// on a GitHub runner, and 2868 us on an i7-11800H at ctest -j16, where
// Release reads 200 us). The relative bound on the stage's cost is
// common/dsp's test_hot_square_every_factor, held in every build.
void hot_input(Suite& s) {
    const double fs = 48000.0;
    const char* names[] = {"eco (2x)", "standard (4x)", "high (8x)"};
    // 500 V at the load over a gain of 62.4 (test_triode).
    const double bound = 2.0 * 250.0 / 62.0;
    for (int q = 0; q < 3; ++q) {
        for (double dbfs : {0.0, 12.0, 24.0}) {
            const double a = std::pow(10.0, dbfs / 20.0);
            Chans x(2, std::vector<float>(static_cast<std::size_t>(0.5 * fs)));
            for (std::size_t n = 0; n < x[0].size(); ++n) {
                const float v = static_cast<float>(
                    std::sin(dsp::kTwoPi * 997.0 * static_cast<double>(n) / fs) >= 0.0 ? a : -a);
                x[0][n] = v;
                x[1][n] = -v;
            }
            std::vector<double> worst;
            double peak = 0.0;
            bool finite = true;
            for (int run = 0; run < 3; ++run) {
                auto p = s.make();
                measure::apply(*p, {{"quality", static_cast<float>(q)}, {"drive", 36.0f}, {"auto_level", 0.0f}});
                p->prepare(fs, 128, 2);
                std::vector<double> us;
                measure::RenderOptions opt;
                opt.block_us = &us;
                const Chans y = measure::render(*p, x, {}, opt);
                if (run == 0) worst = us;
                for (std::size_t k = 0; k < us.size(); ++k) worst[k] = std::fmin(worst[k], us[k]);
                for (const auto& ch : y) {
                    for (float v : ch) {
                        finite &= std::isfinite(v);
                        peak = std::fmax(peak, std::fabs(v));
                    }
                }
            }
            const double w = *std::max_element(worst.begin() + 1, worst.end());
            const std::string what = std::string(names[q]) + fmt(", %+.0f dBFS square, drive +36 dB", dbfs);
            s.check(what + ": output finite", finite ? 1.0 : 0.0, 1.0, finite, "bool");
            s.check_le(what + ": peak output (the stage's range, auto level off)", peak, bound, "FS");
            s.check_time_le(what + ": worst block, minimum of 3 runs", w, 2667.0, "us",
                            "the 128-frame period at 48 kHz");
        }
    }
}

// ---- modelled noise and hum (slice 5) -----------------------------------

using measure::NoiseSpiceKind;
using measure::NoiseSpiceReference;

// One-sided power spectral density by Welch's method (Hann, half overlap):
// P[k] in units^2/Hz at k fs / size.
std::vector<double> welch_psd(const std::vector<float>& x, std::size_t from, double fs, std::uint32_t size) {
    dsp::Fft fft;
    fft.prepare(size);
    std::vector<double> w(size), psd(size / 2 + 1, 0.0);
    double w2 = 0.0;
    for (std::uint32_t n = 0; n < size; ++n) {
        w[n] = 0.5 - 0.5 * std::cos(dsp::kTwoPi * n / size);
        w2 += w[n] * w[n];
    }
    std::vector<std::complex<double>> buf(size);
    std::size_t segments = 0;
    for (std::size_t at = from; at + size <= x.size(); at += size / 2) {
        for (std::uint32_t n = 0; n < size; ++n) buf[n] = w[n] * x[at + n];
        fft.forward(buf.data());
        for (std::uint32_t k = 0; k <= size / 2; ++k) psd[k] += std::norm(buf[k]);
        ++segments;
    }
    for (std::uint32_t k = 0; k <= size / 2; ++k)
        psd[k] *= (k == 0 || k == size / 2 ? 1.0 : 2.0) / (fs * w2 * static_cast<double>(segments));
    return psd;
}

// The reference density at f, interpolated log-log between ngspice's
// points (20 a decade).
double interp_density(const measure::NoiseSpiceResult& r, double f) {
    if (f <= r.freq.front()) return r.density.front();
    for (std::size_t i = 1; i < r.freq.size(); ++i) {
        if (f > r.freq[i]) continue;
        const double u = std::log(f / r.freq[i - 1]) / std::log(r.freq[i] / r.freq[i - 1]);
        return std::exp(std::log(r.density[i - 1]) + u * std::log(r.density[i] / r.density[i - 1]));
    }
    return r.density.back();
}

// The processor's gain from the load to its output with drive 0 dB, auto
// level on, output 0 dB: 1 / |H(1 kHz)| (tube.cpp normalises by it).
double load_to_output() {
    dsp::TriodeStage st;
    st.prepare(192000.0);
    double mag = 1.0, phase = 0.0;
    st.small_signal_gain(1000.0, mag, phase);
    return 1.0 / mag;
}

const measure::NoiseSpiceResult* find_case(const NoiseSpiceReference& ref, const std::string& name) {
    for (std::size_t i = 0; i < ref.cases.size(); ++i)
        if (ref.cases[i].name == name) return &ref.results[i];
    return nullptr;
}

std::string mains_name(double hz) { return std::to_string(static_cast<int>(hz)) + " Hz mains"; }

// The model's figures against ngspice on the same circuits.
void noise_hum_spice(Suite& s, const NoiseSpiceReference& ref) {
    const dsp::TriodeStageCircuit circuit;
    const dsp::TriodeDevice device;
    const dsp::SupplyCircuit supply;
    const bool same = ref.circuit == measure::circuit_json(circuit) && ref.device == measure::device_json(device) &&
                      ref.supply == measure::supply_json(supply);
    s.check("noise and hum SPICE reference made from the model's circuit, device and supply values", same ? 1.0 : 0.0,
            1.0, same, "bool", "regenerate with --spice-generate --spice-kind noise in a deliberate commit");
    dsp::TriodeStage st;
    st.prepare(192000.0);
    s.info("shot noise: Req = 2.5 / gm at the operating point", st.shot_noise_resistance(), "ohm");
    json::Value rows = json::Value::array();
    for (std::size_t i = 0; i < ref.cases.size(); ++i) {
        const measure::NoiseSpiceCase& c = ref.cases[i];
        const measure::NoiseSpiceResult& r = ref.results[i];
        if (c.kind == NoiseSpiceKind::Noise) {
            double worst = 0.0, at = 0.0;
            json::Value fm = json::Value::array(), fsp = json::Value::array();
            for (std::size_t k = 0; k < r.freq.size(); ++k) {
                const double m = measure::model_noise_density(st, r.freq[k]);
                const double d = std::fabs(db(m / r.density[k]));
                if (d > worst) {
                    worst = d;
                    at = r.freq[k];
                }
                fm.push(json::Value::number_float(static_cast<float>(m)));
                fsp.push(json::Value::number_float(static_cast<float>(r.density[k])));
            }
            s.check_le("stage noise density at the load, 10 Hz-40 kHz: worst |model - SPICE|", worst,
                       s.bar("noise.spice_db"), "dB", fmt("at %.0f Hz; %.4g V/rtHz at 1 kHz", at,
                                                          measure::model_noise_density(st, 1000.0)));
            json::Value row = json::Value::object();
            row.set("case", json::Value::string(c.name));
            row.set("model_v_rthz", fm);
            row.set("spice_v_rthz", fsp);
            rows.push(row);
        } else if (c.kind == NoiseSpiceKind::Supply) {
            dsp::SupplyCircuit sc = supply;
            sc.mains_hz = c.mains_hz;
            dsp::SupplyRipple m;
            dsp::simulate_supply(sc, measure::kSpiceHarmonics, m);
            const double h1 = std::abs(r.value[0]);
            double worst = 0.0, err = 0.0;
            int held = 0;
            for (std::uint32_t k = 0; k < measure::kSpiceHarmonics; ++k) {
                err = std::fmax(err, std::abs(m.reservoir[k] - r.value[k]) / h1);
                if (db(std::abs(r.value[k]) / h1) < s.bar("hum.floor_dbc")) continue;
                worst = std::fmax(worst, std::fabs(db(std::abs(m.reservoir[k]) / std::abs(r.value[k]))));
                ++held;
            }
            s.check_le("supply, " + mains_name(c.mains_hz) + ": reservoir ripple harmonics, worst |model - SPICE| (" +
                           std::to_string(held) + " held)",
                       worst, s.bar("hum.supply_db"), "dB",
                       fmt("H1 %.4f V at %.0f Hz; complex error %.1f dB of H1", h1, 2.0 * c.mains_hz, db(err)));
        } else {
            dsp::SupplyCircuit sc = supply;
            sc.mains_hz = c.mains_hz;
            double worst = 0.0, worst_deg = 0.0;
            for (std::size_t k = 0; k < r.freq.size(); ++k) {
                const std::complex<double> m = measure::model_chain_transfer(st, sc, r.freq[k]);
                worst = std::fmax(worst, std::fabs(db(std::abs(m) / std::abs(r.value[k]))));
                worst_deg = std::fmax(worst_deg, std::fabs(std::arg(m / r.value[k])) * 180.0 / dsp::kPi);
            }
            s.check_le("reservoir to load, " + mains_name(c.mains_hz) + ", 9 ripple harmonics: worst |model - SPICE|",
                       worst, s.bar("hum.chain_db"), "dB",
                       fmt("%.1f dB at %.0f Hz", db(std::abs(r.value[0])), r.freq[0]));
            s.check_le("reservoir to load, " + mains_name(c.mains_hz) + ": worst phase difference", worst_deg,
                       s.bar("hum.chain_deg"), "deg");
        }
    }
    s.data().set("noise_spice", rows);
}

// The processor's noise, band by band, against ngspice's density through
// the processor's own gain: drive 0 dB, auto level, so a load voltage v
// comes out as v / |H(1 kHz)|. Third-octave bands (IEC 61260 nominal
// centres, base-2 edges) from 25 Hz to 16 kHz, 60 s of output per rate.
void noise_processor(Suite& s, const NoiseSpiceReference& ref) {
    const measure::NoiseSpiceResult* r = find_case(ref, "noise");
    if (r == nullptr) {
        s.fail("noise reference: no 'noise' case");
        return;
    }
    const double g = load_to_output();
    const std::vector<double> rates = measure::quick() ? std::vector<double>{48000.0}
                                                       : std::vector<double>{44100.0, 48000.0, 96000.0};
    const double secs = measure::quick() ? 30.0 : 60.0;
    json::Value rows = json::Value::array();
    for (double fs : rates) {
        auto p = s.make();
        measure::apply(*p, {{"drive", 0.0f}, {"noise", 100.0f}, {"mix", 100.0f}});
        p->prepare(fs, 128, 1);
        const Chans y = measure::render(*p, measure::silence(fs, secs, 1));
        const std::uint32_t size = fs > 50000.0 ? 32768u : 16384u;
        const std::vector<double> psd = welch_psd(y[0], static_cast<std::size_t>(fs), fs, size);
        const double df = fs / size;
        const double segments = std::floor(secs * fs / size);
        double worst = 0.0, worst_f = 0.0, total = 0.0;
        for (int b = -16; b <= 12; ++b) {  // 25 Hz .. 16 kHz around 1 kHz
            const double fc = 1000.0 * std::pow(2.0, b / 3.0);
            const double lo = fc * std::pow(2.0, -1.0 / 6.0), hi = fc * std::pow(2.0, 1.0 / 6.0);
            double got = 0.0, want = 0.0;
            std::size_t bins = 0;
            for (std::size_t k = 1; k < psd.size(); ++k) {
                const double f = k * df;
                if (f < lo || f >= hi) continue;
                ++bins;
                got += psd[k] * df;
                const double d = interp_density(*r, f) * g;
                want += d * d * df;
            }
            if (want <= 0.0) continue;
            // A band's level is a mean of bins x segments chi-squared(2)
            // values (Welch, no overlap credit), so its own spread is
            // 4.343 / sqrt(K) dB. The low bands have a handful of bins:
            // 39 Hz has 3 in a 30 s quick render. The model is held to the
            // bar after 3 sigma of the estimator is taken off, but that
            // credit is capped at 0.25 dB so a real bias of about 1 dB
            // cannot hide in a noisy band; never below 0.
            const double k_avg = static_cast<double>(bins) * static_cast<double>(segments);
            const double sigma = 4.343 / std::sqrt(k_avg);
            const double credit = std::fmin(0.25, 3.0 * sigma);
            const double d = std::fmax(0.0, std::fabs(10.0 * std::log10(got / want)) - credit);
            if (d > worst) {
                worst = d;
                worst_f = fc;
            }
            json::Value row = json::Value::object();
            row.set("rate", json::Value::number_float(static_cast<float>(fs)));
            row.set("band_hz", json::Value::number_float(static_cast<float>(fc)));
            row.set("processor_db", json::Value::number_float(static_cast<float>(10.0 * std::log10(got))));
            row.set("spice_db", json::Value::number_float(static_cast<float>(10.0 * std::log10(want))));
            rows.push(row);
        }
        for (std::size_t k = 1; k < psd.size(); ++k)
            if (k * df >= 20.0 && k * df <= 20000.0) total += psd[k] * df;
        s.check_le("noise 100 %, drive 0 dB, " + hz(fs) +
                       ": third-octave bands 25 Hz-16 kHz, worst |processor - SPICE| less the estimator credit (3 sigma, at most 0.25 dB)",
                   worst, s.bar("noise.band_db"), "dB", fmt("at %.0f Hz", worst_f));
        // A full-scale sine is 0 dBFS: its rms is 1 / sqrt(2).
        s.info("noise 100 %, drive 0 dB, " + hz(fs) + ": level 20 Hz-20 kHz", db(std::sqrt(total * 2.0)), "dBFS",
               "the instance's contribution, rms against a full-scale sine");
    }
    s.data().set("noise_bands", rows);
}

// The processor's hum: harmonics of the ripple at the output against
// ngspice's reservoir harmonics through ngspice's reservoir-to-load
// transfer, through the processor's gain.
void hum_processor(Suite& s, const NoiseSpiceReference& ref) {
    const double g = load_to_output();
    const double floor = s.bar("hum.floor_dbc");
    for (int m = 0; m < 2; ++m) {
        const double mains = m == 0 ? 50.0 : 60.0;
        const std::string tag = std::to_string(static_cast<int>(mains)) + "hz";
        const measure::NoiseSpiceResult* sup = find_case(ref, "supply-" + tag);
        const measure::NoiseSpiceResult* chain = find_case(ref, "chain-" + tag);
        if (sup == nullptr || chain == nullptr) {
            s.fail("hum reference: no case for " + tag);
            return;
        }
        Harmonics want(measure::kSpiceHarmonics);
        for (std::uint32_t k = 0; k < measure::kSpiceHarmonics; ++k) want[k] = sup->value[k] * chain->value[k] * g;
        for (double fs : measure::kRates) {
            if (measure::quick() && fs != 48000.0) continue;
            auto p = s.make();
            measure::apply(*p, {{"drive", 0.0f}, {"hum", 100.0f}, {"mains", static_cast<float>(m)}, {"mix", 100.0f}});
            p->prepare(fs, 128, 1);
            const Chans y = measure::render(*p, measure::silence(fs, 1.5, 1));
            const std::vector<double> yd(y[0].begin(), y[0].end());
            const std::size_t from = static_cast<std::size_t>(fs);
            const std::size_t count = static_cast<std::size_t>(std::llround(0.5 * fs));  // whole ripple periods
            const Harmonics got = measure::sampled_harmonics(yd, fs, 2.0 * mains, from, count);
            double worst = 0.0;
            int held = 0;
            for (std::uint32_t k = 0; k < measure::kSpiceHarmonics; ++k) {
                if (db(std::abs(want[k]) / std::abs(want[0])) < floor) continue;
                worst = std::fmax(worst, std::fabs(db(std::abs(got[k]) / std::abs(want[k]))));
                ++held;
            }
            s.check_le("hum 100 %, " + mains_name(mains) + ", " + hz(fs) + ": ripple harmonics at the output, worst "
                           "|processor - SPICE| (" + std::to_string(held) + " held)",
                       worst, s.bar("hum.processor_db"), "dB",
                       fmt("H1 %.1f dBFS at %.0f Hz", db(std::abs(got[0])),
                           2.0 * mains));
            const double rms = measure::rms(y[0], from);
            s.info("hum 100 %, " + mains_name(mains) + ", drive 0 dB, " + hz(fs) + ": level", db(rms * std::sqrt(2.0)),
                   "dBFS", "the instance's contribution, rms against a full-scale sine");
        }
    }
}

// 0 % is the path without noise and hum, to the bit (the golden files,
// recorded before slice 5, also hold it); 100 % is deterministic: two
// fresh renders and a render after reset() are identical; the two
// channels' noise is independent and their hum common.
void noise_hum_silence_and_determinism(Suite& s) {
    const double fs = 48000.0;
    {
        auto p = s.make();
        measure::apply(*p, {{"noise", 0.0f}, {"hum", 0.0f}});
        p->prepare(fs, 128, 2);
        const Chans y = measure::render(*p, measure::silence(fs, 1.0, 2));
        double peak = 0.0;
        for (const auto& ch : y) peak = std::fmax(peak, measure::peak(ch));
        // The generators add exactly nothing at 0 % (tube.cpp skips them);
        // what is left is the stage's own residue at rest, 3e-14 (-270
        // dBFS), there before slice 5. The golden files, recorded before
        // slice 5, pass unchanged at 0 %.
        s.check_le("noise 0 %, hum 0 %: silence in, peak out", peak, 1e-12, "FS",
                   "digitally silent: the stage's rest residue only");
    }
    {
        // 0 % is the path without noise and hum, to the bit: a unit that
        // had them at 100 % and is set back to 0 renders exactly as one
        // that never did, so nothing leaks from the generators.
        const Chans gin = measure::golden_input(fs, 1.0);
        auto base = s.make();
        auto back = s.make();
        measure::apply(*base, {{"drive", 12.0f}});
        measure::apply(*back, {{"noise", 100.0f}, {"hum", 100.0f}});
        measure::apply(*back, {{"noise", 0.0f}, {"hum", 0.0f}, {"drive", 12.0f}});
        base->prepare(fs, 128, 2);
        back->prepare(fs, 128, 2);
        const Chans y0 = measure::render(*base, gin);
        const Chans y1 = measure::render(*back, gin);
        double dz = 0.0;
        for (std::size_t c = 0; c < 2; ++c)
            for (std::size_t i = 0; i < y0[c].size(); ++i) dz = std::fmax(dz, std::fabs(y0[c][i] - y1[c][i]));
        s.check("noise 0 %, hum 0 %: against a unit never given them, max |difference|", dz, 0.0, dz == 0.0, "abs",
                "bit-identical, not within the golden tolerance");
    }
    const measure::Config on = {{"noise", 100.0f}, {"hum", 100.0f}, {"drive", 12.0f}};
    const Chans in = measure::golden_input(fs, 1.0);
    auto a = s.make();
    auto b = s.make();
    measure::apply(*a, on);
    measure::apply(*b, on);
    a->prepare(fs, 128, 2);
    b->prepare(fs, 128, 2);
    const Chans ya = measure::render(*a, in);
    const Chans yb = measure::render(*b, in);
    a->reset();
    const Chans yr = measure::render(*a, in);
    double d = 0.0;
    for (std::size_t c = 0; c < 2; ++c)
        for (std::size_t i = 0; i < ya[c].size(); ++i)
            d = std::fmax(d, std::fmax(std::fabs(ya[c][i] - yb[c][i]), std::fabs(ya[c][i] - yr[c][i])));
    s.check("noise 100 %, hum 100 %: two renders and a render after reset, max |difference|", d, 0.0, d == 0.0, "abs",
            "seeded (ADR-0011 §3.3)");
    // Noise alone on silence: the channels' correlation.
    auto q = s.make();
    measure::apply(*q, {{"noise", 100.0f}, {"drive", 0.0f}});
    q->prepare(fs, 128, 2);
    const Chans z = measure::render(*q, measure::silence(fs, 10.0, 2));
    double xy = 0.0, xx = 0.0, yy = 0.0;
    for (std::size_t i = static_cast<std::size_t>(fs); i < z[0].size(); ++i) {
        xy += static_cast<double>(z[0][i]) * z[1][i];
        xx += static_cast<double>(z[0][i]) * z[0][i];
        yy += static_cast<double>(z[1][i]) * z[1][i];
    }
    const double rho = xy / std::sqrt(xx * yy);
    s.check_le("noise 100 %, stereo: |correlation between the channels|", std::fabs(rho), 0.02, "",
               "independent stages, independent seeds");
}

void noise_hum(Suite& s) {
    NoiseSpiceReference ref;
    std::string err;
    if (!measure::read_noise_spice_reference(s.args().golden_dir + "/../spice-noise/reference.json", ref, err)) {
        s.fail("noise and hum SPICE reference: " + err);
        return;
    }
    s.info("noise and hum SPICE reference: " + ref.ngspice, 0.0, "");
    {
        // prepare() integrates the supply (supply.h): its cost, off the
        // audio thread, when a host instantiates or re-activates.
        auto p = s.make();
        const auto t0 = std::chrono::steady_clock::now();
        p->prepare(48000.0, 128, 2);
        const auto t1 = std::chrono::steady_clock::now();
        s.info("prepare(), 48 kHz stereo, wall time (the supply's integration included)",
               std::chrono::duration<double, std::milli>(t1 - t0).count(), "ms", "not real-time");
    }
    noise_hum_spice(s, ref);
    noise_processor(s, ref);
    hum_processor(s, ref);
    noise_hum_silence_and_determinism(s);
}

// The Newton solve stays inside its iteration bound from silence to +36 dB
// of drive on a full-scale input, and its output stays finite.
void solver_bound(Suite& s) {
    dsp::TriodeStage st;
    st.prepare(192000.0);
    bool finite = true;
    for (double a : {0.01, 1.0, 4.0, 63.0, 1000.0}) {
        for (int n = 0; n < 192000 / 10; ++n) {
            const double y = st.tick(a * std::sin(dsp::kTwoPi * 7993.0 * n / 192000.0));
            finite &= std::isfinite(y);
        }
        s.info(fmt("Newton iterations, most per sample, %.2f V peak at 7993 Hz", a), st.max_iterations_seen(), "");
        st.reset();
    }
    s.check("triode stage: output finite up to 1000 V peak", finite ? 1.0 : 0.0, 1.0, finite, "bool");
    dsp::TriodeStage t;
    t.prepare(192000.0);
    for (int n = 0; n < 19200; ++n) t.tick(63.0 * std::sin(dsp::kTwoPi * 997.0 * n / 192000.0));
    s.check_le("Newton iterations, most per sample at +36 dB drive (63 V peak)", t.max_iterations_seen(),
               s.bar("max_iterations"), "");
}

// ADR-0011 §3.5: "a processor over its class budget ships only with an eco
// mode that meets it". Eco is held to class B on every load below;
// standard and high are measured and reported. The loads: the harness's
// programme signal at the golden settings, and two worst cases at full
// drive (+36 dB): the programme raised 12 dB (peaks near 0 dBFS), and a
// full-scale 997 Hz square, whose edges cost the solve the most
// iterations and whose flat tops hold the grid in grid current or deep
// cut-off.
void cpu_qualities(Suite& s, const Config& golden) {
    if (!s.args().cpu) return;
    const double fs = 48000.0;
    const double secs = s.args().cpu_seconds;
    const Chans programme = measure::cpu_programme(fs, secs, 0.0);
    const Chans hot = measure::cpu_programme(fs, secs, 12.0);
    Chans square(2, std::vector<float>(static_cast<std::size_t>(secs * fs)));
    for (std::size_t n = 0; n < square[0].size(); ++n) {
        const float v = std::sin(dsp::kTwoPi * 997.0 * static_cast<double>(n) / fs) >= 0.0 ? 1.0f : -1.0f;
        square[0][n] = v;
        square[1][n] = -v;
    }
    const char* names[] = {", eco (2x)", ", standard (4x)", ", high (8x)"};
    for (int q = 0; q < 3; ++q) {
        Config c = golden;
        c.push_back({"quality", static_cast<float>(q)});
        Config full = c;
        full.push_back({"drive", 36.0f});
        measure::check_cpu_on(s, c, programme, names[q], q == 0);
        measure::check_cpu_on(s, full, hot, std::string(names[q]) + ", programme +12 dB, drive +36 dB", q == 0);
        measure::check_cpu_on(s, full, square, std::string(names[q]) + ", 0 dBFS square, drive +36 dB", q == 0);
        // Slice 5: noise and hum at 100 %, both generated every sample.
        Config noisy = c;
        noisy.push_back({"noise", 100.0f});
        noisy.push_back({"hum", 100.0f});
        measure::check_cpu_on(s, noisy, programme, std::string(names[q]) + ", noise and hum 100 %", q == 0);
    }
}

}  // namespace

// The golden configuration, shared with the host tests (sonora-dsp-measure
// --host-fixture).
measure::Config tube_golden_config() {
    return {{"drive", 14.0f}, {"output", -2.0f}, {"mix", 85.0f}, {"auto_level", 1.0f}};
}

int tube(const measure::Args& args) {
    Suite s("tube", [] { return make_tube(); }, args);
    spice(s);
    noise_hum(s);
    aliasing(s);
    small_signal(s);
    solver_bound(s);
    hot_input(s);
    const Config golden = tube_golden_config();
    // check_common() without its CPU check, which holds the golden
    // (standard) quality to the class bar: cpu_qualities() instead.
    measure::check_latency(s, {{"eco (2x)", {{"quality", 0.0f}}},
                               {"standard (4x)", {{"quality", 1.0f}}},
                               {"high (8x)", {{"quality", 2.0f}}}});
    measure::check_nonfinite(s, golden);
    measure::check_denormals(s, golden);
    Config noisy = golden;
    noisy.push_back({"noise", 100.0f});
    noisy.push_back({"hum", 100.0f});
    measure::check_nonfinite(s, noisy);
    measure::check_denormals(s, noisy);
    measure::check_zipper(s, {{"mix", 60.0f}}, "");
    measure::check_golden(s, golden);
    cpu_qualities(s, golden);
    return s.finish();
}

}  // namespace sonora::native::suites
