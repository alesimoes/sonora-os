// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/tape.h"

#include <cmath>
#include <complex>

namespace sonora::dsp {

OnePoleZero design_matched_one_pole(double zero_hz, double pole_hz, double sample_rate) noexcept {
    OnePoleZero f;
    f.p = std::exp(-kTwoPi * pole_hz / sample_rate);
    const double wn = kPi * sample_rate;
    const double wp = kTwoPi * pole_hz;
    double a = 1.0 / std::sqrt(1.0 + (wn / wp) * (wn / wp));
    if (zero_hz > 0.0) {
        const double wz = kTwoPi * zero_hz;
        a *= std::sqrt(1.0 + (wn / wz) * (wn / wz));
    }
    f.b0 = 0.5 * ((1.0 - f.p) + a * (1.0 + f.p));
    f.b1 = 0.5 * ((1.0 - f.p) - a * (1.0 + f.p));
    return f;
}

OnePoleInverse invert(const OnePoleZero& f) noexcept {
    OnePoleInverse g;
    g.ib0 = 1.0 / f.b0;
    g.b1 = f.b1;
    g.p = f.p;
    return g;
}

double one_pole_zero_magnitude(const OnePoleZero& f, double f_hz, double sample_rate) noexcept {
    const std::complex<double> z1 = std::polar(1.0, -kTwoPi * f_hz / sample_rate);
    return std::abs((f.b0 + f.b1 * z1) / (1.0 - f.p * z1));
}

double tape_speed_mps(TapeSpeed s) noexcept {
    constexpr double kInch = 0.0254;
    switch (s) {
        case TapeSpeed::Ips7_5: return 7.5 * kInch;
        case TapeSpeed::Ips15: return 15.0 * kInch;
        case TapeSpeed::Ips30: return 30.0 * kInch;
    }
    return 15.0 * kInch;
}

double tape_eq_tau(TapeSpeed s, TapeEq e) noexcept {
    switch (s) {
        case TapeSpeed::Ips7_5: return e == TapeEq::Nab ? 50e-6 : 70e-6;
        case TapeSpeed::Ips15: return e == TapeEq::Nab ? 50e-6 : 35e-6;
        case TapeSpeed::Ips30: return 17.5e-6;
    }
    return 50e-6;
}

double tape_head_loss(double f_hz, TapeSpeed s) noexcept {
    const double v = tape_speed_mps(s);
    const double x = kPi * f_hz * kTapeGapM / v;
    const double sinc = x < 1e-9 ? 1.0 : std::sin(x) / x;
    return std::fabs(sinc) * std::exp(-kTwoPi * f_hz * kTapeSpacingM / v);
}

double tape_head_loss_corner_hz(TapeSpeed s) noexcept {
    // Largest |dB error| of an analog one-pole at corner fc against the
    // published loss, on a log grid over the audio band.
    auto worst = [s](double fc) {
        double w = 0.0;
        for (int i = 0; i < 64; ++i) {
            const double f = 20.0 * std::pow(1000.0, i / 63.0);
            const double lp = 1.0 / std::sqrt(1.0 + (f / fc) * (f / fc));
            w = std::fmax(w, std::fabs(20.0 * std::log10(lp / tape_head_loss(f, s))));
        }
        return w;
    };
    // The error is unimodal in log fc: golden-section search.
    double lo = std::log(2000.0), hi = std::log(2.0e6);
    const double r = 0.6180339887498949;
    double x1 = hi - r * (hi - lo), x2 = lo + r * (hi - lo);
    double f1 = worst(std::exp(x1)), f2 = worst(std::exp(x2));
    for (int i = 0; i < 60; ++i) {
        if (f1 < f2) {
            hi = x2; x2 = x1; f2 = f1;
            x1 = hi - r * (hi - lo); f1 = worst(std::exp(x1));
        } else {
            lo = x1; x1 = x2; f1 = f2;
            x2 = lo + r * (hi - lo); f2 = worst(std::exp(x2));
        }
    }
    return std::exp(0.5 * (lo + hi));
}

double tape_bump_hz(TapeSpeed s) noexcept { return tape_speed_mps(s) / kTapeBumpLambdaM; }

}  // namespace sonora::dsp
