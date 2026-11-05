// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/analyze/mix_analysis.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "sonora/dsp/fft.h"
#include "sonora/dsp/loudness.h"
#include "sonora/dsp/math.h"

namespace sonora::analyze {

namespace {

constexpr std::uint32_t kFrame = 16384;
constexpr std::uint32_t kHop = kFrame / 2;
// A frame quieter than this (mean square of the mono sum) is silence for
// the spectrum: -70 dBFS RMS.
constexpr double kGateMeanSquare = 1e-7;
constexpr double kSqrt2 = 1.4142135623730951;

double db10(double power, double floor_power = 1e-300) { return 10.0 * std::log10(std::fmax(power, floor_power)); }

// The Welch accumulator over the mono sum.
class Spectrum {
public:
    bool prepare() {
        if (!fft_.prepare(kFrame)) return false;
        window_.resize(kFrame);
        for (std::uint32_t i = 0; i < kFrame; ++i) {
            window_[i] = 0.5 - 0.5 * std::cos(dsp::kTwoPi * i / kFrame);  // periodic Hann
        }
        buf_.assign(kFrame, 0.0);
        tmp_.assign(kFrame, 0.0);
        bins_.assign(kFrame / 2 + 1, {});
        power_.assign(kFrame / 2 + 1, 0.0);
        return true;
    }
    void push(double x) {
        buf_[fill_++] = x;
        if (fill_ < kFrame) return;
        frame();
        std::copy(buf_.begin() + kHop, buf_.end(), buf_.begin());
        fill_ = kFrame - kHop;
    }
    std::uint32_t frames() const { return frames_; }
    // Mean power in [lo, hi) Hz, summed over bins.
    double band_power(double lo, double hi, double rate) const {
        double sum = 0.0;
        for (std::uint32_t k = 0; k < power_.size(); ++k) {
            const double f = static_cast<double>(k) * rate / kFrame;
            if (f >= lo && f < hi) sum += power_[k];
        }
        return frames_ ? sum / frames_ : 0.0;
    }

private:
    void frame() {
        double ms = 0.0;
        for (std::uint32_t i = 0; i < kFrame; ++i) ms += buf_[i] * buf_[i];
        if (ms / kFrame < kGateMeanSquare) return;
        for (std::uint32_t i = 0; i < kFrame; ++i) tmp_[i] = buf_[i] * window_[i];
        fft_.forward(tmp_.data(), bins_.data());
        for (std::uint32_t k = 0; k < power_.size(); ++k) power_[k] += std::norm(bins_[k]);
        ++frames_;
    }
    dsp::RealFft fft_;
    std::vector<double> window_, buf_, tmp_, power_;
    std::vector<std::complex<double>> bins_;
    std::uint32_t fill_ = 0, frames_ = 0;
};

}  // namespace

double band_lower_hz(int band) noexcept { return kBandCentreHz[band] / kSqrt2; }
double band_upper_hz(int band) noexcept { return kBandCentreHz[band] * kSqrt2; }

std::int64_t MemorySource::read(std::int64_t start, std::int64_t count, float* out) {
    const std::int64_t n = frames();
    if (start < 0 || start > n) return -1;
    const std::int64_t m = std::min(count, n - start);
    const std::size_t chs = x_.size();
    for (std::int64_t i = 0; i < m; ++i) {
        for (std::size_t c = 0; c < chs; ++c) out[i * static_cast<std::int64_t>(chs) + static_cast<std::int64_t>(c)] =
            x_[c][static_cast<std::size_t>(start + i)];
    }
    return m;
}

bool analyse_mix(AudioSource& source, const RunOptions& options, MixAnalysis& out, std::string& error) {
    out = MixAnalysis{};
    const std::uint32_t in_ch = source.channels();
    const double rate = source.sample_rate();
    const std::int64_t total = source.frames();
    if (in_ch == 0 || !(rate >= 8000.0) || total <= 0) {
        error = "the file has no audio to analyse";
        return false;
    }
    if (total < static_cast<std::int64_t>(kFrame)) {
        error = "the file is shorter than 16384 samples: too short to measure a spectrum";
        return false;
    }
    const std::uint32_t chs = std::min<std::uint32_t>(in_ch, 2);
    dsp::LoudnessMeter meter;
    if (!meter.prepare(rate, chs, nullptr, true)) {
        error = "unsupported sample rate for the loudness meter";
        return false;
    }
    Spectrum spec;
    if (!spec.prepare()) {
        error = "could not set up the spectrum";
        return false;
    }
    const std::uint32_t block = std::max<std::uint32_t>(options.block_frames, 1024);
    std::vector<float> inter(static_cast<std::size_t>(block) * in_ch);
    std::vector<float> left(block), right(block);

    double peak = 0.0, sum_sq = 0.0, sum_ll = 0.0, sum_rr = 0.0, sum_lr = 0.0, sum_m = 0.0, sum_s = 0.0;
    for (std::int64_t pos = 0; pos < total;) {
        if (options.cancel != nullptr && options.cancel->load(std::memory_order_relaxed)) {
            error = "cancelled";
            return false;
        }
        const std::int64_t got = source.read(pos, block, inter.data());
        if (got < 0) {
            error = "could not read the audio";
            return false;
        }
        if (got == 0) break;
        for (std::int64_t i = 0; i < got; ++i) {
            const std::size_t o = static_cast<std::size_t>(i) * in_ch;
            // A non-finite sample is a corrupt file, not a signal: measured
            // as silence so that one cannot poison the sums.
            double l = std::isfinite(inter[o]) ? inter[o] : 0.0f;
            double r = chs > 1 ? (std::isfinite(inter[o + 1]) ? inter[o + 1] : 0.0f) : l;
            left[static_cast<std::size_t>(i)] = static_cast<float>(l);
            right[static_cast<std::size_t>(i)] = static_cast<float>(r);
            peak = std::fmax(peak, std::fmax(std::fabs(l), chs > 1 ? std::fabs(r) : 0.0));
            sum_sq += l * l + (chs > 1 ? r * r : 0.0);
            sum_ll += l * l;
            sum_rr += r * r;
            sum_lr += l * r;
            const double m = 0.5 * (l + r), s = 0.5 * (l - r);
            sum_m += m * m;
            sum_s += s * s;
            spec.push(m);
        }
        const float* p[2] = {left.data(), right.data()};
        meter.process(p, static_cast<std::uint32_t>(got));
        pos += got;
        if (options.progress) options.progress(static_cast<double>(pos) / static_cast<double>(total));
    }

    out.sample_rate = rate;
    out.frames = static_cast<std::uint64_t>(total);
    out.channels = chs;
    out.duration_s = static_cast<double>(total) / rate;
    out.integrated_lufs = meter.integrated_lufs();
    out.max_short_term_lufs = meter.max_short_term_lufs();
    out.loudness_range_lu = meter.loudness_range_lu();
    out.true_peak_dbtp = meter.true_peak_dbtp();
    out.sample_peak_dbfs = dsp::gain_to_db(peak);
    out.rms_dbfs = db10(sum_sq / (static_cast<double>(total) * chs), 1e-20);
    out.crest_db = out.sample_peak_dbfs - out.rms_dbfs;
    out.plr_db = out.true_peak_dbtp - out.integrated_lufs;
    if (chs > 1 && sum_ll > 0.0 && sum_rr > 0.0) out.correlation = sum_lr / std::sqrt(sum_ll * sum_rr);
    out.side_mid_db = chs > 1 ? std::fmax(-120.0, db10(sum_s + 1e-20) - db10(sum_m + 1e-20)) : -120.0;

    out.spectrum_frames = spec.frames();
    if (out.spectrum_frames == 0 || out.integrated_lufs < -70.0) {
        error = "the mix is silent (nothing above -70 dBFS): nothing to analyse";
        return false;
    }
    for (int b = 0; b < kBands; ++b) {
        out.band_valid[b] = band_lower_hz(b) < 0.9 * rate * 0.5;
        if (out.band_valid[b]) out.band_db[b] = db10(spec.band_power(band_lower_hz(b), band_upper_hz(b), rate));
    }
    return true;
}

}  // namespace sonora::analyze
