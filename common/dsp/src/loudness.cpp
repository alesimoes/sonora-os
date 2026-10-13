// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/loudness.h"

#include <cmath>

#include "sonora/dsp/math.h"

namespace sonora::dsp {

void k_weighting(double fs, BiquadCoeffs& stage1, BiquadCoeffs& stage2) noexcept {
    // Stage 1, the pre-filter: a second-order high shelf of about +4 dB
    // above 1.7 kHz, modelling the head's acoustic effect. Analog form
    // (Vh s^2 + Vb s / Q + 1) / (s^2 + s / Q + 1).
    {
        const double f0 = 1681.974450955533;
        const double g = 3.999843853973347;
        const double q = 0.7071752369554196;
        const double vh = std::pow(10.0, g / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        AnalogSection s{vh, vb / q, 1.0, 1.0, 1.0 / q, 1.0};
        stage1 = bilinear(s, f0, fs);
    }
    // Stage 2, the RLB weighting: a second-order high-pass near 38 Hz.
    {
        const double f0 = 38.13547087602444;
        const double q = 0.5003270373238773;
        AnalogSection s{1.0, 0.0, 0.0, 1.0, 1.0 / q, 1.0};
        stage2 = bilinear(s, f0, fs);
        stage2.b0 = 1.0;
        stage2.b1 = -2.0;
        stage2.b2 = 1.0;
    }
}

void LoudnessMeter::Hist::clear() noexcept {
    for (auto& c : count) c = 0;
    for (auto& e : energy) e = 0.0;
}

void LoudnessMeter::Hist::add(double lufs, double e) noexcept {
    if (lufs < kHistMin) return;  // the absolute gate
    double idx = std::floor((lufs - kHistMin) / kBin);
    const double last = static_cast<double>(count.size() - 1);
    if (idx > last) idx = last;
    const auto i = static_cast<std::size_t>(idx);
    ++count[i];
    energy[i] += e;
}

double LoudnessMeter::to_lufs(double ms) noexcept {
    if (!(ms > 0.0)) return kFloorLufs;
    const double l = -0.691 + 10.0 * std::log10(ms);
    return l < kFloorLufs ? kFloorLufs : l;
}

bool LoudnessMeter::prepare(double sample_rate, std::uint32_t channels,
                            const double* weights) {
    if (!(sample_rate > 0.0) || channels == 0) return false;
    rate_ = sample_rate;
    channels_ = channels;
    weight_.assign(channels, 1.0);
    if (weights != nullptr) {
        for (std::uint32_t c = 0; c < channels; ++c) weight_[c] = weights[c];
    }
    k_weighting(sample_rate, k1_, k2_);
    s1_.assign(channels, BiquadState{});
    s2_.assign(channels, BiquadState{});
    tp_.prepare(channels);
    tp_max_.assign(channels, 0.0);
    sub_len_ = static_cast<std::uint32_t>(std::lround(sample_rate / 100.0));
    if (sub_len_ == 0) sub_len_ = 1;
    ring_.assign(kSubsPerShortTerm, 0.0);
    const auto bins = static_cast<std::size_t>(std::lround((kHistMax - kHistMin) / kBin));
    gate_hist_.count.assign(bins, 0);
    gate_hist_.energy.assign(bins, 0.0);
    lra_hist_.count.assign(bins, 0);
    lra_hist_.energy.assign(bins, 0.0);
    reset();
    return true;
}

void LoudnessMeter::reset() noexcept {
    for (auto& s : s1_) s.reset();
    for (auto& s : s2_) s.reset();
    tp_.reset();
    for (auto& m : tp_max_) m = 0.0;
    for (auto& r : ring_) r = 0.0;
    ring_pos_ = 0;
    sub_fill_ = 0;
    sub_sum_ = 0.0;
    steps_ = 0;
    momentary_ = short_term_ = kFloorLufs;
    max_momentary_ = max_short_term_ = kFloorLufs;
    gate_hist_.clear();
    lra_hist_.clear();
}

void LoudnessMeter::process(const float* const* in, std::uint32_t frames) noexcept {
    for (std::uint32_t i = 0; i < frames; ++i) {
        double sum = 0.0;
        for (std::uint32_t c = 0; c < channels_; ++c) {
            const double x = in[c][i];
            const double y = s2_[c].tick(k2_, s1_[c].tick(k1_, x));
            sum += weight_[c] * y * y;
            const double tp = tp_.process(c, x);
            if (tp > tp_max_[c]) tp_max_[c] = tp;
        }
        sub_sum_ += sum;
        if (++sub_fill_ == sub_len_) finish_sub_block();
    }
}

void LoudnessMeter::finish_sub_block() noexcept {
    ring_[ring_pos_] = sub_sum_ / sub_len_;
    ring_pos_ = (ring_pos_ + 1) % kSubsPerShortTerm;
    sub_sum_ = 0.0;
    sub_fill_ = 0;
    ++steps_;

    // Sums are recomputed rather than kept running, so no rounding drift
    // accumulates over a long programme. 300 additions per 10 ms.
    double m_sum = 0.0, s_sum = 0.0;
    for (std::uint32_t k = 0; k < kSubsPerShortTerm; ++k) {
        const std::uint32_t idx = (ring_pos_ + kSubsPerShortTerm - 1 - k) % kSubsPerShortTerm;
        if (k < kSubsPerMomentary) m_sum += ring_[idx];
        s_sum += ring_[idx];
    }
    const double m_ms = m_sum / kSubsPerMomentary;
    const double s_ms = s_sum / kSubsPerShortTerm;
    momentary_ = to_lufs(m_ms);
    short_term_ = to_lufs(s_ms);
    // Until a full window has elapsed the window holds silence before the
    // start; that reads low, never high, so the maxima stay honest.
    if (momentary_ > max_momentary_) max_momentary_ = momentary_;
    if (short_term_ > max_short_term_) max_short_term_ = short_term_;

    if (steps_ % kSubsPerGateStep == 0) {
        if (steps_ >= kSubsPerMomentary) gate_hist_.add(momentary_, m_ms);
        if (steps_ >= kSubsPerShortTerm) lra_hist_.add(short_term_, s_ms);
    }
}

double LoudnessMeter::integrated_lufs() const noexcept {
    double e = 0.0;
    std::uint64_t n = 0;
    const std::uint32_t bins = gate_hist_.bins();
    for (std::uint32_t i = 0; i < bins; ++i) {
        e += gate_hist_.energy[i];
        n += gate_hist_.count[i];
    }
    if (n == 0) return kFloorLufs;
    const double rel = to_lufs(e / n) - 10.0;
    double e2 = 0.0;
    std::uint64_t n2 = 0;
    for (std::uint32_t i = 0; i < bins; ++i) {
        // A bin is in when its lower edge is at or above the gate: blocks
        // within 0.01 LU under the gate may be dropped, never added.
        const double lo = kHistMin + i * kBin;
        if (lo >= rel) {
            e2 += gate_hist_.energy[i];
            n2 += gate_hist_.count[i];
        }
    }
    return n2 == 0 ? kFloorLufs : to_lufs(e2 / n2);
}

double LoudnessMeter::loudness_range_lu() const noexcept {
    double e = 0.0;
    std::uint64_t n = 0;
    const std::uint32_t bins = lra_hist_.bins();
    for (std::uint32_t i = 0; i < bins; ++i) {
        e += lra_hist_.energy[i];
        n += lra_hist_.count[i];
    }
    if (n == 0) return 0.0;
    const double rel = to_lufs(e / n) - 20.0;
    std::uint32_t first = 0;
    std::uint64_t m = 0;
    for (std::uint32_t i = bins; i-- > 0;) {
        if (kHistMin + i * kBin < rel) {
            first = i + 1;
            break;
        }
        m += lra_hist_.count[i];
    }
    if (m == 0) return 0.0;
    // Tech 3342's reference: sorted(round((n-1) * p / 100 + 1)), 1-based.
    auto value_at_rank = [&](std::uint64_t rank1) {
        std::uint64_t seen = 0;
        for (std::uint32_t i = first; i < bins; ++i) {
            seen += lra_hist_.count[i];
            if (seen >= rank1) return kHistMin + (i + 0.5) * kBin;
        }
        return kHistMax;
    };
    const auto low = static_cast<std::uint64_t>(std::llround((m - 1) * 0.10 + 1.0));
    const auto high = static_cast<std::uint64_t>(std::llround((m - 1) * 0.95 + 1.0));
    return value_at_rank(high) - value_at_rank(low);
}

double LoudnessMeter::true_peak_dbtp() const noexcept {
    double m = 0.0;
    for (double v : tp_max_) m = v > m ? v : m;
    return gain_to_db(m);
}

}  // namespace sonora::dsp
