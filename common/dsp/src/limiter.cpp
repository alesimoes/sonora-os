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

bool LookaheadLimiter::prepare(double sample_rate, std::uint32_t channels,
                               std::uint32_t lookahead_samples,
                               std::uint32_t detector_taps_per_phase, std::uint32_t guard) {
    if (channels == 0 || channels > kMaxChannels || lookahead_samples < guard + 1) {
        return false;
    }
    rate_ = sample_rate;
    channels_ = channels;
    lookahead_ = lookahead_samples;
    guard_ = guard;
    hold_len_ = lookahead_ + guard + 2;
    // Two moving averages whose spans add up to M = L - G + 1.
    const std::uint32_t span = lookahead_ - guard + 1;
    avg_len_ = (span + 1) / 2;
    avg2_len_ = span + 1 - avg_len_;
    tp_.prepare(channels, detector_taps_per_phase, 100.0, true);
    paths_.assign(channels, Path{});
    for (Path& p : paths_) {
        p.dq_val.assign(hold_len_ + 1, 1.0);
        p.dq_idx.assign(hold_len_ + 1, 0);
        p.avg_ring.assign(avg_len_, 1.0);
        p.avg2_ring.assign(avg2_len_, 1.0);
    }
    delay_.assign(channels, std::vector<float>(lookahead_ + 1, 0.0f));
    frame_need_.assign(channels, 1.0);
    reset();
    return true;
}

void LookaheadLimiter::reset() noexcept {
    tp_.reset();
    for (Path& p : paths_) {
        p.dq_head = p.dq_size = 0;
        for (double& v : p.avg_ring) v = 1.0;
        p.avg_pos = 0;
        p.avg_sum = static_cast<double>(avg_len_);
        for (double& v : p.avg2_ring) v = 1.0;
        p.avg2_pos = 0;
        p.avg2_sum = static_cast<double>(avg2_len_);
        p.released = 1.0;
        p.gain = 1.0;
    }
    for (auto& d : delay_) {
        for (float& v : d) v = 0.0f;
    }
    delay_pos_ = 0;
    n_ = 0;
    min_gain_seen_ = 1.0;
}

void LookaheadLimiter::set_release(double seconds) noexcept {
    release_coeff_ = time_constant_coeff(seconds, rate_);
}

double LookaheadLimiter::hold_and_smooth(Path& p, double need) noexcept {
    // Sliding minimum over the last W needs: a deque of increasing values
    // (Lemire's monotonic wedge). Bounded by W per sample, O(1) amortised.
    const std::uint32_t cap = hold_len_ + 1;
    while (p.dq_size > 0) {
        const std::uint32_t back = (p.dq_head + p.dq_size - 1) % cap;
        if (p.dq_val[back] >= need) --p.dq_size;
        else break;
    }
    const std::uint32_t slot = (p.dq_head + p.dq_size) % cap;
    p.dq_val[slot] = need;
    p.dq_idx[slot] = n_;
    ++p.dq_size;
    while (p.dq_idx[p.dq_head] + hold_len_ <= n_) {
        p.dq_head = (p.dq_head + 1) % cap;
        --p.dq_size;
    }
    const double held = p.dq_val[p.dq_head];

    // Release on the held value: rising, it approaches through a one-pole
    // and so stays at or below the held value; falling, it takes it at
    // once. Released before the averaging, so the averages also round off
    // the corner where release starts.
    if (held < p.released) {
        p.released = held;
    } else {
        p.released = held - release_coeff_ * (held - p.released);
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
    p.avg_sum += p.released - p.avg_ring[p.avg_pos];
    p.avg_ring[p.avg_pos] = p.released;
    p.avg_pos = (p.avg_pos + 1) % avg_len_;
    const double a1 = p.avg_sum / avg_len_;
    p.avg2_sum += a1 - p.avg2_ring[p.avg2_pos];
    p.avg2_ring[p.avg2_pos] = a1;
    p.avg2_pos = (p.avg2_pos + 1) % avg2_len_;
    double g = p.avg2_sum / avg2_len_;
    if (g > 1.0) g = 1.0;
    p.gain = g;
    return p.gain;
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
        const double g = hold_and_smooth(paths_[c], linked_ ? link_need : frame_need_[c]);
        if (g < min_gain_seen_) min_gain_seen_ = g;
        frame[c] = static_cast<float>(delay_[c][read] * g);
    }
    delay_pos_ = read;
    ++n_;
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

}  // namespace sonora::dsp
