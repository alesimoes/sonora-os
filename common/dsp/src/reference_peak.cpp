// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/dsp/reference_peak.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "sonora/dsp/fft.h"
#include "sonora/dsp/math.h"
#include "sonora/dsp/oversampling.h"

namespace sonora::dsp {

namespace {

constexpr int kHalf = 256;
constexpr int kOver = 16;

const std::vector<double>& sinc_table() {
    static const std::vector<double> table = [] {
        std::vector<double> t(kOver * (2 * kHalf) + 1);
        const double beta = kaiser_beta(120.0);
        for (int i = 0; i <= kOver * 2 * kHalf; ++i) {
            const double x = static_cast<double>(i) / kOver - kHalf;
            const double s = x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            t[static_cast<std::size_t>(i)] = s * kaiser(x / kHalf, beta);
        }
        return t;
    }();
    return table;
}

// The reconstruction at time q / 16 samples.
double at16(const float* y, long n, long q, const std::vector<double>& table) {
    const long k = q >= 0 ? q / kOver : -((-q + kOver - 1) / kOver);
    const int p = static_cast<int>(q - k * kOver);
    if (p == 0) return k >= 0 && k < n ? y[k] : 0.0;
    double acc = 0.0;
    for (int j = -kHalf + 1; j <= kHalf; ++j) {
        const long idx = k + j;
        if (idx < 0 || idx >= n) continue;
        acc += y[idx] * table[static_cast<std::size_t>((kHalf - j) * kOver + p)];
    }
    return acc;
}

}  // namespace

double reference_true_peak(const float* y, std::size_t n, double near_level) {
    double pk = 0.0;
    for (std::size_t i = 0; i < n; ++i) pk = std::fmax(pk, std::fabs(y[i]));
    if (n == 0) return pk;

    constexpr std::uint32_t kFactor = 4, kBlock = 1024;
    constexpr long kSpan = kOver / static_cast<long>(kFactor);  // 16ths per 4x step
    Oversampler os;
    os.prepare(kFactor, 1, kBlock, 0.495, 100.0);
    const std::uint32_t up = os.up_delay();
    // Zeros after the end flush the pre-scan's delay, so the last peaks
    // are seen too.
    const std::size_t total = n + up / kFactor + 4;
    std::vector<double> z;
    z.reserve(total * kFactor);
    std::vector<float> block(kBlock);
    for (std::size_t pos = 0; pos < total; pos += kBlock) {
        const auto len = static_cast<std::uint32_t>(total - pos < kBlock ? total - pos : kBlock);
        for (std::uint32_t i = 0; i < len; ++i) block[i] = pos + i < n ? y[pos + i] : 0.0f;
        const double* h = os.up(0, block.data(), len);
        z.insert(z.end(), h, h + static_cast<std::size_t>(len) * kFactor);
    }

    const std::vector<double>& table = sinc_table();
    const double pre_level = near_level * std::cos(kPi / 8.0) * 0.97;
    const long ln = static_cast<long>(n);
    for (std::size_t m = 1; m + 1 < z.size(); ++m) {
        const double a = std::fabs(z[m]);
        if (a < pre_level || a < std::fabs(z[m - 1]) || a < std::fabs(z[m + 1])) continue;
        // Pre-scan point m is at time (m - up) / 4 samples: (m - up) * 4 in 16ths.
        const long c16 = (static_cast<long>(m) - static_cast<long>(up)) * kSpan;
        for (long d = -kSpan; d <= kSpan; ++d) {
            pk = std::fmax(pk, std::fabs(at16(y, ln, c16 + d, table)));
        }
    }
    return pk;
}

}  // namespace sonora::dsp
