// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/limiter.h"

#include <cmath>

#include "sonora/dsp/math.h"

namespace sonora::dsp {

// Window arithmetic. The detector's output at time t covers audio times
// [t - D, t - D + 1). The audio leaves delayed by L + D, so output sample n
// is x[n - L - D], touched by the detector's intervals at t = n - L - 1 and
// t = n - L. For the gain g[n'] to stay at or below those two needs for
// every n' within G (the guard) of n, with g = mean over M of (min over W of need):
//   M <= L - G + 1   and   W >= L + G + 2.

void LookaheadGain::prepare(std::uint32_t lookahead, std::uint32_t guard) {
    hold_len_ = lookahead + guard + 2;
    // Two moving averages whose spans add up to M = L - G + 1.
    const std::uint32_t span = lookahead - guard + 1;
    avg_len_ = (span + 1) / 2;
    avg2_len_ = span + 1 - avg_len_;
    dq_val_.assign(hold_len_ + 1, 1.0);
    dq_idx_.assign(hold_len_ + 1, 0);
    avg_ring_.assign(avg_len_, 1.0);
    avg2_ring_.assign(avg2_len_, 1.0);
    reset();
}

void LookaheadGain::reset() noexcept {
    dq_head_ = dq_size_ = 0;
    for (double& v : avg_ring_) v = 1.0;
    avg_pos_ = 0;
    avg_sum_ = static_cast<double>(avg_len_);
    for (double& v : avg2_ring_) v = 1.0;
    avg2_pos_ = 0;
    avg2_sum_ = static_cast<double>(avg2_len_);
    released_ = 1.0;
    n_ = 0;
}

double LookaheadGain::next(double need) noexcept {
    // Sliding minimum over the last W needs: a deque of increasing values
    // (Lemire's monotonic wedge). Bounded by W per sample, O(1) amortised.
    const std::uint32_t cap = hold_len_ + 1;
    while (dq_size_ > 0) {
        const std::uint32_t back = (dq_head_ + dq_size_ - 1) % cap;
        if (dq_val_[back] >= need) --dq_size_;
        else break;
    }
    const std::uint32_t slot = (dq_head_ + dq_size_) % cap;
    dq_val_[slot] = need;
    dq_idx_[slot] = n_;
    ++dq_size_;
    while (dq_idx_[dq_head_] + hold_len_ <= n_) {
        dq_head_ = (dq_head_ + 1) % cap;
        --dq_size_;
    }
    ++n_;
    const double held = dq_val_[dq_head_];

    // Release on the held value: rising, it approaches through a one-pole
    // and so stays at or below the held value; falling, it takes it at
    // once. Released before the averaging, so the averages also round off
    // the corner where release starts.
    if (held < released_) {
        released_ = held;
    } else {
        released_ = held - release_coeff_ * (held - released_);
    }

    // Two cascaded moving averages (a triangular window over M samples).
    // Every value either one sees is at or below the needs it must cover,
    // so the guarantee is the single average's; the second one turns the
    // ramp's corners into curves. A piecewise-linear gain has sidebands
    // falling as 1/f^2, and multiplied into 20 kHz content they reach
    // Nyquist, where they make intersample peaks the 4x detector cannot
    // see; the triangular window's fall as 1/f^3 (measured in
    // tests/test_limiter.cpp). The running sums drift by about 1e-16 per
    // step in double; the safety margin on the need absorbs it.
    avg_sum_ += released_ - avg_ring_[avg_pos_];
    avg_ring_[avg_pos_] = released_;
    avg_pos_ = (avg_pos_ + 1) % avg_len_;
    const double a1 = avg_sum_ / avg_len_;
    avg2_sum_ += a1 - avg2_ring_[avg2_pos_];
    avg2_ring_[avg2_pos_] = a1;
    avg2_pos_ = (avg2_pos_ + 1) % avg2_len_;
    const double g = avg2_sum_ / avg2_len_;
    return g > 1.0 ? 1.0 : g;
}

bool LookaheadLimiter::prepare(double sample_rate, std::uint32_t channels,
                               std::uint32_t lookahead_samples,
                               std::uint32_t detector_taps_per_phase, std::uint32_t guard) {
    if (channels == 0 || channels > kMaxChannels || lookahead_samples < guard + 1) {
        return false;
    }
    rate_ = sample_rate;
    channels_ = channels;
    lookahead_ = lookahead_samples;
    tp_.prepare(channels, detector_taps_per_phase, 100.0, true);
    paths_.assign(channels, LookaheadGain{});
    for (LookaheadGain& p : paths_) p.prepare(lookahead_, guard);
    delay_.assign(channels, std::vector<float>(lookahead_ + 1, 0.0f));
    frame_need_.assign(channels, 1.0);
    reset();
    return true;
}

void LookaheadLimiter::reset() noexcept {
    tp_.reset();
    for (LookaheadGain& p : paths_) p.reset();
    for (auto& d : delay_) {
        for (float& v : d) v = 0.0f;
    }
    delay_pos_ = 0;
    min_gain_seen_ = 1.0;
}

void LookaheadLimiter::set_release(double seconds) noexcept {
    const double c = time_constant_coeff(seconds, rate_);
    for (LookaheadGain& p : paths_) p.set_release_coeff(c);
}

void LookaheadLimiter::process_frame(float* frame) noexcept {
    double link_need = 1.0;
    double aligned[kMaxChannels];
    for (std::uint32_t c = 0; c < channels_; ++c) {
        double pts[TruePeakDetector::kFactor];
        tp_.process_points(c, frame[c], pts);
        // Phase 0 is x[n - D], the audio already aligned with the need
        // computed from these points; it is delayed by L more below.
        aligned[c] = pts[0];
        const double refined = tp_.peak_of(c, pts);
        const double peak = true_peak_ ? refined : std::fabs(pts[0]);
        // The target sits kSafetyDb under the ceiling: the refined 4x
        // detector still reads the band-limited stress set up to 0.023 dB
        // low against a 16x reference (tests/test_limiter.cpp), and the
        // margin is twice that. It also covers the running sums' rounding.
        const double target = ceiling_ * kSafetyGain;
        const double need = peak > target ? target / peak : 1.0;
        frame_need_[c] = need;
        if (need < link_need) link_need = need;
    }
    const std::uint32_t len = lookahead_ + 1;
    const std::uint32_t read = (delay_pos_ + 1) % len;  // written L samples ago
    for (std::uint32_t c = 0; c < channels_; ++c) {
        delay_[c][delay_pos_] = static_cast<float>(aligned[c]);
        const double g = paths_[c].next(linked_ ? link_need : frame_need_[c]);
        if (g < min_gain_seen_) min_gain_seen_ = g;
        frame[c] = static_cast<float>(delay_[c][read] * g);
    }
    delay_pos_ = read;
}

void LookaheadLimiter::process(float* const* io, std::uint32_t frames) noexcept {
    float frame[kMaxChannels];
    for (std::uint32_t i = 0; i < frames; ++i) {
        for (std::uint32_t c = 0; c < channels_; ++c) frame[c] = io[c][i];
        process_frame(frame);
        for (std::uint32_t c = 0; c < channels_; ++c) io[c][i] = frame[c];
    }
}

double LookaheadLimiter::take_gain_reduction_db() noexcept {
    const double db = gain_to_db(min_gain_seen_);
    min_gain_seen_ = 1.0;
    return db;
}

// ---- TruePeakGuard -----------------------------------------------------------
//
// Alignment. up() puts base sample t at 8x index 8 t + U (U = up_delay()),
// so with U = 8 q + r the eight points of step t sit at times
// t - q + (p - r) / 8. The fit is centred one point behind (it needs the
// right-hand neighbour), so the needs of step t cover [t - q - 1, t - q + 1).
// With D = q + 1, sample a is touched by steps a + D - 2 .. a + D: one more
// than the first stage's two, which costs one sample of guard. Hence
// W >= L + G + 3 for an effective guard G, which LookaheadGain gives when
// prepared with G + 1 (it then has M = L - G, under the L - G + 1 allowed).

namespace {

// 1 / cos(pi / 16): the most a peak below Nyquist rises over the nearest
// 8x point. A larger fit comes from noise in it.
constexpr double kFitCap8x = 1.0195911582083184;

// Largest |value| of the eight points, each local maximum refined by the
// sinusoid through it and its neighbours (as TruePeakDetector::peak_of,
// whose comment derives it). prev holds the last two points of the step
// before; it is updated.
double refined_peak8(const double* pts, double* prev) noexcept {
    double y[TruePeakGuard::kFactor + 2];
    y[0] = prev[0];
    y[1] = prev[1];
    double m = 0.0;
    for (std::uint32_t p = 0; p < TruePeakGuard::kFactor; ++p) {
        y[p + 2] = pts[p];
        const double a = std::fabs(pts[p]);
        if (a > m) m = a;
    }
    prev[0] = y[TruePeakGuard::kFactor];
    prev[1] = y[TruePeakGuard::kFactor + 1];
    for (std::uint32_t c = 1; c <= TruePeakGuard::kFactor; ++c) {
        const double s = y[c] < 0.0 ? -1.0 : 1.0;
        const double u0 = s * y[c - 1], u1 = s * y[c], u2 = s * y[c + 1];
        if (u1 <= 0.0 || u1 < std::fabs(u0) || u1 < std::fabs(u2)) continue;
        const double cs = (u0 + u2) / (2.0 * u1);
        double v = u1;
        if (cs > -1.0 && cs < 1.0) {
            const double sn = std::sqrt(1.0 - cs * cs);
            const double d = (u2 - u0) / (2.0 * sn);
            v = std::sqrt(u1 * u1 + d * d);
        }
        const double cap = kFitCap8x * u1;
        if (v > cap) v = cap;
        if (v > m) m = v;
    }
    return m;
}

}  // namespace

bool TruePeakGuard::prepare(double sample_rate, std::uint32_t channels, std::uint32_t max_frames,
                            std::uint32_t lookahead_samples) {
    if (channels == 0 || channels > LookaheadLimiter::kMaxChannels || max_frames == 0 ||
        lookahead_samples < kGuard + 1) {
        return false;
    }
    rate_ = sample_rate;
    channels_ = channels;
    lookahead_ = lookahead_samples;
    if (!os_.prepare(kFactor, channels, max_frames, kPassband, kStopbandDb)) return false;
    meter_.prepare(channels);
    align_ = os_.up_delay() / kFactor + 1;
    // The meter's points lag its input by its own delay (16 samples), so it
    // is fed the audio delayed by the rest of align_.
    if (align_ < meter_.delay()) return false;
    paths_.assign(channels, LookaheadGain{});
    for (LookaheadGain& p : paths_) p.prepare(lookahead_, kGuard);
    prev_.assign(2 * static_cast<std::size_t>(channels), 0.0);
    audio_.assign(channels, std::vector<float>(align_ + lookahead_ + 1, 0.0f));
    ceil_line_.assign(align_ + lookahead_ + 1, 1.0);
    need_.assign(channels, 1.0);
    reset();
    return true;
}

void TruePeakGuard::reset() noexcept {
    os_.reset();
    meter_.reset();
    for (LookaheadGain& p : paths_) p.reset();
    for (double& v : prev_) v = 0.0;
    for (auto& a : audio_) {
        for (float& v : a) v = 0.0f;
    }
    for (double& v : ceil_line_) v = 1.0;
    pos_ = 0;
    min_gain_seen_ = 1.0;
    clamped_ = 0;
}

void TruePeakGuard::set_release(double seconds) noexcept {
    const double c = time_constant_coeff(seconds, rate_);
    for (LookaheadGain& p : paths_) p.set_release_coeff(c);
}

void TruePeakGuard::process(float* const* io, const double* ceiling, std::uint32_t frames) noexcept {
    const double* hi[LookaheadLimiter::kMaxChannels];
    for (std::uint32_t c = 0; c < channels_; ++c) hi[c] = os_.up(c, io[c], frames);
    const std::uint32_t len = align_ + lookahead_ + 1;
    for (std::uint32_t i = 0; i < frames; ++i) {
        const std::uint32_t at_align = (pos_ + len - align_) % len;  // x[t - D]
        const std::uint32_t out = (pos_ + 1) % len;                  // x[t - D - L]
        const std::uint32_t to_meter = (pos_ + len - (align_ - meter_.delay())) % len;
        ceil_line_[pos_] = ceiling[i];
        // The ceiling the judged sample entered with; the clamp below uses
        // the one its output sample entered with, so the two agree.
        const double target = ceil_line_[at_align] * LookaheadLimiter::kSafetyGain;
        double link_need = 1.0;
        for (std::uint32_t c = 0; c < channels_; ++c) {
            audio_[c][pos_] = io[c][i];
            // A BS.1770 meter's 4x interpolator passes part of the images
            // of content near Nyquist, and on full-band material it reads
            // above the true reconstruction (by +0.3 dB on white noise,
            // tests/test_limiter.cpp). The ceiling must hold on that meter
            // too, so its reading of the same interval is a need as well.
            const double metered = meter_.process(c, audio_[c][to_meter]);
            double peak;
            if (true_peak_) {
                peak = refined_peak8(hi[c] + static_cast<std::size_t>(i) * kFactor, &prev_[2 * c]);
                if (metered > peak) peak = metered;
            } else {
                peak = std::fabs(static_cast<double>(audio_[c][at_align]));
            }
            if (!true_peak_) {
                // Keep the fit's history current, so switching back to true
                // peak does not fit across a stale point.
                prev_[2 * c] = hi[c][static_cast<std::size_t>(i) * kFactor + kFactor - 2];
                prev_[2 * c + 1] = hi[c][static_cast<std::size_t>(i) * kFactor + kFactor - 1];
            }
            const double need = peak > target ? std::pow(target / peak, kDepthExponent) : 1.0;
            need_[c] = need;
            if (need < link_need) link_need = need;
        }
        const double clamp = ceil_line_[out];
        for (std::uint32_t c = 0; c < channels_; ++c) {
            const double g = paths_[c].next(linked_ ? link_need : need_[c]);
            if (g < min_gain_seen_) min_gain_seen_ = g;
            double y = static_cast<double>(audio_[c][out]) * g;
            if (y > clamp) {
                y = clamp;
                ++clamped_;
            } else if (y < -clamp) {
                y = -clamp;
                ++clamped_;
            }
            io[c][i] = static_cast<float>(y);
        }
        pos_ = (pos_ + 1) % len;
    }
}

double TruePeakGuard::take_gain_reduction_db() noexcept {
    const double db = gain_to_db(min_gain_seen_);
    min_gain_seen_ = 1.0;
    return db;
}

}  // namespace sonora::dsp
