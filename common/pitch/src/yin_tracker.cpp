// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <utility>

#include "sonora/pitch/tracker.h"

namespace sonora::pitch {
namespace {

bool check_config(const YinConfig& c, uint32_t rate, std::string& why) {
    if (rate < 8000) {
        why = "pitch: sample rate " + std::to_string(rate) + " Hz is below 8000 Hz";
        return false;
    }
    if (!(c.a4_hz >= 400.0 && c.a4_hz <= 480.0)) {
        why = "pitch: A4 must be 400 to 480 Hz";
        return false;
    }
    if (!(c.hop_ms >= 5.0 && c.hop_ms <= 10.0)) {
        why = "pitch: hop must be 5 to 10 ms";
        return false;
    }
    if (!(c.min_hz >= 40.0 && c.max_hz > c.min_hz && c.max_hz <= 2000.0)) {
        why = "pitch: pitch range must be within 40 to 2000 Hz";
        return false;
    }
    if (!(c.threshold > 0.0 && c.threshold < 1.0)) {
        why = "pitch: threshold must be between 0 and 1";
        return false;
    }
    if (!(c.gate_db > -200.0 && c.gate_db < 0.0)) {
        why = "pitch: gate must be below 0 dBFS";
        return false;
    }
    return true;
}

std::vector<float> decimate(const std::vector<float>& in, std::size_t factor) {
    if (factor <= 1) return in;
    std::vector<float> out(in.size() / factor);
    for (std::size_t i = 0; i < out.size(); ++i) {
        float sum = 0.0f;
        for (std::size_t k = 0; k < factor; ++k) sum += in[i * factor + k];
        out[i] = sum / float(factor);
    }
    return out;
}

// Per-track scratch, sized once, so a frame allocates nothing.
struct Frame {
    std::size_t tau_min = 0, tau_max = 0, window = 0;
    std::vector<float> samples;  // window + tau_max + 2
    std::vector<double> d;       // normalised difference, index 0..tau_max + 1
};

void load_window(const std::vector<float>& x, long start, Frame& f) {
    for (std::size_t j = 0; j < f.samples.size(); ++j) {
        const long at = start + long(j);
        f.samples[j] = (at >= 0 && at < long(x.size())) ? x[std::size_t(at)] : 0.0f;
    }
}

double window_rms(const Frame& f) {
    double e = 0.0;
    for (std::size_t j = 0; j < f.window; ++j) e += double(f.samples[j]) * double(f.samples[j]);
    return std::sqrt(e / double(f.window));
}

void cmndf(Frame& f) {
    f.d[0] = 1.0;
    double running = 0.0;
    for (std::size_t tau = 1; tau <= f.tau_max + 1; ++tau) {
        double sum = 0.0;
        for (std::size_t j = 0; j < f.window; ++j) {
            const double diff = double(f.samples[j]) - double(f.samples[j + tau]);
            sum += diff * diff;
        }
        running += sum;
        f.d[tau] = running > 0.0 ? sum * double(tau) / running : 1.0;
    }
}

// First lag under the threshold, walked down to its local minimum; 0 if none.
std::size_t pick_lag(const Frame& f, double threshold) {
    for (std::size_t tau = f.tau_min; tau <= f.tau_max; ++tau) {
        if (f.d[tau] < threshold) {
            while (tau + 1 <= f.tau_max && f.d[tau + 1] < f.d[tau]) ++tau;
            return tau;
        }
    }
    return 0;
}

double refine_lag(const Frame& f, std::size_t tau) {
    const double a = f.d[tau - 1], b = f.d[tau], c = f.d[tau + 1];
    const double denom = a - 2.0 * b + c;
    if (denom == 0.0) return double(tau);
    return double(tau) + std::clamp((a - c) / (2.0 * denom), -1.0, 1.0);
}

}  // namespace

bool YinTracker::track(const std::vector<float>& mono, uint32_t rate, Contour& out,
                       std::string& why) {
    if (!check_config(config_, rate, why)) return false;
    for (const float s : mono) {
        if (!std::isfinite(s)) {
            why = "pitch: the audio holds a non-finite sample";
            return false;
        }
    }
    const std::size_t factor = std::max<std::size_t>(1, rate / 16000);
    const std::vector<float> x = decimate(mono, factor);
    const double rate_d = double(rate) / double(factor);

    Frame f;
    f.tau_min = std::max<std::size_t>(2, std::size_t(rate_d / config_.max_hz));
    f.tau_max = std::size_t(std::ceil(rate_d / config_.min_hz));
    f.window = f.tau_max;
    f.samples.assign(f.window + f.tau_max + 2, 0.0f);
    f.d.assign(f.tau_max + 2, 1.0);

    Contour c;
    c.rate = rate;
    c.hop = std::max<std::size_t>(1, std::size_t(std::lround(config_.hop_ms * rate / 1000.0)));
    c.length = mono.size();
    c.a4_hz = config_.a4_hz;
    c.frames.assign(mono.size() / c.hop + 1, PitchFrame{});

    const double gate = std::pow(10.0, config_.gate_db / 20.0);
    for (std::size_t i = 0; i < c.frames.size(); ++i) {
        const long centre = long(i * c.hop / factor);
        load_window(x, centre - long((f.window + f.tau_max) / 2), f);
        if (window_rms(f) < gate) continue;
        cmndf(f);
        const std::size_t tau = pick_lag(f, config_.threshold);
        if (tau == 0) continue;
        PitchFrame& fr = c.frames[i];
        fr.voiced = true;
        fr.confidence = float(std::clamp(1.0 - f.d[tau], 0.0, 1.0));
        fr.cents = float(hz_to_cents(rate_d / refine_lag(f, tau), config_.a4_hz));
    }
    out = std::move(c);
    return true;
}

}  // namespace sonora::pitch
