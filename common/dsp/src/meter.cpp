// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/meter.h"

#include <cmath>

#include "sonora/dsp/denormals.h"
#include "sonora/dsp/math.h"

namespace sonora::dsp {

namespace {

// The ballistic is let go below this (-200 dBFS), where it could only walk
// into the subnormal range on a target without FTZ.
constexpr double kEnvFloor = 1e-10;

float to_db(double gain) noexcept {
    return static_cast<float>(gain_to_db(gain, TrackMeter::kFloorDb));
}

}  // namespace

bool TrackMeter::prepare(double sample_rate, std::uint32_t max_frames, std::uint32_t channels,
                         const Options& options) {
    if (!(sample_rate > 0.0) || max_frames == 0 || channels == 0) return false;
    if (!options.weights.empty() && options.weights.size() != channels) return false;
    if (!(options.release_db_per_s > 0.0)) return false;
    channels_ = channels;
    max_frames_ = max_frames;
    loudness_on_ = options.loudness;
    tp_on_ = options.true_peak;
    fall_ = db_to_gain(-options.release_db_per_s / sample_rate);
    ch_.reset(new Channel[channels]);
    if (tp_on_) {
        tp_.prepare(channels);
        tp_.prepare_blocks(max_frames);
    }
    // The sanitised copy both the interpolator and the loudness read.
    const bool copy = loudness_on_ || tp_on_;
    scratch_.assign(copy ? static_cast<std::size_t>(channels) * max_frames : 0, 0.0f);
    scratch_ptr_.assign(copy ? channels : 0, nullptr);
    for (std::size_t c = 0; c < scratch_ptr_.size(); ++c) {
        scratch_ptr_[c] = scratch_.data() + c * max_frames;
    }
    // The true-peak is ours, read per take; the loudness meter's own would
    // only repeat it.
    if (loudness_on_ &&
        !loudness_.prepare(sample_rate, channels,
                           options.weights.empty() ? nullptr : options.weights.data(), false)) {
        return false;
    }
    reset_requested_.store(false, std::memory_order_relaxed);
    reset_now();
    return true;
}

void TrackMeter::raise(std::atomic<float>& a, float v) noexcept {
    // The reader's take may zero it between our load and store; a CAS keeps
    // either the reader's zero followed by our peak, or the larger value.
    float cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

float TrackMeter::take_peak(std::uint32_t ch) noexcept {
    return ch < channels_ ? ch_[ch].peak.exchange(0.0f, std::memory_order_relaxed) : 0.0f;
}

float TrackMeter::display_peak(std::uint32_t ch) const noexcept {
    return ch < channels_ ? ch_[ch].display.load(std::memory_order_relaxed) : 0.0f;
}

float TrackMeter::take_true_peak(std::uint32_t ch) noexcept {
    return ch < channels_ ? ch_[ch].true_peak.exchange(0.0f, std::memory_order_relaxed) : 0.0f;
}

void TrackMeter::reset_now() noexcept {
    // Clears the loudness histograms, 320 KB: about 67 us per stereo meter
    // on the reference laptop, in the callback that sees the request. The
    // cost is stated in meter.h; staggering a "reset all" is the caller's.
    if (loudness_on_) loudness_.reset();
    if (tp_on_) tp_.reset();
    for (std::uint32_t c = 0; c < channels_; ++c) {
        Channel& k = ch_[c];
        k.env = 0.0;
        k.tp_max = 0.0;
        k.peak.store(0.0f, std::memory_order_relaxed);
        k.display.store(0.0f, std::memory_order_relaxed);
        k.true_peak.store(0.0f, std::memory_order_relaxed);
    }
    nonfinite_.store(0, std::memory_order_relaxed);
    steps_.store(0, std::memory_order_relaxed);
    tp_max_db_.store(kFloorDb, std::memory_order_relaxed);
    momentary_.store(kFloorDb, std::memory_order_relaxed);
    short_term_.store(kFloorDb, std::memory_order_relaxed);
    max_momentary_.store(kFloorDb, std::memory_order_relaxed);
    max_short_term_.store(kFloorDb, std::memory_order_relaxed);
    integrated_.store(kFloorDb, std::memory_order_relaxed);
    lra_.store(0.0f, std::memory_order_relaxed);
}

void TrackMeter::process(const float* const* in, std::uint32_t frames) noexcept {
    if (channels_ == 0) return;
    const DenormalGuard ftz;
    if (reset_requested_.exchange(false, std::memory_order_acquire)) reset_now();
    std::uint32_t done = 0;
    while (done < frames) {
        const std::uint32_t n = frames - done < max_frames_ ? frames - done : max_frames_;
        std::uint64_t bad = 0;
        double tp_all = 0.0;
        for (std::uint32_t c = 0; c < channels_; ++c) {
            Channel& k = ch_[c];
            const float* x = in[c] + done;
            float* s = scratch_.empty()
                           ? nullptr
                           : scratch_.data() + static_cast<std::size_t>(c) * max_frames_;
            const double fall = fall_;
            double env = k.env;
            float pk = 0.0f;
            for (std::uint32_t i = 0; i < n; ++i) {
                float v = x[i];
                if (!std::isfinite(v)) {
                    ++bad;
                    v = 0.0f;
                }
                if (s != nullptr) s[i] = v;
                const float a = std::fabs(v);
                if (a > pk) pk = a;
                const double held = env * fall;
                env = a > held ? a : held;
            }
            if (env < kEnvFloor) env = 0.0;
            k.env = env;
            raise(k.peak, pk);
            k.display.store(static_cast<float>(env), std::memory_order_relaxed);
            if (tp_on_) {
                const double tpk = tp_.process_block(c, s, n);
                raise(k.true_peak, static_cast<float>(tpk));
                if (tpk > k.tp_max) k.tp_max = tpk;
                if (k.tp_max > tp_all) tp_all = k.tp_max;
            }
        }
        if (bad != 0) nonfinite_.fetch_add(bad, std::memory_order_relaxed);
        if (tp_on_) tp_max_db_.store(to_db(tp_all), std::memory_order_relaxed);
        if (loudness_on_) {
            const std::uint64_t before = loudness_.steps();
            loudness_.process(scratch_ptr_.data(), n);
            publish_loudness(before);
        }
        done += n;
    }
}

void TrackMeter::publish_loudness(std::uint64_t before) noexcept {
    const std::uint64_t after = loudness_.steps();
    if (after == before) return;
    momentary_.store(static_cast<float>(loudness_.momentary_lufs()), std::memory_order_relaxed);
    short_term_.store(static_cast<float>(loudness_.short_term_lufs()), std::memory_order_relaxed);
    max_momentary_.store(static_cast<float>(loudness_.max_momentary_lufs()),
                         std::memory_order_relaxed);
    max_short_term_.store(static_cast<float>(loudness_.max_short_term_lufs()),
                          std::memory_order_relaxed);
    // The gated statistics change only on a 100 ms gate step (ten 10 ms
    // steps, loudness.h).
    if (after / 10 != before / 10) {
        integrated_.store(static_cast<float>(loudness_.integrated_lufs()),
                          std::memory_order_relaxed);
        lra_.store(static_cast<float>(loudness_.loudness_range_lu()), std::memory_order_relaxed);
    }
    steps_.store(after, std::memory_order_relaxed);
}

}  // namespace sonora::dsp
