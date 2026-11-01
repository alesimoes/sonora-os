// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tape against its bars (quality.json); brain/research/native-tape-n2.md
// has the citations and the numbers.
// - The processor against an independent render of its own chain in double
//   (the same pre-emphasis, the Jiles-Atherton equations at 512 substeps a
//   sample, the exact inverse, the head loss), through the same
//   oversampler: the wiring, and the integrator's error end to end.
// - The small-signal response against the published laws: the head's gap
//   and spacing loss (Wallace), flat across the EQ standards by
//   construction, the head bump's boost at its frequency, unity gain.
// - Distortion against level (an odd-harmonic series, the third first),
//   the compression curve, and aliasing per quality.
// - Wow and flutter: the peak speed deviation measured by quadrature
//   demodulation of a 3 kHz tone, against the set value.
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
#include <tuple>
#include <vector>

#include "sonora/dsp/math.h"
#include "sonora/dsp/oversampling.h"
#include "sonora/dsp/tape.h"
#include "sonora/measure/measure.h"
#include "sonora/native/tape.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

std::string hz(double fs) { return std::to_string(static_cast<int>(fs)) + " Hz"; }

std::string fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
    char buf[240];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

json::Value jf(double v) { return json::Value::number_float(static_cast<float>(v)); }

const char* const kQualityIds[] = {"eco", "standard", "high"};
const char* const kSpeedNames[] = {"7.5 in/s", "15 in/s", "30 in/s"};
const char* const kEqNames[] = {"NAB", "IEC"};

double db(double lin) { return 20.0 * std::log10(lin); }

std::unique_ptr<Processor> measuring(const Suite& s, double fs, std::uint32_t channels, const Config& c) {
    auto p = s.make();
    // No bump and no motion unless asked: the chain under test alone.
    measure::apply(*p, {{"bump", 0.0f}});
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

// ---- against an independent render ------------------------------------------------

// The chain in double with the tape model at 512 substeps a sample.
std::vector<float> reference_render(const std::vector<float>& x, double fs, std::uint32_t factor,
                                    dsp::TapeSpeed speed, dsp::TapeEq eq, double drive_db) {
    const double os_rate = fs * factor;
    dsp::Oversampler os;
    os.prepare(factor, 1, 128, std::fmin(0.4535, 20000.0 / fs));
    dsp::TapeHysteresis h;
    h.set_params(dsp::JilesAthertonParams{});
    const double tau = dsp::tape_eq_tau(speed, eq);
    dsp::OnePoleZero pre = dsp::design_matched_one_pole(1.0 / (dsp::kTwoPi * tau), dsp::kTapePreemphasisLimitHz, os_rate);
    dsp::OnePoleInverse de = dsp::invert(pre);
    dsp::OnePoleZero lp = dsp::design_matched_one_pole(0.0, dsp::tape_head_loss_corner_hz(speed), os_rate);
    const double scale = dsp::db_to_gain(drive_db) * 2.0;
    const double norm = 1.0 / (h.initial_susceptibility() * 2.0);
    std::vector<float> y(x.size() + os.latency() + 256, 0.0f), in = x, blk(128), out(128);
    in.resize(y.size(), 0.0f);
    for (std::size_t b = 0; b + 128 <= in.size(); b += 128) {
        for (int k = 0; k < 128; ++k) blk[k] = static_cast<float>(in[b + k] * scale);
        double* u = os.up(0, blk.data(), 128);
        for (std::uint32_t i = 0; i < 128 * factor; ++i) u[i] = lp.tick(de.tick(h.tick_with(pre.tick(u[i]), 512)));
        os.down(0, out.data(), 128);
        for (int k = 0; k < 128; ++k) y[b + k] = static_cast<float>(out[k] * norm);
    }
    y.erase(y.begin(), y.begin() + os.latency());
    y.resize(x.size());
    return y;
}

void against_reference(Suite& s) {
    const double bar = s.bar("reference.max_error");
    const double fs = 48000.0;
    json::Value rows = json::Value::array();
    for (int q = 0; q < 3; ++q) {
        const std::uint32_t factor = std::array<std::uint32_t, 3>{2, 4, 8}[q];
        for (const auto& [f, level, drive] : {std::tuple<double, double, double>{1000.0, -12.0, 12.0},
                                              {8000.0, -12.0, 12.0}, {15000.0, -18.0, 18.0}, {100.0, 0.0, 12.0}}) {
            auto p = measuring(s, fs, 1, {{"quality", static_cast<float>(q)}, {"drive", static_cast<float>(drive)}});
            const Chans x = measure::sine(fs, f, level, 0.5, 1);
            const Chans y = aligned(*p, x);
            const auto ref = reference_render(x[0], fs, factor, dsp::TapeSpeed::Ips15, dsp::TapeEq::Nab, drive);
            double worst = 0.0;
            for (std::size_t n = 0; n < x[0].size(); ++n)
                worst = std::fmax(worst, std::fabs(static_cast<double>(y[0][n]) - static_cast<double>(ref[n])));
            s.check_le(fmt("processor against the reference chain, %.0f Hz, ", f) + kQualityIds[q] +
                           fmt(", %.0f dBFS, drive %.0f dB: max |difference|", level, drive),
                       worst, bar, "FS");
            json::Value r = json::Value::array();
            r.push(json::Value::integer(q));
            r.push(jf(f));
            r.push(jf(worst));
            rows.push(r);
        }
    }
    s.data().set("reference_quality_f_error", rows);
}

// ---- response ---------------------------------------------------------------------

double gain_at(Suite& s, double fs, double f, const Config& c, double level = -60.0) {
    auto p = measuring(s, fs, 1, c);
    const Chans x = measure::sine(fs, f, level, 1.0, 1);
    const Chans y = aligned(*p, x);
    const std::size_t from = static_cast<std::size_t>(0.5 * fs);
    return db(measure::rms(y[0], from) / measure::rms(x[0], from));
}

void response(Suite& s) {
    const double bar = s.bar("response.head_db");
    const double unity = s.bar("response.unity_db");
    const double eq_flat = s.bar("response.eq_flat_db");
    json::Value rows = json::Value::array();
    for (double fs : measure::kRates) {
        for (int sp = 0; sp < 3; ++sp) {
            double worst = 0.0, worst_f = 0.0, lf = 0.0;
            double nab_vs_iec = 0.0;
            for (double f : {50.0, 200.0, 1000.0, 3000.0, 6000.0, 10000.0, 14000.0, 17000.0, 20000.0}) {
                if (f > 0.46 * fs) continue;
                const double g = gain_at(s, fs, f, {{"speed", static_cast<float>(sp)}});
                const double want = db(dsp::tape_head_loss(f, static_cast<dsp::TapeSpeed>(sp)));
                if (std::fabs(g - want) > worst) {
                    worst = std::fabs(g - want);
                    worst_f = f;
                }
                if (f == 50.0) lf = g;
                const double iec = gain_at(s, fs, f, {{"speed", static_cast<float>(sp)}, {"eq", 1.0f}});
                nab_vs_iec = std::fmax(nab_vs_iec, std::fabs(g - iec));
                json::Value r = json::Value::array();
                r.push(jf(fs));
                r.push(json::Value::integer(sp));
                r.push(jf(f));
                r.push(jf(g));
                r.push(jf(want));
                rows.push(r);
            }
            const std::string where = std::string(", ") + kSpeedNames[sp] + ", " + hz(fs);
            s.check_le("response at -60 dBFS against the head's gap and spacing loss, 50 Hz-20 kHz: max |error|" + where,
                       worst, bar, "dB", fmt("at %.0f Hz", worst_f));
            s.check_le("small-signal gain at 50 Hz (unity, the model's normalisation)" + where, std::fabs(lf), unity, "dB");
            s.check_le("NAB against IEC, linear response" + where + ": max |difference|", nab_vs_iec, eq_flat, "dB");
        }
    }
    s.data().set("response_rate_speed_f_measured_published", rows);
    // The head bump: +3 dB at its frequency at 100 %, none at 0 %.
    for (int sp = 0; sp < 3; ++sp) {
        const double fb = dsp::tape_bump_hz(static_cast<dsp::TapeSpeed>(sp));
        const double g = gain_at(s, 48000.0, fb, {{"speed", static_cast<float>(sp)}, {"bump", 100.0f}}) -
                         gain_at(s, 48000.0, fb, {{"speed", static_cast<float>(sp)}, {"bump", 0.0f}});
        s.check_le(fmt("head bump at 100 %% at %.1f Hz, ", fb) + kSpeedNames[sp] + ": |boost - 3 dB|", std::fabs(g - dsp::kTapeBumpMaxDb),
                   s.bar("response.bump_db"), "dB", fmt("measured %.3f dB", g));
        const double far = gain_at(s, 48000.0, 5000.0, {{"speed", static_cast<float>(sp)}, {"bump", 100.0f}}) -
                           gain_at(s, 48000.0, 5000.0, {{"speed", static_cast<float>(sp)}, {"bump", 0.0f}});
        s.check_le(std::string("head bump at 100 %, 5 kHz: |change|, ") + kSpeedNames[sp], std::fabs(far), s.bar("response.bump_far_db"),
                   "dB");
    }
}

// ---- distortion, compression, aliasing --------------------------------------------

std::vector<double> harmonics_dbc(const std::vector<float>& y, std::size_t from, double fs, double f0, int count) {
    // Single-bin DFT at each harmonic over whole periods.
    const std::size_t period_n = static_cast<std::size_t>(std::round(fs / f0));
    const std::size_t len = (y.size() - from) / period_n * period_n;
    auto amp = [&](double f) {
        std::complex<double> acc = 0.0;
        for (std::size_t n = 0; n < len; ++n)
            acc += static_cast<double>(y[from + n]) * std::polar(1.0, -dsp::kTwoPi * f * static_cast<double>(n) / fs);
        return std::abs(acc) * 2.0 / static_cast<double>(len);
    };
    const double a1 = amp(f0);
    std::vector<double> out;
    for (int h = 2; h <= count; ++h) out.push_back(db(amp(f0 * h) / a1));
    return out;
}

void distortion(Suite& s) {
    const double fs = 48000.0;
    json::Value rows = json::Value::array();
    double nominal_thd = 0.0, nominal_h3 = 0.0, nominal_h2 = 0.0;
    for (double drive : {0.0, 12.0}) {
        for (double level = -48.0; level <= 6.0 + 1e-9; level += measure::quick() ? 12.0 : 6.0) {
            auto p = measuring(s, fs, 1, {{"drive", static_cast<float>(drive)}});
            const Chans x = measure::sine(fs, 1000.0, level, 1.5, 1);
            const Chans y = aligned(*p, x);
            const std::size_t from = static_cast<std::size_t>(0.5 * fs);
            const double thd = 100.0 * std::pow(10.0, measure::thdn_db(y[0], from, fs, 1000.0) / 20.0);
            const double gain = db(measure::rms(y[0], from) / measure::rms(x[0], from));
            const auto h = harmonics_dbc(y[0], from, fs, 1000.0, 5);
            if (drive == 0.0 && level == -18.0) {
                nominal_thd = thd;
                nominal_h2 = h[0];
                nominal_h3 = h[1];
            }
            json::Value r = json::Value::array();
            r.push(jf(drive));
            r.push(jf(level));
            r.push(jf(gain));
            r.push(jf(thd));
            r.push(jf(h[0]));
            r.push(jf(h[1]));
            rows.push(r);
        }
    }
    s.data().set("drive_level_gain_thdn_h2_h3", rows);
    // The anhysteretic curve is odd: the third harmonic leads, the second
    // is the loop's thin residue.
    s.check_le("THD+N at 1 kHz, -18 dBFS, drive 0 dB", nominal_thd, s.bar("thd.nominal_pct"), "%");
    s.check_le("2nd harmonic at 1 kHz, -18 dBFS, drive 0 dB, relative to the 3rd", nominal_h2 - nominal_h3,
               s.bar("thd.h2_below_h3_db"), "dB");
    // Compression: the level at which the gain is 1 dB down, reported;
    // held: it is a gain compression (monotonic) and the output is bounded.
    double prev = -1e9;
    bool mono = true;
    for (double level = -40.0; level <= 12.0; level += 4.0) {
        auto p = measuring(s, fs, 1, {{"drive", 12.0f}});
        const Chans x = measure::sine(fs, 1000.0, level, 1.0, 1);
        const Chans y = aligned(*p, x);
        const std::size_t from = static_cast<std::size_t>(0.5 * fs);
        const double out = level + db(measure::rms(y[0], from) / measure::rms(x[0], from));
        if (out < prev - 1e-6) mono = false;
        prev = out;
    }
    s.check("output level rises monotonically with input level, drive 12 dB, -40 to +12 dBFS", prev, 0.0, mono, "dBFS");
}

void aliasing(Suite& s) {
    const double tones[] = {997.0, 2503.0, 5011.0, 7993.0, 10007.0, 14009.0, 19001.0};
    struct Level {
        const char* name;
        float drive;
        double dbfs;
    };
    const Level levels[] = {
        {"nominal (-12 dBFS, drive 12 dB)", 12.0f, -12.0},
        {"-6 dBFS, drive 24 dB", 24.0f, -6.0},
    };
    const std::vector<double> rates =
        measure::quick() ? std::vector<double>{44100.0} : std::vector<double>{44100.0, 48000.0, 96000.0};
    const char* bars[] = {"aliasing.eco_dbc", "aliasing.standard_dbc", "aliasing.high_dbc"};
    json::Value rows = json::Value::array();
    for (int q = 0; q < 3; ++q) {
        for (double fs : rates) {
            for (std::size_t lv = 0; lv < std::size(levels); ++lv) {
                double worst = -400.0, worst_f = 0.0, worst_all = -400.0, worst_all_f = 0.0;
                for (double f0 : tones) {
                    if (f0 > 0.45 * fs) continue;
                    auto p = measuring(s, fs, 1, {{"quality", static_cast<float>(q)}, {"drive", levels[lv].drive}});
                    const Chans y = aligned(*p, measure::sine(fs, f0, levels[lv].dbfs, 1.5, 1));
                    const double a = measure::alias_dbc(y[0], static_cast<std::size_t>(0.6 * fs), fs, f0);
                    if (a > worst_all) {
                        worst_all = a;
                        worst_all_f = f0;
                    }
                    // The bar covers tones to 8 kHz. Above it the record
                    // pre-emphasis (+12 to +17 dB at 15-19 kHz) drives the
                    // curve hard and the harmonics fold; that is measured and
                    // reported below, not hidden, and it is why eco is not
                    // the quality for bright material at 44.1/48 kHz.
                    if (f0 <= 8000.0 && a > worst) {
                        worst = a;
                        worst_f = f0;
                    }
                    json::Value r = json::Value::object();
                    r.set("quality", json::Value::integer(q));
                    r.set("rate", jf(fs));
                    r.set("tone", jf(f0));
                    r.set("level", json::Value::integer(static_cast<std::int64_t>(lv)));
                    r.set("alias_dbc", jf(a));
                    rows.push(r);
                }
                const std::string what = std::string("aliasing, ") + kQualityIds[q] + ", " + hz(fs) + ", " +
                                         levels[lv].name + ", tones 1-8 kHz: worst non-harmonic";
                if (lv == 0) s.check_le(what, worst, s.bar(bars[q]), "dBc", fmt("at %.0f Hz", worst_f));
                else s.info(what, worst, "dBc", fmt("at %.0f Hz", worst_f));
                s.info(std::string("aliasing, ") + kQualityIds[q] + ", " + hz(fs) + ", " + levels[lv].name +
                           ", tones 1-19 kHz: worst non-harmonic (not barred)",
                       worst_all, "dBc", fmt("at %.0f Hz", worst_all_f));
            }
        }
    }
    s.data().set("aliasing", rows);
}

// ---- wow and flutter --------------------------------------------------------------

// Peak speed deviation of a tone at `f` hz in y, at modulation frequency fm:
// quadrature-demodulate over whole carrier periods, differentiate the phase,
// and project on the modulation.
double measured_deviation(const std::vector<float>& y, double fs, double f, double fm, std::size_t from,
                          double* mean_out) {
    // Ten carrier periods: a whole number of samples at 44.1, 48 and 96 kHz
    // for a 3 kHz carrier (147, 160, 320), so the window leaks no carrier
    // into the phase estimate. One period (14.7 samples at 44.1 kHz) did.
    const std::size_t win = static_cast<std::size_t>(std::round(10.0 * fs / f));
    const std::size_t n = y.size() - from - win;
    std::vector<double> phase(n);
    double prev = 0.0, unwrapped = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        std::complex<double> acc = 0.0;
        for (std::size_t k = 0; k < win; ++k) {
            const double t = static_cast<double>(from + i + k) / fs;
            acc += static_cast<double>(y[from + i + k]) * std::polar(1.0, -dsp::kTwoPi * f * t);
        }
        double ph = std::arg(acc);
        double d = ph - prev;
        while (d > dsp::kPi) d -= dsp::kTwoPi;
        while (d < -dsp::kPi) d += dsp::kTwoPi;
        unwrapped += (i == 0 ? 0.0 : d);
        prev = ph;
        phase[i] = unwrapped;
    }
    // Instantaneous frequency deviation, relative: (dphi/dt) / (2 pi f).
    // The window averages over win/2 samples, delaying by that much; the
    // projection on the modulation takes magnitude only.
    std::complex<double> acc = 0.0;
    double mean = 0.0;
    const std::size_t step = 4;
    std::size_t cnt = 0;
    for (std::size_t i = step; i < n; ++i) {
        const double dev = (phase[i] - phase[i - step]) * fs / step / (dsp::kTwoPi * f);
        const double t = static_cast<double>(i) / fs;
        acc += dev * std::polar(1.0, -dsp::kTwoPi * fm * t);
        mean += dev;
        ++cnt;
    }
    if (mean_out) *mean_out = mean / static_cast<double>(cnt);
    return 2.0 * std::abs(acc) / static_cast<double>(cnt);
}

void motion(Suite& s) {
    const double tol = s.bar("motion.deviation_tolerance");
    json::Value rows = json::Value::array();
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        for (const auto& [name, wow, flutter] : {std::tuple<const char*, float, float>{"wow 0.3 %", 0.3f, 0.0f},
                                                  {"wow 0.1 %", 0.1f, 0.0f}, {"flutter 0.2 %", 0.0f, 0.2f},
                                                  {"flutter 0.05 %", 0.0f, 0.05f}}) {
            auto p = measuring(s, fs, 1, {{"motion", 1.0f}, {"wow", wow}, {"flutter", flutter}});
            const bool is_wow = wow > 0.0f;
            const double fm = is_wow ? tape::kWowHz : tape::kFlutterHz;
            const double secs = is_wow ? 7.0 : 3.0;
            const Chans x = measure::sine(fs, 3000.0, -12.0, secs, 1);
            const Chans y = aligned(*p, x);
            double mean = 0.0;
            // Start after a full modulation period so the delay has settled,
            // and project over whole periods of it.
            const std::size_t from = static_cast<std::size_t>(1.0 * fs);
            const std::size_t whole = static_cast<std::size_t>(std::floor((secs - 1.0 - 0.01) * fm) / fm * fs);
            std::vector<float> yy(y[0].begin(), y[0].begin() + static_cast<std::ptrdiff_t>(from + whole + static_cast<std::size_t>(std::round(10.0 * fs / 3000.0))));
            const double got = 100.0 * measured_deviation(yy, fs, 3000.0, fm, from, &mean);
            const double want = is_wow ? wow : flutter;
            s.check_le(std::string("peak speed deviation, ") + name + ", " + hz(fs) + ": |measured/set - 1|",
                       std::fabs(got / want - 1.0), tol, "ratio", fmt("measured %.4f %%", got));
            json::Value r = json::Value::array();
            r.push(jf(fs));
            r.push(jf(want));
            r.push(jf(got));
            rows.push(r);
        }
    }
    s.data().set("motion_rate_set_measured_percent", rows);
}

// ---- hot input --------------------------------------------------------------------

void hot_input(Suite& s) {
    const double fs = 48000.0;
    const double bound = s.bar("hot.max_abs");
    for (int q = 0; q < 3; ++q) {
        double worst = 0.0;
        bool finite = true;
        for (double level : {0.0, 12.0, 24.0}) {
            auto p = measuring(s, fs, 2, {{"quality", static_cast<float>(q)}, {"drive", 24.0f}, {"bump", 100.0f}});
            Chans x = measure::sine(fs, 187.5, level, 0.5, 2);
            for (auto& c : x)
                for (float& v : c) v = v >= 0.0f ? static_cast<float>(dsp::db_to_gain(level)) : -static_cast<float>(dsp::db_to_gain(level));
            const Chans y = measure::render(*p, x);
            for (const auto& c : y)
                for (float v : c) {
                    finite = finite && std::isfinite(v);
                    worst = std::fmax(worst, std::fabs(v));
                }
        }
        s.check(std::string("hot square waves 0, +12, +24 dBFS, drive 24 dB, ") + kQualityIds[q] + ": output finite and bounded",
                worst, bound, finite && worst <= bound, "FS");
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
        full.push_back({"drive", 24.0f});
        full.push_back({"motion", 1.0f});
        measure::check_cpu_on(s, c, programme, names[q], assert_bar);
        measure::check_cpu_on(s, full, hot, std::string(names[q]) + ", programme +12 dB, drive 24 dB, motion on", assert_bar);
    }
}

}  // namespace

measure::Config tape_golden_config() {
    return {{"drive", 6.0f}, {"speed", 1.0f}, {"eq", 0.0f}, {"bump", 60.0f}, {"quality", 0.0f}};
}

int tape(const measure::Args& args) {
    Suite s("tape", [] { return make_tape(); }, args);
    against_reference(s);
    response(s);
    distortion(s);
    aliasing(s);
    motion(s);
    hot_input(s);
    const Config golden = tape_golden_config();
    std::vector<std::pair<std::string, Config>> latency;
    for (int q = 0; q < 3; ++q) {
        const float qf = static_cast<float>(q);
        const std::string n = kQualityIds[q];
        // 30 in/s with the bump off: the latency is the oversampler's and
        // the motion line's. The head-loss low-pass and the bump are
        // magnitude shaping with their own group delay (about 90 us at 7.5
        // in/s with the bump at 38 Hz), which a cross-correlation on noise
        // reads as extra samples but PDC must not compensate.
        const Config eqless = {{"quality", qf}, {"speed", 2.0f}, {"bump", 0.0f}};
        auto with = [&](Config c, std::initializer_list<std::pair<const char*, float>> more) {
            for (const auto& m : more) c.push_back({m.first, m.second});
            return c;
        };
        latency.push_back({n + ", motion off", eqless});
        latency.push_back({n + ", motion on, wow 0, flutter 0", with(eqless, {{"motion", 1.0f}, {"wow", 0.0f}, {"flutter", 0.0f}})});
        // Wow and flutter at zero: the default depths move the delay by several
        // samples by design, and a correlation on noise reads that as an offset.
        latency.push_back({n + ", motion on, wow 0, flutter 0, IEC", with(eqless, {{"motion", 1.0f}, {"wow", 0.0f}, {"flutter", 0.0f}, {"eq", 1.0f}})});
    }
    measure::check_latency(s, latency);
    measure::check_nonfinite(s, golden);
    measure::check_reset(s, golden);
    measure::check_denormals(s, golden);
    // Zipper sweeps with the motion on, so wow and flutter are swept too;
    // drive 12 dB so the tone is in the curve.
    measure::check_zipper(s, {{"motion", 1.0f}, {"drive", 12.0f}}, "");
    measure::check_golden(s, golden);
    cpu_qualities(s, golden);
    return s.finish();
}

}  // namespace sonora::native::suites
