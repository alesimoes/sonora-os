// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/oversampling.h"

#include <cmath>

#include "sonora/dsp/fft.h"
#include "sonora/dsp/math.h"

namespace sonora::dsp {

namespace {

// Half-band taps h[2j], j = 0..2k-1 (the non-zero, non-centre ones), for a
// filter of 4k-1 taps centred on index 2k-1.
std::vector<double> halfband_even_taps(std::uint32_t k, double beta) {
    const int c = static_cast<int>(2 * k - 1);
    std::vector<double> even(2 * k);
    double sum = 0.0;
    for (std::uint32_t j = 0; j < 2 * k; ++j) {
        const int d = static_cast<int>(2 * j) - c;  // odd
        const double ideal = std::sin(kPi * d / 2.0) / (kPi * d);
        even[j] = ideal * kaiser(static_cast<double>(d) / c, beta);
        sum += even[j];
    }
    // The non-centre taps of a half-band sum to 1/2, so with the centre's
    // 1/2 the DC gain is exactly one; the window leaves a residue of order
    // the stopband ripple, removed here.
    for (double& t : even) t *= 0.5 / sum;
    return even;
}

// sum over j < 2k of c[j] x[j], for half-band taps c, which are symmetric
// (c[j] = c[2k - 1 - j]): folded to k products, and summed into four
// independent accumulators. A single running sum is bound by the adder's
// latency, not its throughput; four chains run about four times faster.
// The order of summation is fixed here, so the result is the same on every
// compiler (no reassociation is allowed or needed).
double folded_dot(const double* c, const double* x, std::uint32_t k) noexcept {
    const std::uint32_t last = 2 * k - 1;
    double a0 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
    std::uint32_t j = 0;
    for (; j + 4 <= k; j += 4) {
        a0 += c[j] * (x[j] + x[last - j]);
        a1 += c[j + 1] * (x[j + 1] + x[last - j - 1]);
        a2 += c[j + 2] * (x[j + 2] + x[last - j - 2]);
        a3 += c[j + 3] * (x[j + 3] + x[last - j - 3]);
    }
    for (; j < k; ++j) a0 += c[j] * (x[j] + x[last - j]);
    return (a0 + a1) + (a2 + a3);
}

}  // namespace

bool Oversampler::prepare(std::uint32_t factor, std::uint32_t channels,
                          std::uint32_t max_frames, double passband_ratio,
                          double stopband_db) {
    std::uint32_t s_count = 0;
    if (factor == 1) s_count = 0;
    else if (factor == 2) s_count = 1;
    else if (factor == 4) s_count = 2;
    else if (factor == 8) s_count = 3;
    else return false;
    factor_ = factor;

    const double beta = kaiser_beta(stopband_db);
    stages_.assign(s_count, Stage{});
    taps_.assign(s_count, 0);
    std::uint64_t top_delay = 0;
    std::uint64_t up_delay = 0;
    for (std::uint32_t s = 0; s < s_count; ++s) {
        const double rate = static_cast<double>(2u << s);  // output rate / base
        const double df = ((1u << s) - 2.0 * passband_ratio) / rate;
        const double n_est = (stopband_db - 7.95) / (2.285 * kTwoPi * df) + 1.0;
        std::uint32_t k = static_cast<std::uint32_t>(std::ceil((n_est + 1.0) / 4.0));
        if (k < 2) k = 2;
        stages_[s].k = k;
        stages_[s].even = halfband_even_taps(k, beta);
        taps_[s] = 4 * k - 1;
        top_delay += static_cast<std::uint64_t>(2 * k - 1) << (s_count - s);
        // On the way up alone, a stage delays by 2k - 1 samples at its
        // output rate.
        up_delay += static_cast<std::uint64_t>(2 * k - 1) << (s_count - 1 - s);
    }
    up_delay_ = static_cast<std::uint32_t>(up_delay);
    const std::uint64_t top = std::uint64_t{1} << s_count;
    pad_ = static_cast<std::uint32_t>((top - top_delay % top) % top);
    latency_ = static_cast<std::uint32_t>((top_delay + pad_) / top);

    ch_.assign(channels, Channel{});
    for (Channel& c : ch_) {
        c.stages.assign(s_count, ChannelStage{});
        for (std::uint32_t s = 0; s < s_count; ++s) {
            const std::uint32_t k = stages_[s].k;
            c.stages[s].up_hist.assign(4 * k, 0.0);
            c.stages[s].dn_even.assign(4 * k, 0.0);
            c.stages[s].dn_odd.assign(2 * (k + 1), 0.0);
        }
        c.bufs.assign(s_count + 1, {});
        for (std::uint32_t s = 0; s <= s_count; ++s) {
            c.bufs[s].assign(static_cast<std::size_t>(max_frames) << s, 0.0);
        }
        c.pad.assign(pad_ + 1, 0.0);
    }
    reset();
    return true;
}

void Oversampler::reset() noexcept {
    for (Channel& c : ch_) {
        for (ChannelStage& cs : c.stages) {
            for (double& v : cs.up_hist) v = 0.0;
            for (double& v : cs.dn_even) v = 0.0;
            for (double& v : cs.dn_odd) v = 0.0;
            cs.up_pos = 0;
            cs.dn_pos = 0;
            cs.dn_opos = 0;
        }
        for (double& v : c.pad) v = 0.0;
        c.pad_pos = 0;
    }
}

double* Oversampler::up(std::uint32_t ch, const float* in, std::uint32_t frames) noexcept {
    Channel& c = ch_[ch];
    double* cur = c.bufs[0].data();
    for (std::uint32_t i = 0; i < frames; ++i) cur[i] = in[i];
    std::uint32_t n = frames;
    for (std::size_t s = 0; s < stages_.size(); ++s) {
        const Stage& st = stages_[s];
        ChannelStage& cs = c.stages[s];
        const std::uint32_t len = 2 * st.k;
        double* out = c.bufs[s + 1].data();
        for (std::uint32_t i = 0; i < n; ++i) {
            cs.up_pos = (cs.up_pos == 0 ? len : cs.up_pos) - 1;
            cs.up_hist[cs.up_pos] = cs.up_hist[cs.up_pos + len] = cur[i];
            const double* h = &cs.up_hist[cs.up_pos];  // h[j] = x[n - j]
            const double acc = folded_dot(st.even.data(), h, st.k);
            // Interpolation gain 2: the zero-stuffed signal has half the
            // energy per sample. The centre branch is 2 * 1/2 = a delay.
            out[2 * i] = 2.0 * acc;
            out[2 * i + 1] = h[st.k - 1];
        }
        cur = out;
        n *= 2;
    }
    return cur;
}

void Oversampler::down(std::uint32_t ch, float* out, std::uint32_t frames) noexcept {
    Channel& c = ch_[ch];
    const std::size_t s_count = stages_.size();
    double* cur = c.bufs[s_count].data();
    std::uint32_t n = frames << s_count;
    if (pad_ != 0) {
        const std::uint32_t len = pad_ + 1;
        for (std::uint32_t i = 0; i < n; ++i) {
            c.pad[c.pad_pos] = cur[i];
            c.pad_pos = (c.pad_pos + 1) % len;
            cur[i] = c.pad[c.pad_pos];  // written pad_ samples ago
        }
    }
    for (std::size_t si = s_count; si-- > 0;) {
        const Stage& st = stages_[si];
        ChannelStage& cs = c.stages[si];
        const std::uint32_t len = 2 * st.k;
        const std::uint32_t olen = st.k + 1;
        double* dst = c.bufs[si].data();
        n /= 2;
        for (std::uint32_t i = 0; i < n; ++i) {
            cs.dn_pos = (cs.dn_pos == 0 ? len : cs.dn_pos) - 1;
            cs.dn_even[cs.dn_pos] = cs.dn_even[cs.dn_pos + len] = cur[2 * i];
            cs.dn_opos = (cs.dn_opos == 0 ? olen : cs.dn_opos) - 1;
            const std::uint32_t op = cs.dn_opos;
            cs.dn_odd[op] = cs.dn_odd[op + olen] = cur[2 * i + 1];
            const double* e = &cs.dn_even[cs.dn_pos];
            const double acc = folded_dot(st.even.data(), e, st.k);
            dst[i] = acc + 0.5 * cs.dn_odd[op + st.k];
        }
        cur = dst;
    }
    for (std::uint32_t i = 0; i < frames; ++i) out[i] = static_cast<float>(cur[i]);
}

}  // namespace sonora::dsp
