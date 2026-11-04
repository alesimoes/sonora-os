// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whole-mix analysis (ADR-0011 N4, ADR-0046): what the mastering
// suggestion pass reads from a bounce of the mix. It measures, and decides
// nothing; mastering_rules.h turns the figures into settings.
//
// - Loudness and true peak: common/dsp's meter, ITU-R BS.1770-5 and
//   EBU Tech 3341/3342 (integrated with both gates, short-term maximum,
//   loudness range, 4x true peak).
// - Crest factor: sample peak over RMS, over all channels.
// - Stereo: the L/R correlation coefficient, and the side-to-mid energy
//   ratio (M = (L + R) / 2, S = (L - R) / 2). A one-channel file has
//   correlation 1 and no side.
// - Spectral balance: the long-term mean power of the mono sum in
//   octave bands (31.5 Hz to 16 kHz), by Welch's method (P. D. Welch,
//   IEEE Trans. Audio Electroacoust. 15(2), 1967): Hann-windowed
//   16384-point frames at 50 % overlap, frames below
//   -70 dBFS RMS left out so that silence does not pull the average down.
//
// Offline: it allocates, reads a file in blocks and can take seconds on a
// long bounce. It runs in sonora-analyze, never on an audio thread, and
// stops between blocks when asked.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace sonora::analyze {

constexpr int kBands = 10;
constexpr double kBandCentreHz[kBands] = {31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
// An octave band spans centre / sqrt(2) to centre * sqrt(2); the bands tile
// the spectrum with no gap or overlap.
double band_lower_hz(int band) noexcept;
double band_upper_hz(int band) noexcept;

struct MixAnalysis {
    double sample_rate = 0.0;
    std::uint64_t frames = 0;
    std::uint32_t channels = 0;
    double duration_s = 0.0;

    double integrated_lufs = -200.0;
    double max_short_term_lufs = -200.0;
    double loudness_range_lu = 0.0;
    double true_peak_dbtp = -200.0;
    double sample_peak_dbfs = -200.0;
    double rms_dbfs = -200.0;
    double crest_db = 0.0;  // sample peak over RMS
    double plr_db = 0.0;    // true peak over integrated loudness

    double correlation = 1.0;
    double side_mid_db = -120.0;

    // dB, relative: only the differences between bands mean anything.
    // band_valid is false for a band whose lower edge is too near Nyquist
    // to hold bins (a 44.1 kHz file still has all ten; an 8 kHz one has not).
    double band_db[kBands] = {};
    bool band_valid[kBands] = {};
    std::uint32_t spectrum_frames = 0;
};

// Where audio comes from, so the analysis reads a WAV file in the app and
// memory in a test. Not thread-safe; one reader at a time.
class AudioSource {
public:
    virtual ~AudioSource() = default;
    virtual std::uint32_t channels() const = 0;
    virtual double sample_rate() const = 0;
    virtual std::int64_t frames() const = 0;
    // Up to `count` interleaved frames from `start`; the number read, 0 at
    // the end, negative on an error.
    virtual std::int64_t read(std::int64_t start, std::int64_t count, float* interleaved) = 0;
};

class MemorySource final : public AudioSource {
public:
    MemorySource(const std::vector<std::vector<float>>& planar, double sample_rate)
        : x_(planar), rate_(sample_rate) {}
    std::uint32_t channels() const override { return static_cast<std::uint32_t>(x_.size()); }
    double sample_rate() const override { return rate_; }
    std::int64_t frames() const override { return x_.empty() ? 0 : static_cast<std::int64_t>(x_[0].size()); }
    std::int64_t read(std::int64_t start, std::int64_t count, float* out) override;

private:
    const std::vector<std::vector<float>>& x_;
    double rate_;
};

struct RunOptions {
    // Polled between blocks; true stops the run with error "cancelled".
    const std::atomic<bool>* cancel = nullptr;
    // Called between blocks with the fraction done, 0 to 1.
    std::function<void(double)> progress;
    std::uint32_t block_frames = 16384;
};

// Reads the whole source in blocks and measures it. False, with `error`,
// for a source that is empty, has no channel, is shorter than one spectrum
// frame (16384 samples), is silent (nothing above -70 dBFS), fails to read,
// or was cancelled. Only the first two channels are measured.
bool analyse_mix(AudioSource& source, const RunOptions& options, MixAnalysis& out, std::string& error);

}  // namespace sonora::analyze
