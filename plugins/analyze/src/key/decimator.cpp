// SPDX-License-Identifier: GPL-3.0-or-later
#include "key/decimator.h"

#include <algorithm>
#include <cmath>

namespace sonora::analyze::key {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

bool Decimator::prepare(double in_rate, double out_rate) {
    if (!(in_rate > 0.0) || !(out_rate > 0.0)) return false;
    step_ = in_rate / out_rate;
    const double fc = 0.45 * std::min(in_rate, out_rate);
    const double w = kZeroCrossings * in_rate / (2.0 * fc);
    half_ = static_cast<int>(std::ceil(w));
    const int taps = 2 * half_;
    table_.assign(static_cast<std::size_t>(kPhases + 1) * static_cast<std::size_t>(taps), 0.0f);
    const double gain = 2.0 * fc / in_rate;
    for (int p = 0; p <= kPhases; ++p) {
        const double frac = static_cast<double>(p) / kPhases;
        for (int k = 0; k < taps; ++k) {
            const double x = (k - half_ + 1) - frac;
            double h = gain;
            if (std::fabs(x) > 1e-12) h = gain * std::sin(2.0 * kPi * fc / in_rate * x) / (2.0 * kPi * fc / in_rate * x);
            const double win = std::fabs(x) < w ? 0.5 * (1.0 + std::cos(kPi * x / w)) : 0.0;
            table_[static_cast<std::size_t>(p) * static_cast<std::size_t>(taps) + static_cast<std::size_t>(k)] =
                static_cast<float>(h * win);
        }
    }
    next_ = 0.0;
    base_ = -(half_ - 1);  // the signal before the start is zero
    buf_.assign(static_cast<std::size_t>(half_ - 1), 0.0f);
    return true;
}

void Decimator::process(const float* in, std::size_t n, std::vector<float>& out) {
    buf_.insert(buf_.end(), in, in + n);
    const int taps = 2 * half_;
    const std::int64_t end = base_ + static_cast<std::int64_t>(buf_.size());
    for (;;) {
        const std::int64_t i0 = static_cast<std::int64_t>(std::floor(next_));
        if (i0 + half_ >= end) break;  // the last tap is not here yet
        const double pos = (next_ - static_cast<double>(i0)) * kPhases;
        const int p = std::min(static_cast<int>(pos), kPhases - 1);
        const float a = static_cast<float>(pos - p);
        const float* t0 = &table_[static_cast<std::size_t>(p) * static_cast<std::size_t>(taps)];
        const float* t1 = t0 + taps;
        const float* x = &buf_[static_cast<std::size_t>(i0 - half_ + 1 - base_)];
        double acc = 0.0;
        for (int k = 0; k < taps; ++k) acc += static_cast<double>(x[k]) * ((1.0f - a) * t0[k] + a * t1[k]);
        out.push_back(static_cast<float>(acc));
        next_ += step_;
    }
    const std::int64_t keep_from = static_cast<std::int64_t>(std::floor(next_)) - half_ + 1;
    if (keep_from > base_) {
        const std::size_t drop = std::min(static_cast<std::size_t>(keep_from - base_), buf_.size());
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(drop));
        base_ += static_cast<std::int64_t>(drop);
    }
}

}  // namespace sonora::analyze::key
