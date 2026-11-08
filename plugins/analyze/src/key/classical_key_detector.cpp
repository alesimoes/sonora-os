// SPDX-License-Identifier: GPL-3.0-or-later
#include "key/classical_key_detector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "key/decimator.h"
#include "key/key_profiles.h"
#include "sonora/dsp/fft.h"

namespace sonora::analyze {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kFinePerSemitone = 20;  // 5-cent cells
constexpr int kFine = 12 * kFinePerSemitone;

using Detector = ClassicalKeyDetector;

// One analysis pass over the decimated signal; owns the STFT and the
// accumulators so that detect() stays a read loop.
class ChromaAccumulator {
public:
    bool prepare() {
        if (!fft_.prepare(Detector::kWindow)) return false;
        window_.resize(Detector::kWindow);
        for (int i = 0; i < Detector::kWindow; ++i)
            window_[static_cast<std::size_t>(i)] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / Detector::kWindow);
        spectrum_.resize(Detector::kWindow / 2 + 1);
        mag_.resize(spectrum_.size());
        frame_.resize(Detector::kWindow);
        const double bin_hz = Detector::kAnalysisRate / Detector::kWindow;
        first_bin_ = static_cast<int>(std::ceil(Detector::kMinHz / bin_hz));
        last_bin_ = static_cast<int>(std::floor(Detector::kMaxHz / bin_hz));
        fine_cell_.assign(spectrum_.size(), -1);
        pitch_.assign(spectrum_.size(), 0.0);
        for (int k = first_bin_; k <= last_bin_; ++k) {
            const double p = 12.0 * std::log2(k * bin_hz / 440.0);  // semitones from A4
            pitch_[static_cast<std::size_t>(k)] = p;
            const int cell = static_cast<int>(std::floor(p * kFinePerSemitone + 0.5));
            fine_cell_[static_cast<std::size_t>(k)] = ((cell % kFine) + kFine) % kFine;
        }
        return true;
    }

    // `x` is kWindow samples. Returns false for a frame under the gate.
    bool add_frame(const float* x) {
        double energy = 0.0;
        for (int i = 0; i < Detector::kWindow; ++i) {
            const double v = x[i];
            energy += v * v;
            frame_[static_cast<std::size_t>(i)] = v * window_[static_cast<std::size_t>(i)];
        }
        const double rms = std::sqrt(energy / Detector::kWindow);
        if (!(rms > std::pow(10.0, Detector::kGateDbfs / 20.0))) return false;
        fft_.forward(frame_.data(), spectrum_.data());
        double peak = 0.0;
        for (int k = first_bin_ - 1; k <= last_bin_ + 1; ++k) {
            mag_[static_cast<std::size_t>(k)] = std::abs(spectrum_[static_cast<std::size_t>(k)]);
            if (k >= first_bin_ && k <= last_bin_) peak = std::max(peak, mag_[static_cast<std::size_t>(k)]);
        }
        if (!(peak > 0.0)) return false;

        std::array<double, kFine> fine{};
        double sum = 0.0;
        for (int k = first_bin_; k <= last_bin_; ++k) {
            const double m = mag_[static_cast<std::size_t>(k)];
            fine[static_cast<std::size_t>(fine_cell_[static_cast<std::size_t>(k)])] += m;
            sum += m;
            add_tuning_vote(k, m, peak, rms);
        }
        // Per-frame normalisation then RMS weighting: a loud chord counts
        // more than a quiet one, but a spectrum's overall level does not.
        for (int c = 0; c < kFine; ++c) fine_[static_cast<std::size_t>(c)] += fine[static_cast<std::size_t>(c)] / sum * rms;
        ++frames_;
        return true;
    }

    int frames() const { return frames_; }

    double tuning_cents() const {
        if (std::abs(vote_) < 1e-12) return 0.0;
        return std::arg(vote_) * 100.0 / (2.0 * kPi);
    }

    std::array<double, 12> chroma(double tuning_cents) const {
        const int shift = static_cast<int>(std::lround(tuning_cents / (100.0 / kFinePerSemitone)));
        std::array<double, 12> out{};
        for (int s = 0; s < 12; ++s) {
            const int centre = s * kFinePerSemitone + shift;
            double v = 0.0;
            for (int j = -kFinePerSemitone / 2; j < kFinePerSemitone / 2; ++j)
                v += fine_[static_cast<std::size_t>((((centre + j) % kFine) + kFine) % kFine)];
            out[static_cast<std::size_t>((s + 9) % 12)] = v;  // semitone 0 is A: pitch class 9
        }
        return out;
    }

private:
    void add_tuning_vote(int k, double m, double peak, double rms) {
        const std::size_t i = static_cast<std::size_t>(k);
        if (m < 0.1 * peak || !(m > mag_[i - 1]) || m < mag_[i + 1]) return;
        const double a = std::log(std::max(mag_[i - 1], 1e-30));
        const double b = std::log(m);
        const double c = std::log(std::max(mag_[i + 1], 1e-30));
        const double den = a - 2.0 * b + c;
        if (!(den < 0.0)) return;
        const double delta = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
        const double bin_hz = Detector::kAnalysisRate / Detector::kWindow;
        const double p = 12.0 * std::log2((k + delta) * bin_hz / 440.0);
        const double dev = p - std::round(p);  // semitones, -0.5 .. 0.5
        vote_ += std::polar(m / peak * rms, 2.0 * kPi * dev);
    }

    dsp::RealFft fft_;
    std::vector<double> window_, frame_, mag_, pitch_;
    std::vector<std::complex<double>> spectrum_;
    std::vector<int> fine_cell_;
    std::array<double, kFine> fine_{};
    std::complex<double> vote_{0.0, 0.0};
    int first_bin_ = 0, last_bin_ = 0, frames_ = 0;
};

}  // namespace

bool ClassicalKeyDetector::detect(AudioSource& source, const RunOptions& run, KeyResult& out, std::string& error) {
    tuning_cents_ = 0.0;
    const std::uint32_t chs = source.channels();
    const std::int64_t total = source.frames();
    if (chs == 0 || !(source.sample_rate() > 0.0) || total <= 0) {
        error = "key: the source is empty";
        return false;
    }
    key::Decimator dec;
    ChromaAccumulator acc;
    if (!dec.prepare(source.sample_rate(), kAnalysisRate) || !acc.prepare()) {
        error = "key: cannot prepare the analysis";
        return false;
    }
    const std::int64_t block = std::max<std::int64_t>(run.block_frames, 1024);
    std::vector<float> inter(static_cast<std::size_t>(block) * chs), mono(static_cast<std::size_t>(block));
    std::vector<float> pending;
    pending.reserve(static_cast<std::size_t>(kWindow + block));
    std::int64_t pos = 0;
    while (pos < total) {
        if (run.cancel && run.cancel->load()) {
            error = "cancelled";
            return false;
        }
        const std::int64_t got = source.read(pos, std::min(block, total - pos), inter.data());
        if (got < 0) {
            error = "key: read error";
            return false;
        }
        if (got == 0) break;
        for (std::int64_t i = 0; i < got; ++i) {
            double s = 0.0;
            for (std::uint32_t c = 0; c < chs; ++c) s += inter[static_cast<std::size_t>(i) * chs + c];
            mono[static_cast<std::size_t>(i)] = static_cast<float>(s / chs);
        }
        pos += got;
        dec.process(mono.data(), static_cast<std::size_t>(got), pending);
        std::size_t used = 0;
        while (pending.size() - used >= static_cast<std::size_t>(kWindow)) {
            if (run.cancel && run.cancel->load()) {
                error = "cancelled";
                return false;
            }
            acc.add_frame(&pending[used]);
            used += kHop;
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(used));
        if (run.progress) run.progress(static_cast<double>(pos) / static_cast<double>(total));
    }
    if (acc.frames() == 0) {
        error = "key: no signal above -60 dBFS for a whole analysis frame (silent, or shorter than 0.75 s)";
        return false;
    }
    tuning_cents_ = acc.tuning_cents();
    const key::KeyRanking r = key::rank_keys(acc.chroma(tuning_cents_));
    out.tonic = r.tonic;
    out.mode = r.minor ? KeyMode::minor : KeyMode::major;
    out.confidence = r.confidence;
    out.method = KeyMethod::krumhansl;
    return true;
}

}  // namespace sonora::analyze
