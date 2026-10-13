// SPDX-License-Identifier: GPL-3.0-or-later
//
// The feedback delay network reverberator core (ADR-0011 §2.1, "FDN reverb
// core"; stage N2).
//
// Structure, from the literature:
// - J.-M. Jot and A. Chaigne, "Digital delay networks for designing
//   artificial reverberators", AES 90th Convention, 1991, preprint 3030:
//   N delay lines closed by a lossless (orthogonal) feedback matrix, with
//   an absorbent filter on each line whose gain in dB is proportional to
//   the line's length, so every mode at frequency f decays at the same
//   rate and the reverberation time T60(f) is set independently of the
//   lengths. Each line of m samples attenuates by -60 m / (fs T60(f)) dB.
// - S. J. Schlecht and E. A. P. Habets, "On lossless feedback delay
//   networks", IEEE Trans. Signal Process. 65(6), 2017: with an
//   orthogonal matrix and unity line gains the network is lossless, so
//   "decay = infinity" (freeze) neither grows nor decays. The 16 x 16
//   Hadamard matrix scaled by 1/4 is orthogonal, and 1/4 is exact in
//   binary floating point, so the scaling adds no rounding of its own.
// - M. R. Schroeder, "Natural sounding artificial reverberation", JAES
//   10(3), 1962: the series allpass diffusers in front of the network.
//
// Our choices, each measured by plugins/native/reverb's suite:
// - 16 lines, lengths spread geometrically and rounded to distinct primes,
//   so no two lines share a period and the echo pattern does not repeat
//   (Jot and Chaigne's "mutually prime" condition).
// - Absorbent filters: a gain, a low shelf and a high shelf per line, the
//   shelves designed with design_matched() (Vicanek 2016, biquad.h) at
//   Q = 1/sqrt(2), so the decay at DC, mid band and high frequencies is
//   the three-band specification below.
// - The outputs read the lines' inputs (after the matrix and the input
//   injection), with sign vectors whose Walsh spectra are flat, so every
//   line reaches each output with equal weight through the matrix. The
//   injection and output vectors share a small direct term (1/8), so the
//   first wet sample comes out at the pre-delay exactly, whatever the
//   size: a reverb on a send has no latency to report, and its onset is
//   where the tempo grid puts it (ADR-0011 §4.4).
// - A change of length crossfades the old and the new tap of every line
//   over a raised cosine instead of gliding the read point: no pitch
//   sweep, no fractional interpolation in the loop (whose high-frequency
//   loss would shorten the decay above a few kHz), and no click.
#pragma once

#include <cstdint>
#include <vector>

#include "sonora/dsp/biquad.h"
#include "sonora/rt.h"

namespace sonora::dsp {

// The reverberation time, in three bands joined by second-order shelves.
struct DecaySpec {
    double t60_s = 2.0;       // mid band
    double low_ratio = 1.0;   // T60 below low_hz, as a multiple of t60_s
    double low_hz = 250.0;
    double high_ratio = 1.0;  // T60 above high_hz, as a multiple of t60_s
    double high_hz = 5000.0;
};

// The shelves' Q: the steepest second-order shelf without overshoot (the
// cookbook's S = 1), so a line's attenuation moves monotonically between
// its band values.
constexpr double kDecayShelfQ = 0.7071067811865476;

// One line's absorbent filter: gain * low shelf * high shelf, the shelves
// by design_matched(). What Fdn::set_decay() runs, and what the tests
// check for stability (|H| < 1 at every frequency while T60 is finite).
struct LineFilter {
    double gain = 1.0;
    BiquadCoeffs low, high;
};
LineFilter design_line(const DecaySpec& spec, double sample_rate, double length) noexcept SONORA_RT;

// The attenuation in dB a line of `length` samples is designed to apply at
// f: the gain and the two analog shelf prototypes its filter is matched
// to. Divided by the length it is the decay the specification asks of the
// whole network, in dB per sample.
double line_attenuation_db(const DecaySpec& spec, double f_hz, double sample_rate,
                           double length) noexcept;
// The reverberation time the specification asks for at f, for a line of
// `length` samples (in the flat parts of the curve it does not depend on
// the length).
double design_t60(const DecaySpec& spec, double f_hz, double sample_rate, double length) noexcept;

// M. R. Schroeder's allpass, lattice form: v = x + g v[n-M], y = v[n-M] - g v.
// Its first output sample is -g x, so a chain of them never delays the
// onset of a signal while g != 0.
class SchroederAllpass {
public:
    // Allocates: call from prepare().
    void prepare(std::uint32_t delay);
    void reset() noexcept;
    double tick(double x, double g) noexcept SONORA_RT {
        const double vd = buf_[pos_];
        const double v = x + g * vd;
        buf_[pos_] = v;
        pos_ = pos_ + 1 == len_ ? 0 : pos_ + 1;
        return vd - g * v;
    }

private:
    std::vector<double> buf_;
    std::uint32_t len_ = 1;
    std::uint32_t pos_ = 0;
};

class Fdn {
public:
    static constexpr std::uint32_t kLines = 16;

    // Line lengths for a size in [0, 1] at a sample rate: geometric
    // between kMinSeconds and kMaxSeconds, scaled by
    // kMinScale + (1 - kMinScale) * size, each rounded to the nearest prime
    // not already taken. Not real-time (a sieve): call from prepare() to
    // build a table.
    static constexpr double kMinSeconds = 0.025;
    static constexpr double kMaxSeconds = 0.083;
    static constexpr double kMinScale = 0.15;
    static void lengths_for(double size, double sample_rate, std::uint32_t* out);

    // Allocates lines of up to max_length samples. stereo: two inputs and
    // two decorrelated outputs; otherwise one of each.
    void prepare(double sample_rate, std::uint32_t max_length, bool stereo);
    void reset() noexcept;

    // New line lengths: at once (jump: prepare, reset, state restore), or
    // by a crossfade of fade_samples from the current taps. A call while a
    // crossfade runs is ignored; the caller re-sends when fading() is false.
    void set_lengths(const std::uint32_t* lengths, std::uint32_t fade_samples, bool jump) noexcept SONORA_RT;
    bool fading() const noexcept { return fade_pos_ < fade_len_; }
    const std::uint32_t* lengths() const noexcept { return cur_; }

    // Designs each line's absorbent filter for spec and the line's current
    // target length, and ramps to it over ramp samples (0: at once).
    void set_decay(const DecaySpec& spec, std::uint32_t ramp) noexcept SONORA_RT;

    // One sample. freeze in [0, 1]: at 1 the lines are lossless (no
    // absorption) and take no input, so the tail holds indefinitely
    // (Schlecht and Habets); in between, both are crossfaded.
    void tick(double in_l, double in_r, double freeze, double& out_l, double& out_r) noexcept SONORA_RT;

    // The output and injection sign vectors (0 left, 1 right), for the
    // tests. Each is scaled by 1/4 in use.
    static const signed char* output_signs(int channel) noexcept;
    static const signed char* input_signs(int channel) noexcept;
    // The energy held in the lines (the sum of squares of every sample a
    // read can still reach). Not real-time; for the losslessness test.
    double state_energy() const noexcept;

private:
    struct Line {
        std::vector<float> buf;
        std::uint32_t mask = 0;
        std::uint32_t pos = 0;
        float read(std::uint32_t delay) const noexcept { return buf[(pos - delay) & mask]; }
        void push(float x) noexcept {
            pos = (pos + 1) & mask;
            buf[pos] = x;
        }
    };

    double fs_ = 48000.0;
    bool stereo_ = true;
    Line line_[kLines];
    std::uint32_t cur_[kLines] = {};
    std::uint32_t old_[kLines] = {};
    std::uint32_t fade_pos_ = 0, fade_len_ = 0;

    // Absorption: gain ramps and the two shelves of each line.
    double gain_[kLines] = {};
    double gain_inc_[kLines] = {};
    double gain_tgt_[kLines] = {};
    std::uint32_t gain_left_ = 0;
    SvfRamp low_ramp_[kLines], high_ramp_[kLines];
    SvfState low_[kLines], high_[kLines];
};

}  // namespace sonora::dsp
