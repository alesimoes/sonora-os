// SPDX-License-Identifier: GPL-3.0-or-later
#include "beat/onset_envelope.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "sonora/dsp/fft.h"

namespace sonora::analyze::beat {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kBins = kWindow / 2;
// Magnitudes are divided by the window sum, so a full-scale sine is ~0.5; the
// gain puts a -80 dBFS noise floor below log1p's knee and a click well above.
constexpr double kLogGain = 1000.0;
constexpr std::size_t kMeanHalf = 40;  // frames each side of the local mean
constexpr int kPeakHalf = 3;
constexpr float kPeakFloor = 0.25f;
}  // namespace

bool spectral_flux(const std::vector<float>& signal, const std::atomic<bool>* cancel, std::vector<float>& flux) {
    flux.clear();
    if (signal.size() < static_cast<std::size_t>(kWindow)) return true;
    dsp::RealFft fft;
    if (!fft.prepare(kWindow)) return true;
    std::vector<double> win(kWindow), frame(kWindow);
    double wsum = 0.0;
    for (int i = 0; i < kWindow; ++i) {
        win[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / kWindow);
        wsum += win[static_cast<std::size_t>(i)];
    }
    std::vector<std::complex<double>> spec(kBins + 1);
    std::vector<double> prev(kBins, 0.0), cur(kBins, 0.0);
    const std::size_t frames = (signal.size() - kWindow) / kHop + 1;
    flux.assign(frames, 0.0f);
    for (std::size_t t = 0; t < frames; ++t) {
        if ((t & 255u) == 0 && cancel && cancel->load()) return false;
        const float* x = &signal[t * kHop];
        for (int i = 0; i < kWindow; ++i) frame[static_cast<std::size_t>(i)] = x[i] * win[static_cast<std::size_t>(i)];
        fft.forward(frame.data(), spec.data());
        double f = 0.0;
        for (int k = 0; k < kBins; ++k) {
            cur[static_cast<std::size_t>(k)] = std::log1p(kLogGain * std::abs(spec[static_cast<std::size_t>(k) + 1]) / wsum);
            if (t > 0) f += std::max(0.0, cur[static_cast<std::size_t>(k)] - prev[static_cast<std::size_t>(k)]);
        }
        flux[t] = static_cast<float>(f / kBins);
        std::swap(prev, cur);
    }
    return true;
}

bool adaptive_onset(const std::vector<float>& flux, std::vector<float>& onset) {
    const std::size_t n = flux.size();
    onset.assign(n, 0.0f);
    if (n < 2) return false;
    std::vector<double> cum(n + 1, 0.0);
    for (std::size_t i = 0; i < n; ++i) cum[i + 1] = cum[i] + flux[i];
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t a = i > kMeanHalf ? i - kMeanHalf : 0;
        const std::size_t b = std::min(n, i + kMeanHalf + 1);
        const double mean = (cum[b] - cum[a]) / static_cast<double>(b - a);
        onset[i] = static_cast<float>(std::max(0.0, flux[i] - mean));
    }
    std::vector<float> sorted(onset);
    const std::size_t top = std::max<std::size_t>(1, n / 20);
    std::nth_element(sorted.begin(), sorted.end() - static_cast<std::ptrdiff_t>(top), sorted.end());
    double scale = 0.0;
    for (std::size_t i = n - top; i < n; ++i) scale += sorted[i];
    scale /= static_cast<double>(top);
    if (!(scale > 1e-6)) return false;
    for (float& v : onset) v = static_cast<float>(v / scale);
    return true;
}

std::vector<int> pick_peaks(const std::vector<float>& onset) {
    std::vector<int> peaks;
    const int n = static_cast<int>(onset.size());
    for (int t = 0; t < n; ++t) {
        const float v = onset[static_cast<std::size_t>(t)];
        if (v < kPeakFloor) continue;
        bool top = true;
        for (int d = 1; d <= kPeakHalf && top; ++d) {
            if (t - d >= 0 && onset[static_cast<std::size_t>(t - d)] >= v) top = false;
            if (t + d < n && onset[static_cast<std::size_t>(t + d)] > v) top = false;
        }
        if (top) peaks.push_back(t);
    }
    return peaks;
}

}  // namespace sonora::analyze::beat
