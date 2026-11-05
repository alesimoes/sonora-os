// SPDX-License-Identifier: GPL-3.0-or-later
//
// The passive EQ against its bars (quality.json).
// - Response: the processor's gain at a tone, against the analog prototypes
//   of its sections multiplied together (the reference is the design, not a
//   unit), for the knob settings that matter: each path alone, the
//   simultaneous low boost and cut, all four at once.
// - What the network does: flat knobs are unity; equal low boost and cut is
//   a bump below the corner and a dip above it; the high bandwidth knob
//   narrows the bell.
// - Iron on: small-signal gain unity from 40 Hz up, distortion rises with
//   level and falls with frequency, hot input stays finite.
// - The common checks (latency, non-finite, reset, denormals, zipper, golden
//   files, CPU).
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/passiveeq.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

std::string fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
    char buf[240];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

json::Value jf(double v) { return json::Value::number_float(static_cast<float>(v)); }

double db(double lin) { return 20.0 * std::log10(std::fmax(lin, 1e-30)); }

std::unique_ptr<Processor> measuring(const Suite& s, double fs, std::uint32_t channels, const Config& c) {
    auto p = s.make();
    measure::apply(*p, {{"output", 0.0f}});
    measure::apply(*p, c);
    p->prepare(fs, 128, channels);
    return p;
}

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

double run_gain_db(const Suite& s, double fs, double f, double dbfs, const Config& c) {
    auto p = measuring(s, fs, 1, c);
    const double settle = std::fmax(0.5, 8.0 / f);
    const Chans y = measure::render(*p, measure::sine(fs, f, dbfs, settle + 1.0, 1));
    return db(tone_amp(y[0], static_cast<std::size_t>(settle * fs), fs, f)) - dbfs;
}

double thd_db(const Suite& s, double fs, double f, double dbfs, const Config& c) {
    auto p = measuring(s, fs, 1, c);
    const double settle = std::fmax(0.5, 8.0 / f);
    const Chans y = measure::render(*p, measure::sine(fs, f, dbfs, settle + 1.0, 1));
    return measure::thdn_db(y[0], static_cast<std::size_t>(settle * fs), fs, f);
}

// The analog target of a setting: the sections the processor builds, in the
// same order, as the prototypes of common/dsp/biquad.h.
struct Setting {
    const char* name;
    double lf_boost, lf_atten, hf_boost, hf_q, hf_atten;
    int lf_i, hf_i, ha_i;
};

double target_db(const Setting& st, double f) {
    using namespace passiveeq;
    const double lf = kLfFreqs[st.lf_i];
    const dsp::FilterSpec specs[] = {
        {dsp::Shape::LowShelf, lf, kLfBoostQ, st.lf_boost},
        {dsp::Shape::LowShelf, lf * kCutRatio, kLfCutQ, -st.lf_atten * kCutScale},
        {dsp::Shape::Peak, kHfFreqs[st.hf_i], st.hf_q, st.hf_boost},
        {dsp::Shape::HighShelf, kHfAttenFreqs[st.ha_i], kHfCutQ, -st.hf_atten},
    };
    std::complex<double> h = 1.0;
    for (const auto& sp : specs) h *= dsp::analog_response(sp, f);
    return db(std::abs(h));
}

Config config_of(const Setting& st) {
    return {{"lf_boost", static_cast<float>(st.lf_boost)},
            {"lf_atten", static_cast<float>(st.lf_atten)},
            {"lf_freq", static_cast<float>(st.lf_i)},
            {"hf_boost", static_cast<float>(st.hf_boost)},
            {"hf_q", static_cast<float>(st.hf_q)},
            {"hf_freq", static_cast<float>(st.hf_i)},
            {"hf_atten", static_cast<float>(st.hf_atten)},
            {"hf_atten_freq", static_cast<float>(st.ha_i)},
            {"iron", 0.0f}};
}

const Setting kSettings[] = {
    {"low boost 10 dB at 60 Hz", 10, 0, 0, 0.8, 0, 2, 2, 1},
    {"low cut 10 dB at 100 Hz", 0, 10, 0, 0.8, 0, 3, 2, 1},
    {"low boost and cut 10/10 dB at 60 Hz", 10, 10, 0, 0.8, 0, 2, 2, 1},
    {"high boost 12 dB at 8 kHz, Q 1.5", 0, 0, 12, 1.5, 0, 2, 3, 1},
    {"high boost 12 dB at 5 kHz, Q 0.4", 0, 0, 12, 0.4, 0, 2, 2, 1},
    {"high cut 10 dB at 10 kHz", 0, 0, 0, 0.8, 10, 2, 2, 1},
    {"all four", 8, 6, 9, 1.0, 5, 1, 4, 0},
};

// ---- response ---------------------------------------------------------------------

void response(Suite& s) {
    json::Value rows = json::Value::array();
    const std::vector<double> rates = measure::quick() ? std::vector<double>{48000.0}
                                                       : std::vector<double>{44100.0, 48000.0, 96000.0};
    const double freqs[] = {20.0, 40.0, 80.0, 150.0, 300.0, 1000.0, 3000.0, 6000.0, 10000.0, 14000.0};
    for (const auto& st : kSettings) {
        for (double fs : rates) {
            double worst = 0.0, worst_f = 0.0;
            for (double f : freqs) {
                if (f > 0.33 * fs) continue;
                const double got = run_gain_db(s, fs, f, -30.0, config_of(st));
                const double err = std::fabs(got - target_db(st, f));
                if (err > worst) {
                    worst = err;
                    worst_f = f;
                }
                json::Value r = json::Value::object();
                r.set("setting", json::Value::string(st.name));
                r.set("rate", jf(fs));
                r.set("hz", jf(f));
                r.set("db", jf(got));
                r.set("target_db", jf(target_db(st, f)));
                rows.push(r);
            }
            s.check_le(std::string(st.name) + fmt(", %.0f Hz: worst distance from the analog design", fs),
                       worst, s.bar("response.magnitude_db"), "dB", fmt("at %.0f Hz", worst_f));
        }
    }
    s.data().set("response", rows);
}

// ---- what the network does --------------------------------------------------------

void network(Suite& s) {
    const double fs = 48000.0;
    {
        double worst = 0.0;
        for (double f : {30.0, 200.0, 1000.0, 5000.0, 12000.0})
            worst = std::fmax(worst, std::fabs(run_gain_db(s, fs, f, -20.0, {{"iron", 0.0f}})));
        s.check_le("all knobs at zero: worst |gain|, 30 Hz to 12 kHz", worst, s.bar("flat_db"), "dB");
    }
    {
        // Equal boost and cut at 60 Hz: a bump below the corner and a dip above.
        const Setting& st = kSettings[2];
        const double low = run_gain_db(s, fs, 20.0, -30.0, config_of(st));
        double dip = 0.0;
        for (double f : {90.0, 120.0, 150.0, 200.0}) dip = std::fmin(dip, run_gain_db(s, fs, f, -30.0, config_of(st)));
        s.check("equal low boost and cut (10/10 dB, 60 Hz): bump at 20 Hz of at least", low,
                s.bar("interaction.bump_db"), low >= s.bar("interaction.bump_db"), "dB");
        s.check("equal low boost and cut (10/10 dB, 60 Hz): dip between 90 and 200 Hz of at least", -dip,
                s.bar("interaction.dip_db"), -dip >= s.bar("interaction.dip_db"), "dB");
    }
    {
        // Bandwidth: at the same gain, the sharper bell is narrower. The gain
        // an octave above the centre is lower at Q 1.5 than at Q 0.4.
        const double wide = run_gain_db(s, fs, 10000.0, -30.0, config_of(kSettings[4]));
        const double narrow = run_gain_db(s, fs, 10000.0, -30.0, [] {
            Config c = config_of(kSettings[4]);
            c.push_back({"hf_q", 1.5f});
            return c;
        }());
        s.check("high boost 12 dB at 5 kHz: gain an octave above at Q 0.4 minus at Q 1.5, at least", wide - narrow,
                s.bar("bandwidth_db"), wide - narrow >= s.bar("bandwidth_db"), "dB",
                fmt("Q 0.4 %.2f dB, ", wide) + fmt("Q 1.5 %.2f dB", narrow));
    }
    {
        // The output knob is a plain gain.
        const double g = run_gain_db(s, fs, 1000.0, -20.0, {{"iron", 0.0f}, {"output", 6.0f}});
        s.check_le("output +6 dB: gain error", std::fabs(g - 6.0), s.bar("flat_db"), "dB");
    }
}

// ---- iron -------------------------------------------------------------------------

void iron(Suite& s) {
    const double fs = 48000.0;
    json::Value rows = json::Value::array();
    {
        double worst = 0.0;
        for (double f : {40.0, 200.0, 1000.0, 10000.0})
            for (float drive : {0.0f, 12.0f})
                worst = std::fmax(worst, std::fabs(run_gain_db(s, fs, f, -70.0, {{"iron", 1.0f}, {"drive", drive}})));
        s.check_le("iron on, flat knobs, -70 dBFS: worst |gain|, 40 Hz to 10 kHz, drive 0 and 12 dB", worst,
                   s.bar("iron.small_signal_db"), "dB");
    }
    const Config cfg = {{"iron", 1.0f}, {"drive", 12.0f}};
    double t[2][3];
    const double freqs[] = {1000.0, 40.0};
    const double levels[] = {-30.0, -12.0, 0.0};
    for (int fi = 0; fi < 2; ++fi) {
        for (int li = 0; li < 3; ++li) {
            t[fi][li] = thd_db(s, fs, freqs[fi], levels[li], cfg);
            json::Value r = json::Value::object();
            r.set("hz", jf(freqs[fi]));
            r.set("dbfs", jf(levels[li]));
            r.set("thd_db", jf(t[fi][li]));
            rows.push(r);
        }
    }
    s.data().set("thd", rows);
    s.check("iron on, drive 12 dB, 1 kHz: THD+N rises with level (-30, -12, 0 dBFS)", t[0][2], t[0][0],
            t[0][0] < t[0][1] && t[0][1] < t[0][2], "dB",
            fmt("%.1f, ", t[0][0]) + fmt("%.1f, ", t[0][1]) + fmt("%.1f dB", t[0][2]));
    s.check("iron on, drive 12 dB, 0 dBFS: THD+N at 40 Hz above 1 kHz by at least", t[1][2] - t[0][2],
            s.bar("iron.lf_first_db"), t[1][2] - t[0][2] >= s.bar("iron.lf_first_db"), "dB");
    {
        // Linear, iron off: THD+N at the converter's floor, even at full scale.
        const double lin = thd_db(s, fs, 1000.0, 0.0, {{"iron", 0.0f}, {"lf_boost", 10.0f}});
        s.check_le("iron off, low boost 10 dB, 0 dBFS at 1 kHz: THD+N", lin, s.bar("linear_thdn_db"), "dB");
    }
}

// ---- stereo, hot input ------------------------------------------------------------

void stereo_and_hot(Suite& s) {
    const double fs = 48000.0;
    {
        Chans x = measure::noise(fs, -12.0, 0.5, 7, 1);
        x.push_back(x[0]);
        const Config c = {{"lf_boost", 8.0f}, {"hf_boost", 6.0f}, {"iron", 1.0f}, {"drive", 12.0f}};
        auto st = measuring(s, fs, 2, c);
        auto mo = measuring(s, fs, 1, c);
        const Chans ys = measure::render(*st, x);
        const Chans ym = measure::render(*mo, {x[0]});
        double diff = 0.0;
        for (std::size_t i = 0; i < ym[0].size(); ++i)
            diff = std::fmax(diff, std::fabs(static_cast<double>(ys[0][i]) - ym[0][i]));
        s.check_le("stereo channel against the mono render of the same signal: max |difference|", diff, 0.0, "FS");
    }
    double worst = 0.0;
    bool finite = true;
    for (int iron_on = 0; iron_on < 2; ++iron_on) {
        for (double dbfs : {0.0, 12.0, 24.0}) {
            auto p = measuring(s, fs, 2,
                               {{"lf_boost", 14.0f}, {"hf_boost", 16.0f}, {"iron", static_cast<float>(iron_on)},
                                {"drive", 24.0f}});
            Chans x = measure::silence(fs, 0.3);
            const auto amp = static_cast<float>(std::pow(10.0, dbfs / 20.0));
            for (auto& ch : x)
                for (std::size_t i = 0; i < ch.size(); ++i)
                    ch[i] = std::fmod(30.0 * static_cast<double>(i) / fs, 1.0) < 0.5 ? amp : -amp;
            const Chans y = measure::render(*p, x);
            for (const auto& ch : y)
                for (float v : ch) {
                    finite &= std::isfinite(v);
                    worst = std::fmax(worst, std::fabs(v));
                }
        }
    }
    s.check("hot 30 Hz squares 0, +12, +24 dBFS, full boost, iron off and on: output finite and bounded", worst,
            s.bar("hot.max_abs"), finite && worst <= s.bar("hot.max_abs"), "FS");
}

}  // namespace

measure::Config passiveeq_golden_config() {
    return {{"lf_boost", 6.0f}, {"lf_atten", 3.0f}, {"hf_boost", 5.0f}, {"hf_atten", 2.0f}, {"iron", 1.0f},
            {"drive", 6.0f}, {"output", -1.0f}};
}

int passiveeq(const measure::Args& args) {
    Suite s("passiveeq", [] { return make_passiveeq(); }, args);
    response(s);
    network(s);
    iron(s);
    stereo_and_hot(s);
    const Config golden = passiveeq_golden_config();
    std::vector<std::pair<std::string, Config>> latency;
    latency.push_back({"iron off", {{"iron", 0.0f}}});
    latency.push_back({"iron on", {{"iron", 1.0f}}});
    measure::check_common(s, golden, latency);
    return s.finish();
}

}  // namespace sonora::native::suites
