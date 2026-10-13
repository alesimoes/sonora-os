// SPDX-License-Identifier: GPL-3.0-or-later
//
// Second-order sections: their analog targets, two digital designs, and two
// structures to run them.
//
// Designs
// - design_bilinear(): the RBJ Audio EQ Cookbook (R. Bristow-Johnson, W3C
//   Audio WG Note, https://www.w3.org/TR/audio-eq-cookbook/), which is the
//   bilinear transform prewarped at f0. It cramps near Nyquist: a bell at
//   12 kHz and 48 kHz comes out visibly narrower than its analog prototype.
//   Kept as a reference and for standards that define their filters this way
//   (BS.1770 K-weighting).
// - design_matched(): M. Vicanek, "Matched Second Order Digital Filters",
//   14 Feb 2016, https://www.vicanek.de/articles/BiquadFits.pdf. Poles by
//   the matched-z transform (impulse invariance of the pole pair); the
//   numerator is then chosen so the digital magnitude equals the analog one
//   exactly at DC, at Nyquist and at the section's own frequency. Closed
//   form, cheap enough for per-block retuning, and no cramping. This is what
//   the EQ and every filter inside a processor use (ADR-0011 §3.2).
//
// Structures
// - BiquadState: transposed direct form II, double state. For fixed
//   coefficients (meters, analysis).
// - SvfState with SvfCoeffs: the trapezoidal state-variable filter of
//   A. Simper (Cytomic, "Linear trap SVF", 2013) and V. Zavalishin, *The Art
//   of VA Filter Design*, rev. 2.1.2 (2020). Any stable digital biquad maps
//   onto it exactly (to_svf below), so it runs the matched design. Its
//   coefficients can be interpolated sample by sample while the filter
//   runs: every intermediate (g > 0, k > 0) is a stable filter, and the
//   state keeps its meaning across a change. That is what makes automated
//   EQ sweeps free of zipper noise and blow-ups, which a direct form with
//   moving coefficients does not guarantee.
#pragma once

#include <complex>
#include <cstdint>

#include "sonora/rt.h"

namespace sonora::dsp {

enum class Shape : std::uint8_t {
    Peak,       // bell; q is the cookbook Q, gain_db the boost or cut
    LowShelf,   // q as in the cookbook (0.7071 = steepest without overshoot)
    HighShelf,
    LowPass,    // second order; q = 0.7071 is Butterworth
    HighPass,
    BandPass,   // constant 0 dB peak gain
    Notch,
    LowPass1,   // first order; q and gain ignored
    HighPass1,
};

struct FilterSpec {
    Shape shape = Shape::Peak;
    double freq_hz = 1000.0;
    double q = 0.7071067811865476;
    double gain_db = 0.0;
};

// A normalised analog section: s is divided by 2*pi*f0, so the section is
// (n2 s^2 + n1 s + n0) / (d2 s^2 + d1 s + d0).
struct AnalogSection {
    double n2 = 0, n1 = 0, n0 = 1;
    double d2 = 0, d1 = 0, d0 = 1;
};

AnalogSection analog_prototype(const FilterSpec& spec) noexcept;
// H(j 2 pi f) of the analog prototype: the design target the measurement
// harness compares against.
std::complex<double> analog_response(const FilterSpec& spec, double f_hz) noexcept;

// H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2).
struct BiquadCoeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;
};

std::complex<double> response(const BiquadCoeffs& c, double f_hz,
                              double sample_rate) noexcept;

// Bilinear transform of any normalised analog section, prewarped at f0.
BiquadCoeffs bilinear(const AnalogSection& s, double f0_hz,
                      double sample_rate) noexcept;
BiquadCoeffs design_bilinear(const FilterSpec& spec, double sample_rate) noexcept;
BiquadCoeffs design_matched(const FilterSpec& spec, double sample_rate) noexcept SONORA_RT;

// Highest frequency a design accepts, as a fraction of the sample rate.
// Matching at f0 needs f0 strictly below Nyquist.
constexpr double kMaxFreqRatio = 0.49;

struct BiquadState {
    double z1 = 0.0, z2 = 0.0;

    double tick(const BiquadCoeffs& c, double x) noexcept SONORA_RT {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0.0; }
};

// Trapezoidal SVF coefficients: g and k place the poles, m0..m2 mix the
// input, band-pass and low-pass outputs into the numerator.
struct SvfCoeffs {
    double g = 0.0, k = 2.0;
    double m0 = 1.0, m1 = 0.0, m2 = 0.0;
};

// Exact: the SVF with these coefficients has transfer function c.
SvfCoeffs to_svf(const BiquadCoeffs& c) noexcept SONORA_RT;

struct SvfState {
    double ic1 = 0.0, ic2 = 0.0;

    // Derived per coefficient set; cheap enough to recompute per sample
    // while a ramp runs.
    struct Derived {
        double a1 = 1.0, a2 = 0.0, a3 = 0.0;
        double m0 = 1.0, m1 = 0.0, m2 = 0.0;
    };
    static Derived derive(const SvfCoeffs& c) noexcept SONORA_RT {
        Derived d;
        d.a1 = 1.0 / (1.0 + c.g * (c.g + c.k));
        d.a2 = c.g * d.a1;
        d.a3 = c.g * d.a2;
        d.m0 = c.m0;
        d.m1 = c.m1;
        d.m2 = c.m2;
        return d;
    }

    double tick(const Derived& d, double v0) noexcept SONORA_RT {
        const double v3 = v0 - ic2;
        const double v1 = d.a1 * ic1 + d.a2 * v3;
        const double v2 = ic2 + d.a2 * ic1 + d.a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        return d.m0 * v0 + d.m1 * v1 + d.m2 * v2;
    }
    void reset() noexcept { ic1 = ic2 = 0.0; }
};

// Coefficients shared by every channel of one filter, moved linearly from
// their current value to a new target over a number of samples.
class SvfRamp {
public:
    void set(const SvfCoeffs& c) noexcept SONORA_RT {
        cur_ = tgt_ = c;
        left_ = 0;
        derived_ = SvfState::derive(c);
    }
    void ramp_to(const SvfCoeffs& c, std::uint32_t samples) noexcept SONORA_RT {
        if (samples == 0) {
            set(c);
            return;
        }
        tgt_ = c;
        left_ = samples;
        const double n = static_cast<double>(samples);
        inc_.g = (c.g - cur_.g) / n;
        inc_.k = (c.k - cur_.k) / n;
        inc_.m0 = (c.m0 - cur_.m0) / n;
        inc_.m1 = (c.m1 - cur_.m1) / n;
        inc_.m2 = (c.m2 - cur_.m2) / n;
    }
    // Coefficients for the next sample.
    const SvfState::Derived& next() noexcept SONORA_RT {
        if (left_ != 0) {
            --left_;
            if (left_ == 0) {
                cur_ = tgt_;
            } else {
                cur_.g += inc_.g;
                cur_.k += inc_.k;
                cur_.m0 += inc_.m0;
                cur_.m1 += inc_.m1;
                cur_.m2 += inc_.m2;
            }
            derived_ = SvfState::derive(cur_);
        }
        return derived_;
    }
    bool moving() const noexcept { return left_ != 0; }
    const SvfCoeffs& current() const noexcept { return cur_; }

private:
    SvfCoeffs cur_, tgt_, inc_{0.0, 0.0, 0.0, 0.0, 0.0};
    SvfState::Derived derived_;
    std::uint32_t left_ = 0;
};

// Butterworth sections for an n-th order high- or low-pass (n = 1..8):
// floor(n/2) second-order sections with Q_k = 1 / (2 cos((2k-1) pi / 2n)),
// plus one first-order section when n is odd. Returns the section count.
int butterworth_sections(int order, bool high_pass, double freq_hz,
                         FilterSpec* out /* at least 4 + 1 */) noexcept SONORA_RT;

}  // namespace sonora::dsp
