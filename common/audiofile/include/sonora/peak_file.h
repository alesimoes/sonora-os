// SPDX-License-Identifier: GPL-3.0-or-later
//
// The overview file beside a take: for each channel the minimum and maximum
// of every run of 256 frames, and the same folded by 4 five more times, so a
// window can ask for any zoom from a few frames a pixel to a whole hour
// without reading the audio (ui.editing.waveform). Written off the audio
// thread: by the control thread when a take closes, and by the interface's
// own worker for a source that has none (an import, a file from before this
// existed). Read whole into memory by the interface.
//
// Layout, little endian: "SPK1", u32 version (1), u32 channels, u32 base
// (256), u32 factor (4), u32 levels, i64 source frames; then, level by level
// and channel by channel, ceil(frames / (base * factor^level)) pairs of i16
// (min, max), full scale 32767. A file whose frame count is not the
// source's is stale, and is rebuilt rather than drawn.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sonora {

inline constexpr uint32_t kPeakBase = 256;
inline constexpr uint32_t kPeakFactor = 4;
inline constexpr uint32_t kPeakLevels = 6;

// "<session_dir>/Peaks/<source_path>.spk"; `source_path` is the Source's,
// relative to the session folder.
std::string peak_file_path(const std::string& session_dir, const std::string& source_path);

class PeakBuilder {
public:
    explicit PeakBuilder(uint16_t channels);
    // Interleaved frames, in order.
    void add(const float* interleaved, int64_t frames);
    int64_t frames() const { return frames_; }
    // Atomically: written beside, then renamed over `path`. Creates the
    // folder. False, with `error`, when it cannot.
    bool write(const std::string& path, std::string& error) const;

private:
    uint16_t channels_;
    int64_t frames_ = 0;
    // Per channel: finished level-0 buckets, and the one being filled.
    std::vector<std::vector<int16_t>> level0_;
    std::vector<float> lo_, hi_;
    uint32_t in_bucket_ = 0;
};

// Reads `wav` through and writes `peak` for it.
bool build_peak_file(const std::string& wav, const std::string& peak, std::string& error);

class PeakFile {
public:
    bool open(const std::string& path, std::string& error);
    uint16_t channels() const { return channels_; }
    int64_t frames() const { return frames_; }
    // `count` buckets of `frames_per_bucket` frames from `first`, as -1..1
    // floats; outside the source is 0. Answers from memory.
    void read(uint16_t channel, int64_t first, double frames_per_bucket, int count, float* min,
              float* max) const;

private:
    uint16_t channels_ = 0;
    int64_t frames_ = 0;
    // [level][channel] -> pairs
    std::vector<std::vector<std::vector<int16_t>>> data_;
};

}  // namespace sonora
