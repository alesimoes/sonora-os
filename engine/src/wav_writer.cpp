// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/wav_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace sonora {
namespace {

void put_u16(std::FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }
void put_u32(std::FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
void put_tag(std::FILE* f, const char tag[4]) { std::fwrite(tag, 1, 4, f); }

int32_t float_to_int24(float s) {
    // Clamp before scaling: a sample above 1.0 must pin to full scale rather
    // than wrap around to the opposite polarity.
    s = std::max(-1.0f, std::min(1.0f, s));
    const float scaled = s * 8388607.0f;
    return static_cast<int32_t>(std::lrintf(scaled));
}

constexpr uint32_t kBextSize = 602;  // fixed-size BWF v0 bext chunk

}  // namespace

WavWriter::~WavWriter() { close(); }

bool WavWriter::open(const std::string& path, uint32_t sample_rate,
                     uint16_t channels, Format format,
                     uint64_t time_reference) {
    if (file_) return false;
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) return false;
    path_ = path;
    format_ = format;
    channels_ = channels;
    frames_written_ = 0;
    if (!write_header(sample_rate, channels, time_reference)) {
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    return true;
}

bool WavWriter::write_header(uint32_t sample_rate, uint16_t channels,
                             uint64_t time_reference) {
    const uint16_t bits = (format_ == Format::Int24) ? 24 : 32;
    const uint16_t audio_format = (format_ == Format::Int24) ? 1 : 3;  // PCM/IEEE float
    const uint16_t block_align = channels * (bits / 8);

    put_tag(file_, "RIFF");
    riff_size_offset_ = std::ftell(file_);
    put_u32(file_, 0);  // patched in close()
    put_tag(file_, "WAVE");

    // --- bext ---
    put_tag(file_, "bext");
    put_u32(file_, kBextSize);
    std::vector<char> bext(kBextSize, 0);
    const char* originator = "Sonora Engine";
    std::memcpy(bext.data() + 256, originator, std::strlen(originator));
    const uint32_t tr_low = static_cast<uint32_t>(time_reference & 0xFFFFFFFFu);
    const uint32_t tr_high = static_cast<uint32_t>(time_reference >> 32);
    std::memcpy(bext.data() + 338, &tr_low, 4);
    std::memcpy(bext.data() + 342, &tr_high, 4);
    const uint16_t bwf_version = 0;
    std::memcpy(bext.data() + 346, &bwf_version, 2);
    std::fwrite(bext.data(), 1, bext.size(), file_);

    // --- fmt ---
    put_tag(file_, "fmt ");
    put_u32(file_, 16);
    put_u16(file_, audio_format);
    put_u16(file_, channels);
    put_u32(file_, sample_rate);
    put_u32(file_, sample_rate * block_align);  // byte rate
    put_u16(file_, block_align);
    put_u16(file_, bits);

    // --- data ---
    put_tag(file_, "data");
    data_size_offset_ = std::ftell(file_);
    put_u32(file_, 0);  // patched in close()
    return std::ferror(file_) == 0;
}

bool WavWriter::write(const float* interleaved, std::size_t frames) {
    if (!file_ || frames == 0) return file_ != nullptr;
    const std::size_t samples = frames * channels_;

    if (format_ == Format::Float32) {
        if (std::fwrite(interleaved, sizeof(float), samples, file_) != samples) {
            return false;
        }
    } else {
        std::vector<uint8_t> packed(samples * 3);
        for (std::size_t i = 0; i < samples; ++i) {
            const int32_t v = float_to_int24(interleaved[i]);
            packed[i * 3 + 0] = static_cast<uint8_t>(v & 0xFF);
            packed[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            packed[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        }
        if (std::fwrite(packed.data(), 1, packed.size(), file_) != packed.size()) {
            return false;
        }
    }
    frames_written_ += frames;
    return true;
}

bool WavWriter::close() {
    if (!file_) return true;
    const uint16_t bits = (format_ == Format::Int24) ? 24 : 32;
    const uint64_t data_bytes = frames_written_ * channels_ * (bits / 8);

    std::fseek(file_, data_size_offset_, SEEK_SET);
    put_u32(file_, static_cast<uint32_t>(data_bytes));
    std::fseek(file_, riff_size_offset_, SEEK_SET);
    // RIFF size counts everything after this field.
    put_u32(file_, static_cast<uint32_t>(4 + (8 + kBextSize) + (8 + 16) + 8 + data_bytes));

    const bool ok = std::ferror(file_) == 0;
    std::fclose(file_);
    file_ = nullptr;
    return ok;
}

}  // namespace sonora
