// SPDX-License-Identifier: GPL-3.0-or-later
//
// The transient shaper against its bars: exact at neutral, neutral on a
// steady tone at the extreme settings, the attack and the sustain acting on
// the onset and on the decay of a tone burst, each independent of the other
// and of the level, and the common checks.
#include <cmath>
#include <functional>
#include <string>

#include "sonora/measure/measure.h"
#include "sonora/native/transient.h"

namespace sonora::native::suites {

namespace {

using measure::Chans;
using measure::Config;
using measure::Suite;

constexpr double kFs = 48000.0;
constexpr double kOnset = 0.2;  // s

// A 1 kHz tone from kOnset whose amplitude is env(t) (t since the onset).
Chans burst(double dbfs, double seconds, const std::function<double(double)>& env) {
    Chans x = measure::silence(kFs, seconds);
    const double a = std::pow(10.0, dbfs / 20.0);
    const auto n0 = static_cast<std::size_t>(kOnset * kFs);
    for (std::size_t i = n0; i < x[0].size(); ++i) {
        const double t = static_cast<double>(i - n0) / kFs;
        const float v = static_cast<float>(a * env(t) * std::sin(2.0 * 3.14159265358979 * 1000.0 * t));
        x[0][i] = v;
        x[1][i] = v;
    }
    return x;
}

Chans run(Suite& s, const Config& c, const Chans& x) {
    auto p = s.make();
    measure::apply(*p, c);
    p->prepare(kFs, 128, 2);
    return measure::render(*p, x);
}

std::size_t at(double t_after_onset) { return static_cast<std::size_t>((kOnset + t_after_onset) * kFs); }

// Level change in dB of the window [t0, t1) after the onset.
double window_db(const Chans& y, const Chans& x, double t0, double t1, bool use_rms) {
    const std::size_t a = at(t0), b = at(t1);
    return use_rms ? measure::to_db(measure::rms(y[0], a, b) / measure::rms(x[0], a, b))
                   : measure::to_db(measure::peak(y[0], a, b) / measure::peak(x[0], a, b));
}

void neutral(Suite& s) {
    const Chans x = measure::noise(kFs, -6.0, 1.0, 5);
    const Chans y = run(s, {}, x);
    double d = 0.0;
    for (int c = 0; c < 2; ++c) {
        for (std::size_t i = 0; i < x[0].size(); ++i) d = std::fmax(d, std::fabs(static_cast<double>(y[c][i]) - x[c][i]));
    }
    s.check_le("attack 0, sustain 0, gain 0: max |out - in| on noise", d, s.bar("neutral_abs"), "abs");
}

void steady(Suite& s) {
    // A steady tone has no attack and no decay once the slow follower has
    // caught up: the extreme settings leave it alone.
    for (double f : {100.0, 1000.0}) {
        const Chans x = measure::sine(kFs, f, -20.0, 2.5);
        const Chans y = run(s, {{"attack", 15.0f}, {"sustain", 15.0f}}, x);
        const std::size_t from = x[0].size() * 3 / 4;
        s.check_le("steady " + std::to_string(static_cast<int>(f)) + " Hz at attack +15, sustain +15: |level change|",
                   std::fabs(measure::to_db(measure::rms(y[0], from) / measure::rms(x[0], from))),
                   s.bar("steady_db"), "dB");
    }
}

void attack_phase(Suite& s) {
    const Chans x = burst(-20.0, 0.6, [](double) { return 1.0; });
    const double up = window_db(run(s, {{"attack", 12.0f}}, x), x, 0.005, 0.015, false);
    const double dn = window_db(run(s, {{"attack", -12.0f}}, x), x, 0.005, 0.015, false);
    s.check("attack +12 dB: peak 5-15 ms after the onset, gain", up, s.bar("attack_min_db"), up >= s.bar("attack_min_db"), "dB");
    s.check("attack -12 dB: peak 5-15 ms after the onset, gain", dn, -s.bar("attack_min_db"), dn <= -s.bar("attack_min_db"), "dB");
    // Independent of the level: the same burst 30 dB lower.
    const Chans xq = burst(-50.0, 0.6, [](double) { return 1.0; });
    const double upq = window_db(run(s, {{"attack", 12.0f}}, xq), xq, 0.005, 0.015, false);
    s.check_le("attack +12 dB: gain at -20 vs -50 dBFS, difference", std::fabs(up - upq), s.bar("level_db"), "dB");
    // The sustain control leaves the onset alone.
    const double su = window_db(run(s, {{"sustain", 12.0f}}, x), x, 0.005, 0.015, false);
    s.check_le("sustain +12 dB: gain at the onset", std::fabs(su), s.bar("cross_db"), "dB");
}

void sustain_phase(Suite& s) {
    // Exponential decay, tau 50 ms: the level falls faster than the slow
    // follower can follow.
    const auto decay = [](double t) { return std::exp(-t / 0.050); };
    const Chans x = burst(-6.0, 0.8, decay);
    const double up = window_db(run(s, {{"sustain", 12.0f}}, x), x, 0.080, 0.120, true);
    const double dn = window_db(run(s, {{"sustain", -12.0f}}, x), x, 0.080, 0.120, true);
    s.check("sustain +12 dB: rms 80-120 ms into a 50 ms decay, gain", up, s.bar("sustain_min_db"), up >= s.bar("sustain_min_db"), "dB");
    s.check("sustain -12 dB: rms 80-120 ms into a 50 ms decay, gain", dn, -s.bar("sustain_min_db"), dn <= -s.bar("sustain_min_db"), "dB");
    const Chans xq = burst(-36.0, 0.8, decay);
    const double upq = window_db(run(s, {{"sustain", 12.0f}}, xq), xq, 0.080, 0.120, true);
    s.check_le("sustain +12 dB: gain at -6 vs -36 dBFS, difference", std::fabs(up - upq), s.bar("level_db"), "dB");
    const double ax = window_db(run(s, {{"attack", 12.0f}}, x), x, 0.080, 0.120, true);
    s.check_le("attack +12 dB: gain in the decay", std::fabs(ax), s.bar("cross_db"), "dB");
}

void speed(Suite& s) {
    // A faster detector ends the attack phase sooner: at 30 ms the boost is
    // gone at 4x and still there at 0.5x.
    const Chans x = burst(-20.0, 0.6, [](double) { return 1.0; });
    const double fast = window_db(run(s, {{"attack", 12.0f}, {"speed", 4.0f}}, x), x, 0.030, 0.040, false);
    const double slow = window_db(run(s, {{"attack", 12.0f}, {"speed", 0.5f}}, x), x, 0.030, 0.040, false);
    s.check("speed: attack boost at 30-40 ms, 4x less than 0.5x by", slow - fast, s.bar("speed_db"), slow - fast >= s.bar("speed_db"), "dB");
}

}  // namespace

measure::Config transient_golden_config() {
    return {{"attack", 8.0f}, {"sustain", -4.0f}, {"speed", 1.2f}, {"link", 80.0f}, {"gain", 1.0f}};
}

int transient(const measure::Args& args) {
    Suite s("transient", [] { return make_transient(); }, args);
    neutral(s);
    steady(s);
    attack_phase(s);
    sustain_phase(s);
    speed(s);
    measure::check_common(s, transient_golden_config(), {{"extreme", {{"attack", 15.0f}, {"sustain", -15.0f}}}});
    return s.finish();
}

}  // namespace sonora::native::suites
