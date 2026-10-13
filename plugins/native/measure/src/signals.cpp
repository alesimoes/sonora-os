// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>

#include "sonora/dsp/fft.h"
#include "sonora/dsp/math.h"
#include "sonora/measure/measure.h"

namespace sonora::measure {

using dsp::kPi;
using dsp::kTwoPi;

namespace {
std::size_t frames_of(double fs, double seconds) {
    return static_cast<std::size_t>(std::llround(fs * seconds));
}
}  // namespace

Chans sine(double fs, double freq, double dbfs, double seconds, std::uint32_t channels) {
    const double a = dsp::db_to_gain(dbfs);
    Chans x(channels, std::vector<float>(frames_of(fs, seconds)));
    for (std::size_t i = 0; i < x[0].size(); ++i) {
        const auto v = static_cast<float>(a * std::sin(kTwoPi * freq * static_cast<double>(i) / fs));
        for (auto& c : x) c[i] = v;
    }
    return x;
}

Chans silence(double fs, double seconds, std::uint32_t channels) {
    return Chans(channels, std::vector<float>(frames_of(fs, seconds), 0.0f));
}

Chans noise(double fs, double dbfs_peak, double seconds, std::uint32_t seed, std::uint32_t channels) {
    const double a = dsp::db_to_gain(dbfs_peak);
    Chans x(channels, std::vector<float>(frames_of(fs, seconds)));
    for (std::uint32_t c = 0; c < channels; ++c) {
        std::uint32_t s = seed * 2654435761u + c + 1u;
        for (float& v : x[c]) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            v = static_cast<float>(a * (s / 4294967296.0 * 2.0 - 1.0));
        }
    }
    return x;
}

Chans log_sweep(double fs, double f1, double f2, double dbfs, double seconds, std::uint32_t channels) {
    const double a = dsp::db_to_gain(dbfs);
    const double k = std::log(f2 / f1);
    Chans x(channels, std::vector<float>(frames_of(fs, seconds)));
    for (std::size_t i = 0; i < x[0].size(); ++i) {
        const double t = static_cast<double>(i) / fs;
        const double ph = kTwoPi * f1 * seconds / k * (std::exp(t / seconds * k) - 1.0);
        const auto v = static_cast<float>(a * std::sin(ph));
        for (auto& c : x) c[i] = v;
    }
    return x;
}

void band_limit_and_fade(Chans& x, double fs, double f_hz) {
    const int half = 255;
    const double fc = f_hz / fs;
    const double beta = dsp::kaiser_beta(100.0);
    std::vector<double> h(2 * half + 1);
    for (int k = -half; k <= half; ++k) {
        const double t = 2.0 * fc * k;
        const double s = k == 0 ? 1.0 : std::sin(kPi * t) / (kPi * t);
        h[k + half] = 2.0 * fc * s * dsp::kaiser(static_cast<double>(k) / half, beta);
    }
    for (auto& c : x) {
        std::vector<float> y(c.size());
        const long n = static_cast<long>(c.size());
        for (long i = 0; i < n; ++i) {
            double acc = 0.0;
            for (int k = -half; k <= half; ++k) {
                const long j = i - k;
                if (j >= 0 && j < n) acc += h[k + half] * c[j];
            }
            y[i] = static_cast<float>(acc);
        }
        const std::size_t f = std::min<std::size_t>(frames_of(fs, 0.01), y.size() / 2);
        for (std::size_t i = 0; i < f; ++i) {
            const auto g = static_cast<float>(static_cast<double>(i) / f);
            y[i] *= g;
            y[y.size() - 1 - i] *= g;
        }
        c = std::move(y);
    }
}

Chans concat(const std::vector<Chans>& parts) {
    Chans out(parts.front().size());
    for (const Chans& p : parts) {
        for (std::size_t c = 0; c < out.size(); ++c) out[c].insert(out[c].end(), p[c].begin(), p[c].end());
    }
    return out;
}

Chans golden_input(double fs, double seconds) {
    // A band-limited noise burst, a sweep, an impulse and the tail: enough
    // to pin a filter's, a detector's and a delay line's behaviour.
    Chans burst = noise(fs, -12.0, 0.1, 1234u);
    band_limit_and_fade(burst, fs);
    Chans sweep = log_sweep(fs, 20.0, 20000.0, -6.0, 0.2, 2);
    // Decorrelate the channels so a stereo processor's channels differ.
    for (std::size_t i = 0; i < sweep[1].size(); ++i) sweep[1][i] *= -0.5f;
    Chans imp = silence(fs, 0.01);
    imp[0][0] = 0.5f;
    imp[1][0] = -0.25f;
    Chans x = concat({burst, sweep, imp});
    const auto total = frames_of(fs, seconds);
    for (auto& c : x) c.resize(total, 0.0f);
    return x;
}

}  // namespace sonora::measure
