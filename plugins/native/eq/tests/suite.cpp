// SPDX-License-Identifier: GPL-3.0-or-later
//
// The EQ against its bars (quality.json): magnitude and phase against the
// analog prototype of the whole curve, THD+N, and the common checks.
#include <cmath>
#include <complex>
#include <string>
#include <vector>

#include "sonora/dsp/biquad.h"
#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/eq.h"

namespace sonora::native::suites {

namespace {

using measure::Config;
using measure::Suite;

// The analog target of a configuration: the product of every active
// band's prototype, built from the same parameter values.
std::complex<double> target(const Processor& p, double f) {
    using namespace eq;
    std::complex<double> h(1.0, 0.0);
    auto v = [&](std::uint32_t i) { return static_cast<double>(p.param(i)); };
    for (int g = 0; g < 2; ++g) {
        if (v(g == 0 ? kHpfOn : kLpfOn) < 0.5) continue;
        dsp::FilterSpec secs[5];
        const int order = kSlopes[static_cast<int>(v(g == 0 ? kHpfSlope : kLpfSlope))] / 6;
        const int n = dsp::butterworth_sections(order, g == 0, v(g == 0 ? kHpfFreq : kLpfFreq), secs);
        for (int k = 0; k < n; ++k) h *= dsp::analog_response(secs[k], f);
    }
    if (v(kLowShelfOn) >= 0.5) {
        h *= dsp::analog_response({dsp::Shape::LowShelf, v(kLowShelfFreq), v(kLowShelfQ), v(kLowShelfGain)}, f);
    }
    if (v(kHighShelfOn) >= 0.5) {
        h *= dsp::analog_response({dsp::Shape::HighShelf, v(kHighShelfFreq), v(kHighShelfQ), v(kHighShelfGain)}, f);
    }
    for (int b = 0; b < 4; ++b) {
        if (v(bell(b, 0)) < 0.5) continue;
        h *= dsp::analog_response({dsp::Shape::Peak, v(bell(b, 1)), v(bell(b, 3)), v(bell(b, 2))}, f);
    }
    return h * std::pow(10.0, v(kOutput) / 20.0);
}

struct Curve {
    const char* name;
    Config cfg;
    bool phase;  // minimum-phase bands only, f0 <= 2 kHz: the phase bar applies
};

std::vector<Curve> curves() {
    return {
        {"bells: +12 dB @ 100 Hz Q 0.7, -9 dB @ 1 kHz Q 4, +6 dB @ 2 kHz Q 0.4",
         {{"bell1_freq", 100.0f}, {"bell1_gain", 12.0f}, {"bell1_q", 0.7f},
          {"bell2_freq", 1000.0f}, {"bell2_gain", -9.0f}, {"bell2_q", 4.0f},
          {"bell3_freq", 2000.0f}, {"bell3_gain", 6.0f}, {"bell3_q", 0.4f}},
         true},
        {"air: bell +18 dB @ 10 kHz Q 1, bell -18 dB @ 15 kHz Q 0.4",
         {{"bell3_freq", 10000.0f}, {"bell3_gain", 18.0f}, {"bell3_q", 1.0f},
          {"bell4_freq", 15000.0f}, {"bell4_gain", -18.0f}, {"bell4_q", 0.4f}},
         false},
        {"top bell: +12 dB @ 19 kHz Q 2",
         {{"bell4_freq", 19000.0f}, {"bell4_gain", 12.0f}, {"bell4_q", 2.0f}}, false},
        {"shelves: +9 dB below 120 Hz, -12 dB above 8 kHz",
         {{"low_shelf_freq", 120.0f}, {"low_shelf_gain", 9.0f},
          {"high_shelf_freq", 8000.0f}, {"high_shelf_gain", -12.0f}},
         false},
        {"shelves: -18 dB below 1 kHz Q 1.5, +18 dB above 15 kHz Q 0.5",
         {{"low_shelf_freq", 1000.0f}, {"low_shelf_gain", -18.0f}, {"low_shelf_q", 1.5f},
          {"high_shelf_freq", 15000.0f}, {"high_shelf_gain", 18.0f}, {"high_shelf_q", 0.5f}},
         false},
        {"low shelves only: +12 dB below 200 Hz, bell -6 dB @ 400 Hz",
         {{"low_shelf_freq", 200.0f}, {"low_shelf_gain", 12.0f},
          {"bell1_freq", 400.0f}, {"bell1_gain", -6.0f}},
         true},
        {"HPF 80 Hz 24 dB/oct, LPF 16 kHz 12 dB/oct",
         {{"hpf_on", 1.0f}, {"hpf_freq", 80.0f}, {"hpf_slope", 3.0f},
          {"lpf_on", 1.0f}, {"lpf_freq", 16000.0f}, {"lpf_slope", 1.0f}},
         false},
        {"HPF 2 kHz 48 dB/oct, LPF 20 kHz 6 dB/oct",
         {{"hpf_on", 1.0f}, {"hpf_freq", 2000.0f}, {"hpf_slope", 5.0f},
          {"lpf_on", 1.0f}, {"lpf_freq", 20000.0f}, {"lpf_slope", 0.0f}},
         false},
        {"HPF 30 Hz 18 dB/oct, LPF 8 kHz 36 dB/oct",
         {{"hpf_on", 1.0f}, {"hpf_freq", 30.0f}, {"hpf_slope", 2.0f},
          {"lpf_on", 1.0f}, {"lpf_freq", 8000.0f}, {"lpf_slope", 4.0f}},
         false},
    };
}

void response(Suite& s, float mode, const char* mode_name, bool assert_bar) {
    const double mag_bar = s.bar("eq.magnitude_db");
    const double ph_bar = s.bar("eq.phase_deg");
    json::Value plot = json::Value::array();
    for (const Curve& cv : curves()) {
        for (double fs : measure::kRates) {
            auto p = s.make();
            measure::set(*p, "mode", mode);
            measure::apply(*p, cv.cfg);
            p->prepare(fs, 128, 2);
            const std::uint32_t lat = p->latency();
            measure::Chans in = measure::silence(fs, 0.5);
            in[0][0] = in[1][0] = 1.0f;  // the response, whole: pre-ringing included
            const measure::Chans out = measure::render(*p, in);
            const std::vector<float>& ir = out[0];
            double worst_mag = 0.0, worst_ph = 0.0;
            json::Value pts = json::Value::array();
            for (int i = 0; i < 160; ++i) {
                const double f = 20.0 * std::pow(1000.0, i / 159.0);
                const auto [db, deg] = measure::response_db_deg(ir, f, fs, lat);
                const std::complex<double> t = target(*p, f);
                const double tdb = 20.0 * std::log10(std::abs(t));
                const double tdeg = std::arg(t) * 180.0 / dsp::kPi;
                if (tdb >= -20.0) worst_mag = std::fmax(worst_mag, std::fabs(db - tdb));
                if (cv.phase && f <= 10000.0) {
                    double dp = deg - tdeg;
                    dp -= 360.0 * std::round(dp / 360.0);
                    worst_ph = std::fmax(worst_ph, std::fabs(dp));
                }
                if (fs == 48000.0) {
                    json::Value pt = json::Value::array();
                    pt.push(json::Value::number_float(static_cast<float>(f)));
                    pt.push(json::Value::number_float(static_cast<float>(db)));
                    pt.push(json::Value::number_float(static_cast<float>(tdb)));
                    pts.push(std::move(pt));
                }
            }
            const std::string what = std::string(mode_name) + ", " + cv.name + ", " +
                                     std::to_string(static_cast<int>(fs)) + " Hz";
            if (assert_bar) s.check_le("magnitude vs analog, " + what, worst_mag, mag_bar, "dB");
            else s.info("magnitude vs analog, " + what, worst_mag, "dB", "reported, not a bar");
            if (cv.phase) {
                if (assert_bar) s.check_le("phase vs analog to 10 kHz, " + what, worst_ph, ph_bar, "deg");
                else s.info("phase vs analog to 10 kHz, " + what, worst_ph, "deg", "reported, not a bar");
            }
            if (fs == 48000.0) {
                json::Value c = json::Value::object();
                c.set("mode", json::Value::string(mode_name));
                c.set("curve", json::Value::string(cv.name));
                c.set("points_hz_db_target", std::move(pts));
                plot.push(std::move(c));
            }
        }
    }
    s.data().set(std::string("response_") + (mode >= 0.5f ? "2x" : "1x"), std::move(plot));
}

void thd(Suite& s) {
    const double bar = s.bar("eq.thdn_db");
    for (float mode : {0.0f, 1.0f}) {
        for (double fs : {44100.0, 48000.0}) {
            auto p = s.make();
            measure::apply(*p, {{"mode", mode}, {"bell2_gain", 6.0f}, {"bell3_gain", -6.0f}, {"high_shelf_gain", 3.0f}});
            p->prepare(fs, 128, 2);
            // -1 dBFS out of the EQ: 1 kHz sits under bell 2 (+6 dB), so
            // the input is lowered by the measured gain there.
            const measure::Chans probe = measure::render(*p, measure::sine(fs, 1000.0, -20.0, 0.5));
            const double g = measure::rms(probe[0], probe[0].size() / 2) / (std::pow(10.0, -20.0 / 20.0) / std::sqrt(2.0));
            p->reset();
            const double in_db = -1.0 - 20.0 * std::log10(g);
            const measure::Chans y = measure::render(*p, measure::sine(fs, 1000.0, in_db, 1.2));
            const double t = measure::thdn_db(y[0], static_cast<std::size_t>(0.4 * fs), fs, 1000.0);
            s.check_le(std::string("THD+N at -1 dBFS out, 1 kHz, ") + (mode >= 0.5f ? "2x" : "1x") + ", " +
                           std::to_string(static_cast<int>(fs)) + " Hz",
                       t, bar, "dB");
        }
    }
}

}  // namespace

int eq(const measure::Args& args) {
    Suite s("eq", [] { return make_eq(); }, args);
    response(s, 1.0f, "analog matched (2x)", true);
    response(s, 0.0f, "zero latency (1x)", false);
    thd(s);
    const Config golden = {{"bell1_freq", 120.0f}, {"bell1_gain", 6.0f}, {"bell2_freq", 900.0f},
                           {"bell2_gain", -4.0f}, {"bell2_q", 3.0f}, {"high_shelf_gain", 4.0f},
                           {"hpf_on", 1.0f}, {"hpf_freq", 40.0f}};
    // Every band audible, so each sweep moves something the tone hears.
    const Config zipper = {{"hpf_on", 1.0f}, {"hpf_freq", 30.0f}, {"lpf_on", 1.0f},
                           {"low_shelf_gain", 6.0f}, {"high_shelf_gain", -6.0f},
                           {"bell1_freq", 100.0f}, {"bell1_gain", 9.0f},
                           {"bell4_freq", 150.0f}, {"bell4_gain", -9.0f}};
    measure::check_common(s, golden,
                          {{"analog matched (2x)", {{"mode", 1.0f}, {"bell2_gain", 3.0f}}},
                           {"zero latency (1x)", {{"mode", 0.0f}, {"bell2_gain", 3.0f}}}},
                          &zipper);
    // The zero-latency mode is the EQ's eco mode (ADR-0011 §3.5): its CPU
    // is measured too.
    Config eco = golden;
    eco.push_back({"mode", 0.0f});
    measure::check_cpu(s, eco, ", zero latency (1x)");
    return s.finish();
}

}  // namespace sonora::native::suites
