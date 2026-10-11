// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace sonora {

// Broadcast Wave (BWF) writer. Writes a bext chunk so the file carries its
// own timecode origin, which is what every other DAW reads on import.
//
// All writes happen on the disk thread, never on the audio callback.
class WavWriter {
public:
    enum class Format { Int24, Float32 };

    WavWriter() = default;
    ~WavWriter();

    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    // time_reference is the sample position of the first frame relative to
    // midnight, as BWF defines it.
    bool open(const std::string& path, uint32_t sample_rate, uint16_t channels,
              Format format, uint64_t time_reference);

    // Writes interleaved float frames, converting to the on-disk format.
    bool write(const float* interleaved, std::size_t frames);

    // Patches the RIFF/data sizes and closes. Safe to call twice.
    bool close();

    bool is_open() const { return file_ != nullptr; }
    uint64_t frames_written() const { return frames_written_; }
    const std::string& path() const { return path_; }

private:
    bool write_header(uint32_t sample_rate, uint16_t channels,
                      uint64_t time_reference);

    std::FILE* file_ = nullptr;
    std::string path_;
    Format format_ = Format::Int24;
    uint16_t channels_ = 0;
    uint64_t frames_written_ = 0;
    long data_size_offset_ = 0;
    long riff_size_offset_ = 0;
};

}  // namespace sonora
