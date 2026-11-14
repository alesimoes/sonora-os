// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/resample.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "sonora/dsp/math.h"

namespace sonora::dsp {

namespace {

constexpr double kBeta = 9.0;

double bessel_i0(double x) noexcept {
    double sum = 1.0, term = 1.0;
    const double q = 0.25 * x * x;
    for (int k = 1; k < 60; ++k) {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < 1e-17 * sum) break;
    }
    return sum;
}

// The prototype at `tau` input samples from its centre, windowed over
// +-span input samples.
double kaiser_sinc(double tau, double cut, double span, double i0_beta) noexcept {
    const double w = tau / span;
    if (w <= -1.0 || w >= 1.0) return 0.0;
    const double d = tau * cut;
    const double sinc = std::fabs(d) < 1e-12 ? 1.0 : std::sin(kPi * d) / (kPi * d);
    return cut * sinc * bessel_i0(kBeta * std::sqrt(1.0 - w * w)) / i0_beta;
}

// ceil() that does not round an exact integer up for a last-bit error.
std::uint32_t ceil_whole(double x) noexcept { return static_cast<std::uint32_t>(std::ceil(x - 1e-9)); }

}  // namespace

bool Resampler::prepare(std::uint32_t up, std::uint32_t down, double cut, std::uint32_t delay, std::uint32_t max_in) {
    if (up == 0 || down == 0 || up > kMaxRatioTerm || down > kMaxRatioTerm || max_in == 0) return false;
    if (!(cut > 0.0 && cut <= 1.0)) return false;
    const std::uint64_t taps64 = 2 * static_cast<std::uint64_t>(delay) / up + 1;
    if (static_cast<std::uint64_t>(up) * taps64 > kMaxCoefficients) return false;
    const auto taps = static_cast<std::uint32_t>(taps64);
    up_ = up;
    down_ = down;
    taps_ = taps;
    delay_ = delay;
    const double span = std::max(1.0, static_cast<double>(delay) / up_);
    const double i0b = bessel_i0(kBeta);
    coef_.assign(static_cast<std::size_t>(up_) * taps_, 0.0f);
    std::vector<double> row(taps_);
    for (std::uint32_t phase = 0; phase < up_; ++phase) {
        double sum = 0.0;
        for (std::uint32_t i = 0; i < taps_; ++i) {
            // Slot i holds the input taps_ - 1 - i samples before the newest;
            // the output sits `phase` high-rate samples after the newest.
            const std::uint64_t hi = phase + static_cast<std::uint64_t>(taps_ - 1 - i) * up_;
            const double tau = (static_cast<double>(hi) - delay) / up_;
            row[i] = hi > 2ull * delay ? 0.0 : kaiser_sinc(tau, cut, span, i0b);
            sum += row[i];
        }
        // Each phase is an interpolator on its own: scaled to unit DC gain,
        // so a constant passes exactly and no phase-rate ripple appears.
        for (std::uint32_t i = 0; i < taps_; ++i) {
            coef_[static_cast<std::size_t>(phase) * taps_ + i] = static_cast<float>(sum != 0.0 ? row[i] / sum : 0.0);
        }
    }
    hist_.assign(2 * static_cast<std::size_t>(taps_), 0.0f);
    // Each input moves the next output up_ closer, each output down_
    // further, and the position stays in [0, up_ + down_).
    max_out_ = static_cast<std::uint32_t>((static_cast<std::uint64_t>(max_in + 1) * up_ + down_ - 1) / down_) + 2;
    reset();
    return true;
}

void Resampler::reset() noexcept {
    std::fill(hist_.begin(), hist_.end(), 0.0f);
    write_ = 0;
    t_ = up_;  // nothing due before the first input
}

std::uint32_t Resampler::process(const float* in, std::uint32_t n, float* out, std::uint32_t cap) noexcept {
    if (hist_.empty()) return 0;  // never prepared
    std::uint32_t count = 0;
    const std::uint32_t taps = taps_;
    for (std::uint32_t k = 0; k < n; ++k) {
        // One NaN would poison the ring, and every output, for `taps` inputs.
        const float x = is_finite(in[k]) ? in[k] : 0.0f;
        hist_[write_] = x;
        hist_[write_ + taps] = x;
        const float* window = hist_.data() + write_ + 1;  // oldest .. newest
        write_ = write_ + 1 == taps ? 0 : write_ + 1;
        t_ -= up_;
        while (t_ < up_) {
            const float* c = coef_.data() + static_cast<std::size_t>(t_) * taps;
            float acc = 0.0f;
            for (std::uint32_t i = 0; i < taps; ++i) acc += c[i] * window[i];
            if (count < cap) out[count++] = acc;
            t_ += down_;
        }
    }
    return count;
}

bool plan_rates(double session_rate, double capture_rate, RatePlan* plan, std::string* error) {
    *plan = RatePlan{};
    const double s = std::round(session_rate), c = std::round(capture_rate);
    if (!(s >= 1000.0 && s <= 768000.0) || !(c >= 1000.0 && c <= 768000.0) ||
        std::fabs(s - session_rate) > 1e-6 || std::fabs(c - capture_rate) > 1e-6) {
        if (error) *error = "the capture rate or the session rate is not a whole number of hertz between 1 and 768 kHz";
        return false;
    }
    if (s == c) return true;
    const auto si = static_cast<std::uint64_t>(s), ci = static_cast<std::uint64_t>(c);
    const std::uint64_t g = std::gcd(si, ci);
    if (ci / g > kMaxRatioTerm || si / g > kMaxRatioTerm) {
        if (error) *error = "the capture rate and the session rate have no small ratio to convert between";
        return false;
    }
    plan->resample = true;
    plan->p = static_cast<std::uint32_t>(ci / g);
    plan->q = static_cast<std::uint32_t>(si / g);
    const double lower = std::min(s, c);
    plan->cut_up = kResampleCut * lower / s;
    plan->cut_down = kResampleCut * lower / c;
    // Half-lengths in each stage's input samples, then in high-rate samples
    // rounded up to a multiple of p (see the header).
    plan->delay_up = ceil_whole(kResampleZeros / plan->cut_up) * plan->p;
    const double down_hi = kResampleZeros / plan->cut_down * plan->q;
    plan->delay_down = ceil_whole(down_hi / plan->p) * plan->p;
    plan->latency = (plan->delay_up + plan->delay_down) / plan->p;
    return true;
}

bool prepare_pair(const RatePlan& plan, std::uint32_t max_in, Resampler* up, Resampler* down) {
    if (!plan.resample || up == nullptr || down == nullptr) return false;
    if (!up->prepare(plan.p, plan.q, plan.cut_up, plan.delay_up, max_in)) return false;
    return down->prepare(plan.q, plan.p, plan.cut_down, plan.delay_down, up->max_out());
}

}  // namespace sonora::dsp
