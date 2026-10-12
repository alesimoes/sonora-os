// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/wav_writer.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

std::vector<uint8_t> slurp(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> out(static_cast<std::size_t>(n));
    std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return out;
}

// Finds a RIFF chunk by tag, returning the offset of its payload.
long find_chunk(const std::vector<uint8_t>& b, const char tag[4]) {
    for (std::size_t i = 12; i + 8 <= b.size();) {
        uint32_t size = 0;
        std::memcpy(&size, b.data() + i + 4, 4);
        if (std::memcmp(b.data() + i, tag, 4) == 0) {
            return static_cast<long>(i + 8);
        }
        i += 8 + size + (size & 1);
    }
    return -1;
}

uint32_t read_u32(const std::vector<uint8_t>& b, long off) {
    uint32_t v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

}  // namespace

int main() {
    const std::string path = "test_out.wav";

    // --- a 24-bit mono file has the sizes and payload we expect ---
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        std::vector<float> frames{0.0f, 0.5f, -0.5f, 1.0f, -1.0f};
        CHECK(w.write(frames.data(), frames.size()));
        CHECK(w.frames_written() == 5);
        CHECK(w.close());

        const auto b = slurp(path);
        CHECK(!b.empty());
        CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
        CHECK(std::memcmp(b.data() + 8, "WAVE", 4) == 0);

        const long data = find_chunk(b, "data");
        CHECK(data > 0);
        CHECK(read_u32(b, data - 4) == 5 * 3);  // 5 frames, 3 bytes each
        // The RIFF size field must match the real file length minus 8.
        CHECK(read_u32(b, 4) == b.size() - 8);

        // Full-scale positive clamps to 0x7FFFFF, not a wrapped negative.
        const long fs = data + 3 * 3;
        CHECK(b[fs] == 0xFF && b[fs + 1] == 0xFF && b[fs + 2] == 0x7F);
        // Full-scale negative clamps to 0x800000.
        const long fsn = data + 4 * 3;
        CHECK(b[fsn] == 0x00 && b[fsn + 1] == 0x00 && b[fsn + 2] == 0x80);
    }

    // --- the bext chunk carries the timecode origin, including the high word
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        const uint64_t origin = (1ull << 32) | 2ull;
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, origin));
        CHECK(w.close());

        const auto b = slurp(path);
        const long bext = find_chunk(b, "bext");
        CHECK(bext > 0);
        CHECK(read_u32(b, bext + 338) == 2);  // low word
        CHECK(read_u32(b, bext + 342) == 1);  // high word
    }

    // --- a NaN sample becomes silence instead of garbage ---
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        const float nan_sample = std::numeric_limits<float>::quiet_NaN();
        std::vector<float> frames{nan_sample, 0.5f};
        CHECK(w.write(frames.data(), frames.size()));
        CHECK(w.close());

        const auto b = slurp(path);
        const long data = find_chunk(b, "data");
        CHECK(b[data] == 0x00 && b[data + 1] == 0x00 && b[data + 2] == 0x00);
    }

    // --- float32 writes 4 bytes per sample and reports IEEE float ---
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        CHECK(w.open(path, 96000, 2, sonora::WavWriter::Format::Float32, 0));
        std::vector<float> stereo{0.25f, -0.25f, 0.5f, -0.5f};  // 2 frames
        CHECK(w.write(stereo.data(), 2));
        CHECK(w.close());

        const auto b = slurp(path);
        const long fmt = find_chunk(b, "fmt ");
        CHECK(fmt > 0);
        uint16_t audio_format = 0;
        std::memcpy(&audio_format, b.data() + fmt, 2);
        CHECK(audio_format == 3);  // IEEE float
        const long data = find_chunk(b, "data");
        CHECK(read_u32(b, data - 4) == 2 * 2 * 4);
    }

    // --- double close is a no-op, not a crash or a corrupt header ---
    {
        std::remove(path.c_str());
        sonora::WavWriter w;
        CHECK(w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        CHECK(w.close());
        CHECK(w.close());
        CHECK(!w.is_open());
    }

    // --- an existing file is never truncated ---
    //
    // open() used fopen("wb"), so a take numbered like one already on disk
    // replaced it without a word: record take 1, restart the engine, record
    // again, and the first performance was gone.
    {
        std::remove(path.c_str());
        std::FILE* f = std::fopen(path.c_str(), "wb");
        CHECK(f != nullptr);
        std::fputs("the previous take", f);
        std::fclose(f);

        sonora::WavWriter w;
        CHECK(!w.open(path, 48000, 1, sonora::WavWriter::Format::Int24, 0));
        CHECK(!w.is_open());
        const auto b = slurp(path);
        CHECK(std::string(b.begin(), b.end()) == "the previous take");
    }

    std::remove(path.c_str());
    std::printf("test_wav_writer: ok\n");
    return 0;
}
