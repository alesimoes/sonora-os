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
//   zipper, golden files, CPU).
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/dsp/math.h"
#include "sonora/dsp/triode.h"
#include "sonora/measure/measure.h"
#include "sonora/measure/spice.h"
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
// small-signal gain), and the worst block (minimum of three runs) to the
// 2.67 ms period.
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
            s.check_le(what + ": worst block, minimum of 3 runs", w, 2667.0, "us", "the 128-frame period at 48 kHz");
        }
    }
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
    measure::check_zipper(s, {{"mix", 60.0f}}, "");
    measure::check_golden(s, golden);
    cpu_qualities(s, golden);
    return s.finish();
}

}  // namespace sonora::native::suites
