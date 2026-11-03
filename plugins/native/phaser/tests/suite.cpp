// SPDX-License-Identifier: GPL-3.0-or-later
//
// The phaser and flanger against their bars. Phaser: the cascade is
// allpass (flat magnitude), carries -90 degrees per stage at the set
// frequency, and puts its lowest notch where theory says. The sweep
// follows the LFO. Flanger: the delay is exact at zero depth, and its
// range and period are measured from an impulse train. Both: feedback
// stays bounded, plus the common checks.
#include <cmath>
#include <string>
#include <vector>

#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"
#include "sonora/native/phaser.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

const char* const kStageNames[] = {"2", "4", "6", "8", "12"};
constexpr int kStages[] = {2, 4, 6, 8, 12};

std::string hz(double fs) { return std::to_string(static_cast<int>(fs)) + " Hz"; }

double wrap_deg(double d) {
    while (d > 180.0) d -= 360.0;
    while (d <= -180.0) d += 360.0;
    return d;
}

// Magnitude flat and phase -90 degrees per stage at the set frequency,
// from the impulse response, at every rate.
void allpass_response(Suite& s) {
    const double f0 = 1000.0;
    for (double fs : measure::kRates) {
        double worst_mag = 0.0, worst_phase = 0.0;
        for (int si = 0; si < 5; ++si) {
            auto p = s.make();
            measure::apply(*p, {{"mode", 0.0f}, {"depth", 0.0f}, {"freq", static_cast<float>(f0)},
                                {"stages", static_cast<float>(si)}, {"feedback", 0.0f}, {"mix", 100.0f}});
            p->prepare(fs, 128, 2);
            Chans x = measure::silence(fs, 1.0);
            x[0][0] = 1.0f;
            const Chans y = measure::render(*p, x);
            for (double f : {100.0, 1000.0, 5000.0, 15000.0}) {
                const auto [db, deg] = measure::response_db_deg(y[0], f, fs);
                worst_mag = std::fmax(worst_mag, std::fabs(db));
                if (f == f0) worst_phase = std::fmax(worst_phase, std::fabs(wrap_deg(deg + 90.0 * kStages[si])));
            }
        }
        s.check_le("phaser: |gain| over 100 Hz-15 kHz, every stage count, " + hz(fs), worst_mag,
                   s.bar("phaser.allpass_db"), "dB");
        s.check_le("phaser: phase at the set frequency vs -90 deg per stage, " + hz(fs), worst_phase,
                   s.bar("phaser.phase_deg"), "deg");
    }
}

// The lowest notch of n allpass stages mixed 50/50 with the dry signal is
// where each stage has turned 180/n degrees: tan(pi f / fs) = tan(pi f0 / fs)
// tan(90/n deg). A sine there must all but vanish.
void notch(Suite& s) {
    const double f0 = 1000.0;
    for (double fs : measure::kRates) {
        double worst = -300.0;
        for (int si = 0; si < 5; ++si) {
            const double n = kStages[si];
            const double t0 = std::tan(dsp::kPi * f0 / fs);
            const double f = std::atan(t0 * std::tan(0.5 * dsp::kPi / n)) * fs / dsp::kPi;
            auto p = s.make();
            measure::apply(*p, {{"mode", 0.0f}, {"depth", 0.0f}, {"freq", static_cast<float>(f0)},
                                {"stages", static_cast<float>(si)}, {"feedback", 0.0f}, {"mix", 50.0f}});
            p->prepare(fs, 128, 2);
            const Chans x = measure::sine(fs, f, -6.0, 1.0);
            const Chans y = measure::render(*p, x);
            const std::size_t from = static_cast<std::size_t>(0.5 * fs);
            worst = std::fmax(worst, measure::to_db(measure::rms(y[0], from) / measure::rms(x[0], from)));
        }
        s.check_le("phaser: lowest notch of 2-12 stages at the predicted frequency, " + hz(fs), worst,
                   s.bar("phaser.notch_db"), "dB");
    }
}

// The notch follows the LFO: a sine parked on the notch of the break
// frequency at the LFO's top (centre x 2^(2 depth)) dips there, and one
// parked on the bottom's dips half a period later.
void sweep(Suite& s) {
    const double fs = 48000.0, rate = 0.5, f0 = 1000.0;
    const double octaves = 2.0 * 0.5;  // depth 50 %
    const double r = std::tan(0.5 * dsp::kPi / 4.0);
    auto notch_at = [&](double brk) { return std::atan(std::tan(dsp::kPi * brk / fs) * r) * fs / dsp::kPi; };
    struct Probe {
        const char* what;
        double brk;
        double t_dip;
    } probes[] = {{"top", f0 * std::exp2(octaves), 0.25 / rate}, {"bottom", f0 * std::exp2(-octaves), 0.75 / rate}};
    for (const Probe& pr : probes) {
        auto p = s.make();
        measure::apply(*p, {{"mode", 0.0f}, {"rate", static_cast<float>(rate)}, {"depth", 50.0f},
                            {"freq", static_cast<float>(f0)}, {"stages", 1.0f}, {"feedback", 0.0f},
                            {"mix", 50.0f}, {"spread", 0.0f}});
        p->prepare(fs, 128, 2);
        const Chans x = measure::sine(fs, notch_at(pr.brk), -6.0, 2.0);
        const Chans y = measure::render(*p, x);
        // 2 ms windows; the dip against the loudest window of the cycle.
        const std::size_t w = static_cast<std::size_t>(0.002 * fs);
        const std::size_t centre = static_cast<std::size_t>(pr.t_dip * fs);
        double dip = 1e9, top = 0.0;
        for (std::size_t i = static_cast<std::size_t>(0.1 * fs); i + w < y[0].size(); i += w) {
            const double v = measure::rms(y[0], i, i + w);
            top = std::fmax(top, v);
            if (i + w > centre - 4 * w && i < centre + 4 * w) dip = std::fmin(dip, v);
        }
        s.check_le(std::string("phaser: sweep, notch at the LFO ") + pr.what + " (re loudest window)",
                   measure::to_db(dip / top), s.bar("phaser.sweep_db"), "dB");
    }
}

// Feedback at its limit must stay bounded: the loop gain of 0.85 puts the
// ceiling at 1 / (1 - 0.85) = 6.7 on a full-scale input.
void feedback_bound(Suite& s) {
    const double fs = 48000.0;
    for (int mode = 0; mode < 2; ++mode) {
        for (float fb : {85.0f, -85.0f}) {
            auto p = s.make();
            measure::apply(*p, {{"mode", static_cast<float>(mode)}, {"stages", 4.0f}, {"feedback", fb},
                                {"depth", 100.0f}, {"rate", 3.0f}, {"mix", 100.0f}});
            p->prepare(fs, 128, 2);
            const Chans y = measure::render(*p, measure::noise(fs, 0.0, 3.0, 7));
            s.check_le(std::string(mode ? "flanger" : "phaser") + ": peak at feedback " +
                           (fb > 0 ? "+85" : "-85") + " % on full-scale noise",
                       measure::peak(y[0]), 8.0, "linear");
        }
    }
}

void flanger_delay(Suite& s) {
    for (double fs : measure::kRates) {
        auto p = s.make();
        measure::apply(*p, {{"mode", 1.0f}, {"depth", 0.0f}, {"delay", 10.0f}, {"feedback", 0.0f},
                            {"mix", 100.0f}});
        p->prepare(fs, 128, 2);
        Chans x = measure::silence(fs, 0.1);
        x[0][100] = 1.0f;
        const Chans y = measure::render(*p, x);
        std::size_t pk = 0;
        for (std::size_t i = 0; i < y[0].size(); ++i) {
            if (std::fabs(y[0][i]) > std::fabs(y[0][pk])) pk = i;
        }
        const long err = static_cast<long>(pk) - 100 - std::lround(0.010 * fs);
        s.check("flanger: depth 0: impulse at the set delay (10 ms), " + hz(fs) + ": error",
                static_cast<double>(err), 0.0, err == 0 && std::fabs(y[0][pk] - 1.0f) < 1e-6f, "samples");
    }
}

// Depth 50 % is one octave either way: a 4 ms delay sweeps 2 to 8 ms.
void flanger_range(Suite& s) {
    const double fs = 48000.0, rate = 2.0, delay_ms = 4.0;
    auto p = s.make();
    measure::apply(*p, {{"mode", 1.0f}, {"rate", static_cast<float>(rate)}, {"depth", 50.0f},
                        {"delay", static_cast<float>(delay_ms)}, {"feedback", 0.0f}, {"mix", 100.0f}});
    p->prepare(fs, 128, 2);
    const std::size_t every = static_cast<std::size_t>(0.010 * fs);
    Chans x = measure::silence(fs, 2.0);
    for (std::size_t i = 0; i < x[0].size(); i += every) x[0][i] = 1.0f;
    const Chans y = measure::render(*p, x);
    double lo = 1e9, hi = -1e9;
    std::vector<double> d;
    for (std::size_t i = 0; i + every < x[0].size(); i += every) {
        std::size_t pk = i;
        for (std::size_t k = i; k < i + every; ++k) {
            if (std::fabs(y[0][k]) > std::fabs(y[0][pk])) pk = k;
        }
        const double a = y[0][pk - 1], b = y[0][pk], c = y[0][pk + 1];
        const double off = 0.5 * (a - c) / (a - 2.0 * b + c);
        const double ms = (pk - i + off) / fs * 1e3;
        d.push_back(ms);
        lo = std::fmin(lo, ms);
        hi = std::fmax(hi, ms);
    }
    const double tol_ms = 2.0 / fs * 1e3;
    s.check_le("flanger: shortest delay - delay / 2", std::fabs(lo - delay_ms / 2.0), tol_ms, "ms");
    s.check_le("flanger: longest delay - delay x 2", std::fabs(hi - delay_ms * 2.0), tol_ms, "ms");
    std::vector<double> ups;
    for (std::size_t k = 1; k < d.size(); ++k) {
        if (d[k - 1] < delay_ms && d[k] >= delay_ms) {
            const double f = (delay_ms - d[k - 1]) / (d[k] - d[k - 1]);
            ups.push_back((k - 1 + f) * 0.010);
        }
    }
    const double period = ups.size() >= 2 ? (ups.back() - ups.front()) / (ups.size() - 1) : 0.0;
    s.check_le("flanger: period vs 1 / rate, |measured/set - 1|", std::fabs(period * rate - 1.0), 0.02, "ratio");
}

}  // namespace

// The golden configuration, shared with the host tests (sonora-dsp-measure
// --host-fixture): the bundles are rendered through Sonora's hosts with it.
measure::Config phaser_golden_config() {
    return {{"mode", 0.0f}, {"rate", 0.7f}, {"depth", 70.0f}, {"freq", 600.0f}, {"stages", 3.0f},
            {"spread", 60.0f}, {"feedback", 40.0f}, {"mix", 60.0f}};
}

int phaser(const measure::Args& args) {
    Suite s("phaser", [] { return make_phaser(); }, args);
    allpass_response(s);
    notch(s);
    sweep(s);
    feedback_bound(s);
    flanger_delay(s);
    flanger_range(s);
    const Config golden = phaser_golden_config();
    // The flanger's frequency and delay parameters are only audible in
    // their own mode, so the zipper sweeps run in both.
    const Config flanger_cfg = {{"mode", 1.0f}, {"rate", 0.7f}, {"depth", 70.0f}, {"delay", 2.0f},
                                {"spread", 60.0f}, {"feedback", 40.0f}, {"mix", 60.0f}};
    measure::check_common(s, golden,
                          {{"dry path, mix 0", {{"mix", 0.0f}}}, {"flanger", flanger_cfg}},
                          &golden);
    measure::check_zipper(s, flanger_cfg, " (flanger)");
    return s.finish();
}

}  // namespace sonora::native::suites
