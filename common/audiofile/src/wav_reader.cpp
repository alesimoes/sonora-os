// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/wav_reader.h"

#include <cerrno>
#include <climits>
#include <cstring>

namespace sonora {
namespace {

uint16_t u16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t u32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

constexpr uint16_t kPcm = 1;
constexpr uint16_t kFloat = 3;
constexpr uint16_t kExtensible = 0xFFFE;

}  // namespace

WavReader::~WavReader() { close(); }

void WavReader::close() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
}

bool WavReader::open(const std::string& path, std::string& error) {
    close();
    time_reference_ = 0;
    file_ = std::fopen(path.c_str(), "rb");
    if (!file_) {
        error = path + ": " + std::strerror(errno);
        return false;
    }
    const auto fail = [&](const char* why) {
        error = path + ": " + why;
        close();
        return false;
    };

    unsigned char head[12];
    if (std::fread(head, 1, 12, file_) != 12 || std::memcmp(head, "RIFF", 4) != 0 ||
        std::memcmp(head + 8, "WAVE", 4) != 0) {
        return fail("not a RIFF/WAVE file");
    }
    bool have_fmt = false;
    bool have_data = false;
    uint64_t data_bytes = 0;
    uint64_t at = 12;
    unsigned char ck[8];
    while (!have_data && std::fread(ck, 1, 8, file_) == 8) {
        const uint32_t size = u32(ck + 4);
        const uint64_t body = at + 8;
        if (std::memcmp(ck, "fmt ", 4) == 0) {
            unsigned char fmt[40] = {};
            const uint32_t n = size < sizeof fmt ? size : sizeof fmt;
            if (n < 16 || std::fread(fmt, 1, n, file_) != n) return fail("short fmt chunk");
            uint16_t tag = u16(fmt);
            channels_ = u16(fmt + 2);
            sample_rate_ = u32(fmt + 4);
            const uint16_t bits = u16(fmt + 14);
            // The sub-format's first two bytes are the plain format tag.
            if (tag == kExtensible) {
                if (n < 26) return fail("short WAVE_FORMAT_EXTENSIBLE fmt chunk");
                tag = u16(fmt + 24);
            }
            if (tag == kPcm && bits == 16) {
                encoding_ = Encoding::Pcm16;
            } else if (tag == kPcm && bits == 24) {
                encoding_ = Encoding::Pcm24;
            } else if (tag == kFloat && bits == 32) {
                encoding_ = Encoding::Float32;
            } else {
                return fail("unsupported sample format (16- or 24-bit PCM, or 32-bit float)");
            }
            if (channels_ == 0) return fail("no channels");
            bytes_per_sample_ = bits / 8;
            have_fmt = true;
        } else if (std::memcmp(ck, "bext", 4) == 0 && size >= 346) {
            unsigned char bext[346];
            if (std::fread(bext, 1, sizeof bext, file_) != sizeof bext) {
                return fail("short bext chunk");
            }
            time_reference_ = u32(bext + 338) | (static_cast<uint64_t>(u32(bext + 342)) << 32);
        } else if (std::memcmp(ck, "data", 4) == 0) {
            data_offset_ = body;
            data_bytes = size;
            have_data = true;
            break;
        }
        // Chunks start on even offsets.
        at = body + size + (size & 1);
        if (at > static_cast<uint64_t>(LONG_MAX) ||
            std::fseek(file_, static_cast<long>(at), SEEK_SET) != 0) {
            return fail("truncated chunk");
        }
    }
    if (!have_fmt) return fail("no fmt chunk before the data");
    if (!have_data) return fail("no data chunk");
    frames_ = static_cast<int64_t>(data_bytes / (uint64_t{bytes_per_sample_} * channels_));
    return true;
}

int64_t WavReader::read(int64_t start, int64_t count, float* interleaved) {
    if (!file_ || start < 0 || count < 0) return -1;
    if (start >= frames_) return 0;
    if (count > frames_ - start) count = frames_ - start;
    const uint64_t frame_bytes = uint64_t{bytes_per_sample_} * channels_;
    const uint64_t offset = data_offset_ + static_cast<uint64_t>(start) * frame_bytes;
    if (offset > static_cast<uint64_t>(LONG_MAX) ||
        std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) {
        return -1;
    }
    const std::size_t samples = static_cast<std::size_t>(count) * channels_;
    raw_.resize(samples * bytes_per_sample_);
    const std::size_t got = std::fread(raw_.data(), 1, raw_.size(), file_);
    const std::size_t whole = got / frame_bytes;
    const std::size_t n = whole * channels_;
    const unsigned char* p = raw_.data();
    switch (encoding_) {
        case Encoding::Pcm16:
            for (std::size_t i = 0; i < n; ++i, p += 2) {
                interleaved[i] = static_cast<float>(static_cast<int16_t>(u16(p))) / 32768.0f;
            }
            break;
        case Encoding::Pcm24:
            // The inverse of WavWriter's scale by 2^23, so a take reads back
            // as exactly the floats that were quantised into it.
            for (std::size_t i = 0; i < n; ++i, p += 3) {
                int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
                if (v & 0x800000) v -= 0x1000000;
                interleaved[i] = static_cast<float>(v) / 8388608.0f;
            }
            break;
        case Encoding::Float32:
            for (std::size_t i = 0; i < n; ++i, p += 4) {
                const uint32_t bits = u32(p);
                std::memcpy(&interleaved[i], &bits, 4);
            }
            break;
    }
    if (whole < static_cast<std::size_t>(count) && std::ferror(file_)) return -1;
    return static_cast<int64_t>(whole);
}

}  // namespace sonora
