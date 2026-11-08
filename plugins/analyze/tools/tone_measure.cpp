// SPDX-License-Identifier: GPL-3.0-or-later
#include "tone_measure.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "timbre_fit.h"

namespace sonora::analyze {
namespace {

constexpr int kStimulusSeconds = 10;
constexpr double kStimulusRmsDb = -20.0;
constexpr double kFlatnessFromHz = 2000.0;

// xorshift32: integer-only, so the same noise on every compiler.
class Xorshift32 {
public:
    double next() noexcept {  // uniform in [-1, 1)
        s_ ^= s_ << 13;
        s_ ^= s_ >> 17;
        s_ ^= s_ << 5;
        return static_cast<double>(s_) / 2147483648.0 - 1.0;
    }

private:
    std::uint32_t s_ = 0x5EED0001u;
};

bool cancelled(const RunOptions& run) { return run.cancel && run.cancel->load(); }

// Peak and mean square over every sample of every channel.
bool scan_levels(AudioSource& src, const RunOptions& run, double& peak, double& mean_square, std::string& error) {
    const std::uint32_t ch = std::max<std::uint32_t>(1, src.channels());
    const std::int64_t total = src.frames();
    const std::int64_t block = std::max<std::int64_t>(1, run.block_frames);
    std::vector<float> buf(static_cast<std::size_t>(block * ch));
    double sq = 0.0;
    peak = 0.0;
    for (std::int64_t at = 0; at < total;) {
        if (cancelled(run)) {
            error = "cancelled";
            return false;
        }
        const std::int64_t n = src.read(at, std::min(block, total - at), buf.data());
        if (n < 0) {
            error = "cannot read the audio";
            return false;
        }
        if (n == 0) break;
        for (std::int64_t i = 0; i < n * static_cast<std::int64_t>(ch); ++i) {
            const double v = buf[static_cast<std::size_t>(i)];
            peak = std::max(peak, std::fabs(v));
            sq += v * v;
        }
        at += n;
    }
    mean_square = total > 0 ? sq / (static_cast<double>(total) * ch) : 0.0;
    return true;
}

double flatness_above(const ToneProfile& p) {
    double log_sum = 0.0, sum = 0.0;
    int n = 0;
    for (int b = 0; b < kToneBands; ++b) {
        if (!p.valid[b] || kThirdOctaveCentreHz[b] < kFlatnessFromHz) continue;
        const double power = std::pow(10.0, p.bands_db[b] / 10.0);
        log_sum += std::log(power);
        sum += power;
        ++n;
    }
    if (n == 0 || sum <= 0.0) return 0.0;
    return std::min(1.0, std::exp(log_sum / n) / (sum / n));
}

}  // namespace

std::vector<float> tone_stimulus() {
    const std::size_t n = static_cast<std::size_t>(kToneStimulusRate) * kStimulusSeconds;
    std::vector<double> x(n);
    Xorshift32 rng;
    // Paul Kellett's refined pink filter (musicdsp.org, 2000s): -3 dB/octave
    // to within 0.05 dB from 9 Hz to Nyquist at 44.1 kHz; close enough at 48.
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    double sq = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = rng.next();
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        x[i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        sq += x[i] * x[i];
    }
    const double gain = std::pow(10.0, kStimulusRmsDb / 20.0) / std::sqrt(sq / static_cast<double>(n));
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(x[i] * gain);
    return out;
}

bool measure_tone_profile(AudioSource& source, const RunOptions& run, ToneProfile& out, std::string& error) {
    double peak = 0.0, ms = 0.0;
    if (!scan_levels(source, run, peak, ms, error)) return false;
    TimbreCurve curve;
    WelchTimbreMeasurer measurer;
    if (!measurer.measure(source, run, curve, error)) return false;

    ToneProfile p;
    double mean = 0.0;
    int valid = 0;
    for (int b = 0; b < kToneBands; ++b) {
        p.valid[b] = curve.valid[b];
        if (!curve.valid[b]) continue;
        mean += curve.gain_db[b];
        ++valid;
    }
    if (valid > 0) mean /= valid;
    for (int b = 0; b < kToneBands; ++b) p.bands_db[b] = curve.valid[b] ? curve.gain_db[b] - mean : 0.0;
    p.crest_db = ms > 0.0 ? std::max(0.0, 20.0 * std::log10(peak / std::sqrt(ms))) : 0.0;
    p.flatness = flatness_above(p);
    out = p;
    return true;
}

}  // namespace sonora::analyze
