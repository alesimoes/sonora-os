// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tuner's three blocks, header-only so the suite can drive each on its
// own (tests/suite.cpp): a scale and note chooser, a YIN pitch detector and
// a pitch-synchronous overlap-add shifter. All are standard C++17, size
// everything in prepare(), and run on an audio thread (no allocation, no
// locks, no throws).
//
// - YinDetector: A. de Cheveigne and H. Kawahara, "YIN, a fundamental
//   frequency estimator for speech and music", JASA 111(4), 2002: the
//   difference function, its cumulative-mean normalisation, the absolute
//   threshold and parabolic interpolation (steps 2 to 5 of the paper; the
//   best-local-estimate step 6 is replaced by the caller's median of three).
//   The search runs on a signal decimated to about 12 kHz, which is what
//   keeps it inside the CPU class, and its result is then refined at the
//   full rate over the few lags around it, so the period is exact to a
//   small fraction of a sample at any rate.
// - PsolaShifter: pitch-synchronous overlap-add (E. Moulines and F.
//   Charpentier, "Pitch-synchronous waveform processing techniques for
//   text-to-speech synthesis using diphones", Speech Communication 9, 1990).
//   Each grain is one pitch cycle of the voice itself, centred on its pulse,
//   placed one new period (1 / ratio of the old) after the last: the
//   harmonics move, the spectral envelope (the formants) stays where the
//   voice put it, because nothing is resampled. Where two grains meet,
//   half-way between pulses where the cycle is quiet, they cross-fade over
//   half a period (a raised cosine; the windows sum to one), instead of
//   Moulines and Charpentier's two-period Hann windows, which mix the whole
//   of two cycles of different phase and comb-filter the voice by the
//   amount of the shift.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "sonora/dsp/biquad.h"
#include "sonora/rt.h"

namespace sonora::native::tuner {

// ---- scales ------------------------------------------------------------------------

// Bit d set: the degree d semitones above the tonic is in the scale.
constexpr unsigned degree_bit(int d) { return 1u << d; }
constexpr unsigned kScaleMasks[] = {
    0xFFFu,  // chromatic
    degree_bit(0) | degree_bit(2) | degree_bit(4) | degree_bit(5) | degree_bit(7) | degree_bit(9) | degree_bit(11),  // major
    degree_bit(0) | degree_bit(2) | degree_bit(3) | degree_bit(5) | degree_bit(7) | degree_bit(8) | degree_bit(10),  // natural minor
    degree_bit(0) | degree_bit(2) | degree_bit(3) | degree_bit(5) | degree_bit(7) | degree_bit(8) | degree_bit(11),  // harmonic minor
    degree_bit(0) | degree_bit(2) | degree_bit(4) | degree_bit(7) | degree_bit(9),                                  // major pentatonic
    degree_bit(0) | degree_bit(3) | degree_bit(5) | degree_bit(7) | degree_bit(10),                                 // minor pentatonic
};
constexpr std::uint32_t kScaleCount = sizeof kScaleMasks / sizeof kScaleMasks[0];

inline bool in_scale(int note, int tonic, unsigned mask) noexcept SONORA_RT {
    const int pc = ((note - tonic) % 12 + 12) % 12;
    return ((mask >> pc) & 1u) != 0u;
}

// The scale note nearest to `midi` (a fractional MIDI note number, A4 = 69).
// `prev`, the note chosen last time (-1 for none), is kept while it is
// within `hysteresis` semitones of being as near as the best: a voice
// hovering at the middle of a gap does not flip between two notes.
inline int nearest_note(double midi, int tonic, unsigned mask, int prev, double hysteresis) noexcept SONORA_RT {
    const int base = static_cast<int>(std::floor(midi));
    int best = base;
    double best_d = 1e9;
    for (int n = base - 6; n <= base + 7; ++n) {
        if (!in_scale(n, tonic, mask)) continue;
        const double d = std::fabs(midi - n);
        if (d < best_d) {
            best_d = d;
            best = n;
        }
    }
    if (prev >= 0 && in_scale(prev, tonic, mask) && std::fabs(midi - prev) <= best_d + hysteresis) return prev;
    return best;
}

// ---- the detector ------------------------------------------------------------------

class YinDetector {
public:
    struct Result {
        bool voiced = false;
        double period = 0.0;        // samples at the input rate
        double aperiodicity = 1.0;  // YIN's d', 0 for a perfect period
    };

    // Allocates. The lowest and highest pitch it will report, in Hz.
    bool prepare(double fs, double fmin, double fmax, double threshold = 0.15) {
        if (!(fs >= 8000.0) || !(fmin > 20.0) || !(fmax > fmin) || fmax > 0.25 * fs) return false;
        fs_ = fs;
        thr_ = threshold;
        dec_ = std::max(1, static_cast<int>(std::lround(fs / 12000.0)));
        const double fsd = fs / dec_;
        tmax_ = static_cast<int>(std::ceil(fsd / fmin)) + 2;
        tmin_ = std::max(2, static_cast<int>(std::floor(fsd / fmax)));
        w_ = tmax_ + 64;
        n_ = w_ + tmax_ + 1;
        std::size_t dsize = 1;
        while (dsize < static_cast<std::size_t>(n_)) dsize <<= 1;
        dring_.assign(dsize, 0.0f);
        dmask_ = dsize - 1;
        win_.assign(static_cast<std::size_t>(n_), 0.0f);
        cm_.assign(static_cast<std::size_t>(tmax_) + 2, 0.0f);
        tmax_f_ = static_cast<int>(std::ceil(fs / fmin)) + 2;
        tmin_f_ = std::max(2, static_cast<int>(std::floor(fs / fmax)));
        wf_ = static_cast<int>(std::ceil(0.012 * fs));
        nf_ = wf_ + tmax_f_ + 3;
        std::size_t hsize = 1;
        while (hsize < static_cast<std::size_t>(nf_)) hsize <<= 1;
        hist_.assign(hsize, 0.0f);
        hmask_ = hsize - 1;
        fwin_.assign(static_cast<std::size_t>(nf_), 0.0f);
        df_.assign(static_cast<std::size_t>(2 * dec_ + 4), 0.0f);
        // Fourth-order Butterworth low-pass ahead of the decimation: the
        // search needs the fundamental and the first harmonics, not the top.
        const double fc = std::min(3000.0, 0.4 * fsd);
        lp_[0] = dsp::design_matched({dsp::Shape::LowPass, fc, 0.5411961001461970, 0.0}, fs);
        lp_[1] = dsp::design_matched({dsp::Shape::LowPass, fc, 1.3065629648763766, 0.0}, fs);
        reset();
        return true;
    }

    void reset() noexcept {
        std::fill(dring_.begin(), dring_.end(), 0.0f);
        std::fill(hist_.begin(), hist_.end(), 0.0f);
        st_[0].reset();
        st_[1].reset();
        dpos_ = hpos_ = 0;
        phase_ = hop_count_ = 0;
        ndec_ = nfull_ = 0;
        res_ = Result{};
    }

    // Seconds between two results.
    double hop_seconds() const noexcept { return static_cast<double>(kHopDec) * dec_ / fs_; }

    // One input sample; true when a new result is ready (every hop).
    bool push(float x) noexcept SONORA_RT {
        const double y = st_[1].tick(lp_[1], st_[0].tick(lp_[0], x));
        // The refinement reads the low-passed signal too: a harmonic-rich
        // signal's difference function has a cusp at its minimum, where a
        // parabola is biased by up to a quarter of a sample (about 4 cents at
        // 450 Hz); below 3 kHz the minimum is round.
        hist_[hpos_] = static_cast<float>(y);
        hpos_ = (hpos_ + 1) & hmask_;
        if (nfull_ < nf_) ++nfull_;
        if (++phase_ < dec_) return false;
        phase_ = 0;
        dring_[dpos_] = static_cast<float>(y);
        dpos_ = (dpos_ + 1) & dmask_;
        if (ndec_ < n_) ++ndec_;
        if (++hop_count_ < kHopDec) return false;
        hop_count_ = 0;
        res_ = Result{};
        if (ndec_ >= n_ && nfull_ >= nf_) analyse();
        return true;
    }

    const Result& result() const noexcept { return res_; }

private:
    static constexpr int kHopDec = 96;
    // Mean power below which a window is silence (-70 dBFS RMS): no pitch
    // is read from the noise floor.
    static constexpr float kGatePower = 1e-7f;

    void analyse() noexcept SONORA_RT {
        const std::size_t n = static_cast<std::size_t>(n_);
        for (std::size_t i = 0; i < n; ++i) win_[i] = dring_[(dpos_ + dring_.size() - n + i) & dmask_];
        float power = 0.0f;
        for (int j = 0; j < w_; ++j) power += win_[static_cast<std::size_t>(j)] * win_[static_cast<std::size_t>(j)];  // 235 terms: not the hot loop
        if (power < kGatePower * static_cast<float>(w_)) return;

        // Difference function and its cumulative-mean normalisation (YIN
        // steps 2 and 3).
        float run = 0.0f;
        cm_[0] = 1.0f;
        for (int tau = 1; tau <= tmax_; ++tau) {
            const float s = sq_diff(win_.data(), win_.data() + tau, w_);
            run += s;
            cm_[static_cast<std::size_t>(tau)] = run > 0.0f ? s * static_cast<float>(tau) / run : 1.0f;
        }
        // Absolute threshold (step 4): the first dip below it, followed to
        // its bottom.
        int tau = -1;
        float lowest = 1.0f;
        for (int t = tmin_; t < tmax_; ++t) {
            lowest = std::min(lowest, cm_[static_cast<std::size_t>(t)]);
            if (cm_[static_cast<std::size_t>(t)] < static_cast<float>(thr_)) {
                tau = t;
                while (tau + 1 < tmax_ && cm_[static_cast<std::size_t>(tau + 1)] < cm_[static_cast<std::size_t>(tau)]) ++tau;
                break;
            }
        }
        res_.aperiodicity = lowest;
        if (tau < 0) return;
        // Parabolic interpolation (step 5), then the full-rate refinement.
        const double coarse = (tau + parabola(cm_[static_cast<std::size_t>(tau - 1)], cm_[static_cast<std::size_t>(tau)],
                                              cm_[static_cast<std::size_t>(tau + 1)])) * dec_;
        res_.aperiodicity = cm_[static_cast<std::size_t>(tau)];
        res_.period = refine(coarse);
        res_.voiced = true;
    }

    // Sum of squared differences, four independent sums: without -ffast-math
    // the compiler may not reassociate one, and a single chain of adds is
    // latency-bound at a quarter of the throughput.
    static float sq_diff(const float* a, const float* b, int n) noexcept SONORA_RT {
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
        int j = 0;
        for (; j + 4 <= n; j += 4) {
            const float e0 = a[j] - b[j], e1 = a[j + 1] - b[j + 1], e2 = a[j + 2] - b[j + 2], e3 = a[j + 3] - b[j + 3];
            s0 += e0 * e0;
            s1 += e1 * e1;
            s2 += e2 * e2;
            s3 += e3 * e3;
        }
        for (; j < n; ++j) {
            const float e = a[j] - b[j];
            s0 += e * e;
        }
        return (s0 + s1) + (s2 + s3);
    }

    static double parabola(double l, double c, double r) noexcept {
        const double den = l - 2.0 * c + r;
        if (!(den > 1e-12)) return 0.0;
        return std::max(-1.0, std::min(1.0, 0.5 * (l - r) / den));
    }

    // The difference function at the input rate over the lags around the
    // coarse estimate; its lowest, interpolated.
    double refine(double coarse) noexcept SONORA_RT {
        const std::size_t n = static_cast<std::size_t>(nf_);
        for (std::size_t i = 0; i < n; ++i) fwin_[i] = hist_[(hpos_ + hist_.size() - n + i) & hmask_];
        const float* p = fwin_.data() + (nf_ - wf_);
        const int lo = std::max(tmin_f_, static_cast<int>(std::floor(coarse)) - dec_);
        const int hi = std::min(tmax_f_, static_cast<int>(std::ceil(coarse)) + dec_);
        if (hi <= lo) return std::max<double>(tmin_f_, std::min<double>(tmax_f_, coarse));
        // Lags lo - 1 .. hi + 1 into df_[0 ..].
        const int count = hi - lo + 3;
        if (static_cast<std::size_t>(count) > df_.size()) return coarse;
        for (int k = 0; k < count; ++k) {
            const int lag = lo - 1 + k;
            df_[static_cast<std::size_t>(k)] = sq_diff(p, p - lag, wf_);
        }
        int best = 1;
        for (int k = 2; k <= count - 2; ++k)
            if (df_[static_cast<std::size_t>(k)] < df_[static_cast<std::size_t>(best)]) best = k;
        const double frac = parabola(df_[static_cast<std::size_t>(best - 1)], df_[static_cast<std::size_t>(best)],
                                     df_[static_cast<std::size_t>(best + 1)]);
        const double period = lo - 1 + best + frac;
        return std::max<double>(tmin_f_, std::min<double>(tmax_f_, period));
    }

    double fs_ = 48000.0, thr_ = 0.15;
    int dec_ = 4, tmax_ = 0, tmin_ = 0, w_ = 0, n_ = 0, tmax_f_ = 0, tmin_f_ = 0, wf_ = 0, nf_ = 0;
    dsp::BiquadCoeffs lp_[2];
    dsp::BiquadState st_[2];
    std::vector<float> dring_, win_, cm_, hist_, fwin_, df_;
    std::size_t dmask_ = 0, hmask_ = 0, dpos_ = 0, hpos_ = 0;
    int phase_ = 0, hop_count_ = 0, ndec_ = 0, nfull_ = 0;
    Result res_;
};

// ---- the shifter -------------------------------------------------------------------

class PsolaShifter {
public:
    // Allocates. fmin: the lowest pitch it will be given a period for.
    bool prepare(double fs, double fmin_hz, std::uint32_t channels) {
        if (!(fs > 0.0) || !(fmin_hz > 0.0) || channels == 0 || channels > kMaxChannels) return false;
        channels_ = channels;
        tmax_ = std::ceil(fs / fmin_hz) + 2.0;
        tmin_ = 8.0;
        // The widest grain is 0.75 of the longest new period (the longest
        // period over the smallest ratio), each side of its mark. A grain
        // is written when its start is still ahead of the output, and read
        // up to its end; its centre trails the mark by the latency, so
        // twice the reach and a little make every grain complete when it is
        // written.
        margin_ = std::ceil(0.75 * tmax_ / kMinRatio) + 1.0;
        latency_ = static_cast<std::uint32_t>(2.0 * margin_) + 4u;
        std::size_t size = 1;
        while (size < static_cast<std::size_t>(6.0 * tmax_) + 64u) size <<= 1;
        mask_ = size - 1;
        for (std::uint32_t c = 0; c < kMaxChannels; ++c) {
            hist_[c].assign(c < channels ? size : 0, 0.0f);
            num_[c].assign(c < channels ? size : 0, 0.0f);
        }
        den_.assign(size, 0.0f);
        reset();
        return true;
    }

    void reset() noexcept {
        for (std::uint32_t c = 0; c < channels_; ++c) {
            std::fill(hist_[c].begin(), hist_[c].end(), 0.0f);
            std::fill(num_[c].begin(), num_[c].end(), 0.0f);
        }
        std::fill(den_.begin(), den_.end(), 0.0f);
        m_ = 0;
        s_next_ = margin_;
        a_prev_ = s_next_ - latency_;
        step_ = tmax_;
    }

    std::uint32_t latency() const noexcept { return latency_; }
    double min_period() const noexcept { return tmin_; }
    double max_period() const noexcept { return tmax_; }

    // One sample per channel. `period`: the voice's period in samples.
    // `ratio`: output pitch over input pitch. `aligned`: the input is
    // periodic, so grains are taken a whole number of periods apart; when
    // it is not they follow the synthesis marks exactly and the output is
    // the input delayed by latency() whatever the ratio.
    void tick(const float* in, float* out, double period, double ratio, bool aligned) noexcept SONORA_RT {
        for (std::uint32_t c = 0; c < channels_; ++c) hist_[c][static_cast<std::size_t>(m_) & mask_] = in[c];
        const double t = std::max(tmin_, std::min(tmax_, period));
        const double r = std::max(kMinRatio, std::min(kMaxRatio, ratio));
        while (s_next_ - margin_ <= static_cast<double>(m_)) grain(t, r, aligned);
        const std::size_t idx = static_cast<std::size_t>(m_) & mask_;
        // The windows sum to one where the spacing is steady; the division
        // takes up the difference where it is not, and its floor only
        // matters in the first moments after a reset.
        const float inv = 1.0f / std::max(den_[idx], 0.05f);
        for (std::uint32_t c = 0; c < channels_; ++c) {
            out[c] = num_[c][idx] * inv;
            num_[c][idx] = 0.0f;
        }
        den_[idx] = 0.0f;
        ++m_;
    }

private:
    static constexpr std::uint32_t kMaxChannels = 2;
    // The range of the ratio: +-600 cents, the most the tuner ever asks.
    static constexpr double kMinRatio = 0.70710678118654752;
    static constexpr double kMaxRatio = 1.41421356237309505;
    // The fraction of the new period over which two grains cross-fade.
    static constexpr double kCross = 0.5;
    // How far a grain's centre moves toward the synthesis mark per grain
    // (samples): the delay of a corrected voice relaxes back to the latency
    // when the correction ends, slowly enough not to smear the harmonics.
    static constexpr double kPull = 0.25;

    float read(const std::vector<float>& h, double pos) const noexcept SONORA_RT {
        const double fl = std::floor(pos);
        const auto i = static_cast<std::int64_t>(fl);
        const float f = static_cast<float>(pos - fl);
        const float p0 = h[static_cast<std::size_t>(i - 1) & mask_];
        const float p1 = h[static_cast<std::size_t>(i) & mask_];
        const float p2 = h[static_cast<std::size_t>(i + 1) & mask_];
        const float p3 = h[static_cast<std::size_t>(i + 2) & mask_];
        // Catmull-Rom: a four-point Hermite through the samples.
        return p1 + 0.5f * f * (p2 - p0 + f * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + f * (3.0f * (p1 - p2) + p3 - p0)));
    }

    // Pitch marks. Grains taken a whole number of periods apart are only
    // coherent when each is centred on the same point of the cycle; a grid
    // laid on arbitrary points gives overlapping tails of different phases,
    // which smear the harmonics. The centre therefore climbs to the highest
    // sample (a parabola through it for the fraction) within a tenth of a
    // period of where the grid puts it: one cycle's main pulse, tracked from
    // grain to grain. Never past `limit`, the newest point a whole grain can
    // be read from.
    double snap_to_peak(double a, double t, double limit) const noexcept SONORA_RT {
        const double reach = std::max(2.0, 0.12 * t);
        const auto lo = static_cast<std::int64_t>(std::ceil(a - reach));
        const auto hi = std::min(static_cast<std::int64_t>(std::floor(a + reach)), static_cast<std::int64_t>(std::floor(limit)) - 1);
        if (hi < lo) return a;
        std::int64_t best = lo;
        float best_v = level(lo);
        for (std::int64_t p = lo + 1; p <= hi; ++p) {
            const float v = level(p);
            if (v > best_v) {
                best_v = v;
                best = p;
            }
        }
        const float l = level(best - 1), r = level(best + 1);
        const float den = l - 2.0f * best_v + r;
        double frac = 0.0;
        if (den < -1e-12f) frac = std::max(-1.0, std::min(1.0, 0.5 * static_cast<double>(l - r) / static_cast<double>(den)));
        return std::min(static_cast<double>(best) + frac, limit);
    }

    float level(std::int64_t p) const noexcept SONORA_RT {
        float v = 0.0f;
        for (std::uint32_t c = 0; c < channels_; ++c) v += hist_[c][static_cast<std::size_t>(p) & mask_];
        return v;
    }

    void grain(double t, double r, bool aligned) noexcept SONORA_RT {
        const double s = s_next_;
        const double u = s - static_cast<double>(latency_);
        double a = u;
        if (aligned) {
            // The grain centre goes to the latest point a whole number of
            // (the previous step's) periods after the last one that is not
            // past the target. The epsilon keeps an exactly-unity step from
            // rounding to one period short.
            double j = std::floor((u - a_prev_) / step_ + 1e-6);
            if (j < 0.0) j = 0.0;
            a = std::min(a_prev_ + j * step_, u);
            if (r == 1.0) {
                // Nothing to correct: the centre relaxes back onto the
                // synthesis mark, where the output is the input delayed by
                // exactly the latency.
                const double off = u - a;
                if (off > 0.0) a += std::min(off, kPull);
            } else {
                a = snap_to_peak(a, t, u);
            }
        }
        const double h = t / r;
        const double flat = 0.5 * (1.0 - kCross) * h;
        const double half = 0.5 * (1.0 + kCross) * h;
        const double taper = std::acos(-1.0) / (kCross * h);
        const auto i0 = static_cast<std::int64_t>(std::ceil(s - half));
        const auto i1 = static_cast<std::int64_t>(std::floor(s + half));
        for (std::int64_t i = i0; i <= i1; ++i) {
            const double x = static_cast<double>(i) - s;
            const double ax = std::fabs(x);
            const float w = ax <= flat ? 1.0f : static_cast<float>(0.5 + 0.5 * std::cos(taper * (ax - flat)));
            const double pos = a + x;
            const std::size_t idx = static_cast<std::size_t>(i) & mask_;
            for (std::uint32_t c = 0; c < channels_; ++c) num_[c][idx] += w * read(hist_[c], pos);
            den_[idx] += w;
        }
        a_prev_ = a;
        step_ = t;
        s_next_ = s + h;
    }

    std::uint32_t channels_ = 0, latency_ = 0;
    double tmax_ = 0.0, tmin_ = 8.0, margin_ = 0.0;
    std::size_t mask_ = 0;
    std::vector<float> hist_[kMaxChannels], num_[kMaxChannels], den_;
    std::int64_t m_ = 0;
    double s_next_ = 0.0, a_prev_ = 0.0, step_ = 0.0;
};

}  // namespace sonora::native::tuner
