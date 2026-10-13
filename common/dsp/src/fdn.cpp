// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/fdn.h"

#include <cmath>
#include <complex>

#include "sonora/dsp/math.h"

namespace sonora::dsp {

namespace {

// The output vectors: two +-1 vectors of length 16, orthogonal to each
// other, whose Walsh-Hadamard transforms are flat (every coefficient +-4:
// bent functions). Through the Hadamard matrix every line then reaches
// each output with the same weight, and the left and right outputs share
// no component, so their tails are decorrelated. Found by search and
// checked in tests/test_fdn.cpp.
constexpr signed char kOutL[Fdn::kLines] = {-1, 1, -1, 1, -1, -1, 1, 1, -1, 1, 1, -1, 1, 1, 1, 1};
constexpr signed char kOutR[Fdn::kLines] = {1, -1, 1, 1, -1, -1, -1, 1, 1, 1, -1, 1, -1, 1, 1, 1};
// The injection vectors: +-1, so each input reaches every line with the
// same energy; orthogonal to each other; and with a dot product of 2 with
// both output vectors, so the direct term (the wet sample at the
// pre-delay, before any line has returned) is 1/8 (-18 dB) of each input
// in both outputs. Zero would leave the onset to the first line's return,
// a size-dependent 4 to 30 ms after the pre-delay; 1 would be a
// full-level slap of the dry signal. Two is the smallest dot product the
// parity of these vectors allows with both outputs.
constexpr signed char kInL[Fdn::kLines] = {-1, -1, -1, 1, -1, -1, -1, 1, -1, 1, -1, -1, -1, -1, -1, -1};
constexpr signed char kInR[Fdn::kLines] = {1, 1, 1, 1, -1, -1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1};

struct LineDesign {
    double gain_db, low_db, high_db;
};

// Jot and Chaigne: a line of m samples attenuates by m times the decay per
// sample, -60 / (fs T60), in each band.
LineDesign line_design(const DecaySpec& s, double fs, double m) noexcept {
    const double mid = -60.0 / (fs * s.t60_s);
    const double low = -60.0 / (fs * s.t60_s * s.low_ratio);
    const double high = -60.0 / (fs * s.t60_s * s.high_ratio);
    return {m * mid, m * (low - mid), m * (high - mid)};
}

// In-place fast Walsh-Hadamard transform of 16 values, scaled by 1/4: the
// orthogonal Hadamard matrix of order 16 (Sylvester's construction), in 64
// additions and 16 exact multiplications.
inline void hadamard16(double* v) noexcept {
    for (std::uint32_t h = 1; h < Fdn::kLines; h <<= 1) {
        for (std::uint32_t i = 0; i < Fdn::kLines; i += h << 1) {
            for (std::uint32_t j = i; j < i + h; ++j) {
                const double a = v[j], b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
        }
    }
    for (std::uint32_t i = 0; i < Fdn::kLines; ++i) v[i] *= 0.25;
}

}  // namespace

double line_attenuation_db(const DecaySpec& spec, double f_hz, double sample_rate,
                           double length) noexcept {
    const LineDesign d = line_design(spec, sample_rate, length);
    const FilterSpec low{Shape::LowShelf, spec.low_hz, kDecayShelfQ, d.low_db};
    const FilterSpec high{Shape::HighShelf, spec.high_hz, kDecayShelfQ, d.high_db};
    return d.gain_db + 20.0 * std::log10(std::abs(analog_response(low, f_hz))) +
           20.0 * std::log10(std::abs(analog_response(high, f_hz)));
}

double design_t60(const DecaySpec& spec, double f_hz, double sample_rate, double length) noexcept {
    const double db = line_attenuation_db(spec, f_hz, sample_rate, length);
    return -60.0 * length / (sample_rate * db);
}

LineFilter design_line(const DecaySpec& spec, double sample_rate, double length) noexcept {
    const LineDesign d = line_design(spec, sample_rate, length);
    LineFilter f;
    f.gain = db_to_gain(d.gain_db);
    f.low = design_matched({Shape::LowShelf, spec.low_hz, kDecayShelfQ, d.low_db}, sample_rate);
    f.high = design_matched({Shape::HighShelf, spec.high_hz, kDecayShelfQ, d.high_db}, sample_rate);
    return f;
}

void SchroederAllpass::prepare(std::uint32_t delay) {
    len_ = delay == 0 ? 1 : delay;
    buf_.assign(len_, 0.0);
    pos_ = 0;
}

void SchroederAllpass::reset() noexcept {
    for (double& v : buf_) v = 0.0;
    pos_ = 0;
}

void Fdn::lengths_for(double size, double sample_rate, std::uint32_t* out) {
    size = clamp(size, 0.0, 1.0);
    const double scale = kMinScale + (1.0 - kMinScale) * size;
    std::uint32_t want[kLines];
    std::uint32_t top = 0;
    for (std::uint32_t i = 0; i < kLines; ++i) {
        // Interleaved so neighbouring lines (and the left and right sign
        // patterns) span the whole range: 0, 8, 1, 9, ... of the geometric
        // series.
        const std::uint32_t k = (i % 2 == 0) ? i / 2 : kLines / 2 + i / 2;
        const double sec = kMinSeconds * std::pow(kMaxSeconds / kMinSeconds, k / (kLines - 1.0));
        want[i] = static_cast<std::uint32_t>(std::lround(sec * scale * sample_rate));
        if (want[i] < 3) want[i] = 3;
        if (want[i] > top) top = want[i];
    }
    // Sieve of Eratosthenes past the longest, with room to move up.
    const std::uint32_t limit = top * 2 + 64;
    std::vector<char> composite(limit + 1, 0);
    for (std::uint32_t p = 2; p * p <= limit; ++p) {
        if (composite[p]) continue;
        for (std::uint32_t q = p * p; q <= limit; q += p) composite[q] = 1;
    }
    for (std::uint32_t i = 0; i < kLines; ++i) {
        // The nearest prime not taken yet, searching outwards (below first).
        auto usable = [&](std::uint32_t c) {
            if (c < 2 || c > limit || composite[c]) return false;
            for (std::uint32_t j = 0; j < i; ++j) {
                if (out[j] == c) return false;
            }
            return true;
        };
        for (std::uint32_t d = 0;; ++d) {
            if (d < want[i] && usable(want[i] - d)) {
                out[i] = want[i] - d;
                break;
            }
            if (usable(want[i] + d)) {
                out[i] = want[i] + d;
                break;
            }
        }
    }
}

void Fdn::prepare(double sample_rate, std::uint32_t max_length, bool stereo) {
    fs_ = sample_rate;
    stereo_ = stereo;
    std::uint32_t size = 1;
    while (size < max_length + 2) size <<= 1;
    for (Line& l : line_) {
        l.buf.assign(size, 0.0f);
        l.mask = size - 1;
        l.pos = 0;
    }
    for (std::uint32_t i = 0; i < kLines; ++i) {
        cur_[i] = old_[i] = 1;
        gain_[i] = gain_tgt_[i] = 1.0;
        gain_inc_[i] = 0.0;
    }
    fade_pos_ = fade_len_ = 0;
    gain_left_ = 0;
}

void Fdn::reset() noexcept {
    // Bounded and allocation-free, but not cheap: at 192 kHz this zeroes
    // 16 lines of 16384 floats (1 MB; with the reverb's two pre-delay
    // lines, about 1.5 MB), on the order of 100 us. It runs on the audio
    // thread only on the processor's non-finite recovery path, an error
    // path where one long block is accepted (rt-safety audit of PR #47,
    // low 2); ordinary playback never reaches it.
    for (Line& l : line_) {
        for (float& x : l.buf) x = 0.0f;
        l.pos = 0;
    }
    for (std::uint32_t i = 0; i < kLines; ++i) {
        low_[i].reset();
        high_[i].reset();
        old_[i] = cur_[i];
    }
    fade_pos_ = fade_len_;
}

void Fdn::set_lengths(const std::uint32_t* lengths, std::uint32_t fade_samples, bool jump) noexcept {
    const std::uint32_t cap = line_[0].mask - 1;
    if (jump || fade_samples == 0) {
        for (std::uint32_t i = 0; i < kLines; ++i) {
            cur_[i] = old_[i] = clamp(lengths[i], 1u, cap);
        }
        fade_pos_ = fade_len_ = 0;
        return;
    }
    if (fading()) return;
    for (std::uint32_t i = 0; i < kLines; ++i) {
        old_[i] = cur_[i];
        cur_[i] = clamp(lengths[i], 1u, cap);
    }
    fade_len_ = fade_samples;
    fade_pos_ = 0;
}

void Fdn::set_decay(const DecaySpec& spec, std::uint32_t ramp) noexcept {
    for (std::uint32_t i = 0; i < kLines; ++i) {
        const LineFilter f = design_line(spec, fs_, static_cast<double>(cur_[i]));
        const SvfCoeffs lo = to_svf(f.low);
        const SvfCoeffs hi = to_svf(f.high);
        gain_tgt_[i] = f.gain;
        if (ramp == 0) {
            low_ramp_[i].set(lo);
            high_ramp_[i].set(hi);
            gain_[i] = gain_tgt_[i];
            gain_inc_[i] = 0.0;
        } else {
            low_ramp_[i].ramp_to(lo, ramp);
            high_ramp_[i].ramp_to(hi, ramp);
            gain_inc_[i] = (gain_tgt_[i] - gain_[i]) / static_cast<double>(ramp);
        }
    }
    gain_left_ = ramp;
}

double Fdn::state_energy() const noexcept {
    double e = 0.0;
    for (std::uint32_t i = 0; i < kLines; ++i) {
        const std::uint32_t reach = cur_[i] > old_[i] ? cur_[i] : old_[i];
        for (std::uint32_t k = 0; k < reach; ++k) {
            const double x = line_[i].read(k);
            e += x * x;
        }
    }
    return e;
}

const signed char* Fdn::output_signs(int channel) noexcept { return channel == 0 ? kOutL : kOutR; }
const signed char* Fdn::input_signs(int channel) noexcept { return channel == 0 ? kInL : kInR; }

void Fdn::tick(double in_l, double in_r, double freeze, double& out_l, double& out_r) noexcept {
    double v[kLines];
    // Read every line before any is written: the network's state is the
    // previous sample's. read(m - 1) is a delay of m, the push coming after.
    if (fading()) {
        const double w = 0.5 - 0.5 * std::cos(kPi * (static_cast<double>(fade_pos_) / fade_len_));
        for (std::uint32_t i = 0; i < kLines; ++i) {
            v[i] = (1.0 - w) * line_[i].read(old_[i] - 1) + w * line_[i].read(cur_[i] - 1);
        }
        ++fade_pos_;
    } else {
        for (std::uint32_t i = 0; i < kLines; ++i) v[i] = line_[i].read(cur_[i] - 1);
    }

    if (gain_left_ != 0) {
        --gain_left_;
        for (std::uint32_t i = 0; i < kLines; ++i) {
            gain_[i] = gain_left_ == 0 ? gain_tgt_[i] : gain_[i] + gain_inc_[i];
        }
    }
    const double keep = 1.0 - freeze;
    for (std::uint32_t i = 0; i < kLines; ++i) {
        // The shelves run whether or not they are heard, so leaving a
        // freeze resumes from a settled filter rather than a stale one.
        const double a = gain_[i] * high_[i].tick(high_ramp_[i].next(), low_[i].tick(low_ramp_[i].next(), v[i]));
        v[i] = freeze == 0.0 ? a : (freeze == 1.0 ? v[i] : keep * a + freeze * v[i]);
    }

    hadamard16(v);

    // Injection, then the outputs from the lines' inputs, both scaled by
    // 1/4 (unit-norm vectors), so the direct term is 2/16 of each input.
    double yl = 0.0, yr = 0.0;
    if (stereo_) {
        const double xl = 0.25 * keep * in_l, xr = 0.25 * keep * in_r;
        for (std::uint32_t i = 0; i < kLines; ++i) {
            const double s = v[i] + kInL[i] * xl + kInR[i] * xr;
            line_[i].push(static_cast<float>(s));
            yl += kOutL[i] * s;
            yr += kOutR[i] * s;
        }
        out_l = 0.25 * yl;
        out_r = 0.25 * yr;
    } else {
        const double x = 0.25 * keep * in_l;
        for (std::uint32_t i = 0; i < kLines; ++i) {
            const double s = v[i] + kInL[i] * x;
            line_[i].push(static_cast<float>(s));
            yl += kOutL[i] * s;
        }
        out_l = out_r = 0.25 * yl;
    }
}

}  // namespace sonora::dsp
