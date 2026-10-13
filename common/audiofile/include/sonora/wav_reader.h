// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace sonora {

// Reads the WAV and BWF files the engine plays back: 16- and 24-bit PCM and
// 32-bit float, plain or WAVE_FORMAT_EXTENSIBLE, with or without a bext
// chunk. Converts to float on the way out.
//
// Used by the playback disk reader, never on the audio callback. One thread
// at a time: reads seek a shared FILE*.
class WavReader {
public:
    enum class Encoding { Pcm16, Pcm24, Float32 };

    WavReader() = default;
    ~WavReader();

    WavReader(const WavReader&) = delete;
    WavReader& operator=(const WavReader&) = delete;

    // On failure `error` says what is wrong with the file, and the reader
    // is closed.
    bool open(const std::string& path, std::string& error);
    void close();

    bool is_open() const { return file_ != nullptr; }
    uint16_t channels() const { return channels_; }
    uint32_t sample_rate() const { return sample_rate_; }
    int64_t frames() const { return frames_; }
    Encoding encoding() const { return encoding_; }
    // BWF TimeReference, the timeline sample of the first frame; 0 without
    // a bext chunk.
    uint64_t time_reference() const { return time_reference_; }

    // Up to `count` interleaved frames from frame `start`, as float. Returns
    // how many were read: short at the end of the data, -1 on a read error.
    int64_t read(int64_t start, int64_t count, float* interleaved);

private:
    std::FILE* file_ = nullptr;
    uint16_t channels_ = 0;
    uint32_t sample_rate_ = 0;
    int64_t frames_ = 0;
    Encoding encoding_ = Encoding::Pcm24;
    uint32_t bytes_per_sample_ = 3;
    uint64_t data_offset_ = 0;
    uint64_t time_reference_ = 0;
    std::vector<unsigned char> raw_;
};

}  // namespace sonora
