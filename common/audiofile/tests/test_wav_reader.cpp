// SPDX-License-Identifier: GPL-3.0-or-later
//
// WavReader reads back what WavWriter wrote: the same floats for 32-bit
// float, the same quantised values for 24-bit, the BWF time reference, and
// a short read at the end of the data rather than past it.
#include "sonora/wav_reader.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sonora/wav_writer.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::WavReader;
using sonora::WavWriter;

float sample(uint32_t ch, uint32_t f) {
    return std::sin(0.01f * static_cast<float>(f) * static_cast<float>(ch + 1)) * 0.9f;
}

// The value a 24-bit file holds for `s`: WavWriter's quantisation.
float quantise24(float s) { return static_cast<float>(std::lrintf(s * 8388608.0f)) / 8388608.0f; }

int round_trip(WavWriter::Format format, uint16_t channels, uint32_t frames) {
    const std::string path = "test_wav_reader_" + std::to_string(static_cast<int>(format)) +
                             "_" + std::to_string(channels) + ".wav";
    std::remove(path.c_str());
    std::vector<float> in(static_cast<std::size_t>(frames) * channels);
    for (uint32_t f = 0; f < frames; ++f) {
        for (uint16_t c = 0; c < channels; ++c) in[f * channels + c] = sample(c, f);
    }
    {
        WavWriter w;
        CHECK(w.open(path, 48000, channels, format, 123456789012ull));
        CHECK(w.write(in.data(), frames));
        CHECK(w.close());
    }
    WavReader r;
    std::string error;
    CHECK(r.open(path, error));
    CHECK(r.channels() == channels);
    CHECK(r.sample_rate() == 48000);
    CHECK(r.frames() == frames);
    CHECK(r.time_reference() == 123456789012ull);

    std::vector<float> out(in.size(), -7.0f);
    CHECK(r.read(0, frames, out.data()) == frames);
    for (std::size_t i = 0; i < in.size(); ++i) {
        const float want = format == WavWriter::Format::Float32 ? in[i] : quantise24(in[i]);
        CHECK(out[i] == want);
    }
    // From the middle, across the end: short, and nothing written past it.
    std::vector<float> tail(static_cast<std::size_t>(100) * channels, -7.0f);
    CHECK(r.read(frames - 10, 100, tail.data()) == 10);
    CHECK(tail[0] == out[(frames - 10) * channels]);
    CHECK(tail[10 * channels] == -7.0f);
    CHECK(r.read(frames, 5, tail.data()) == 0);
    std::remove(path.c_str());
    return 0;
}

int test_refuses_what_is_not_wav() {
    const std::string path = "test_wav_reader_junk.wav";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    std::fputs("this is not a wave file at all", f);
    std::fclose(f);
    WavReader r;
    std::string error;
    CHECK(!r.open(path, error));
    CHECK(error.find("not a RIFF/WAVE file") != std::string::npos);
    CHECK(!r.is_open());
    CHECK(!r.open("test_wav_reader_absent.wav", error));
    std::remove(path.c_str());
    return 0;
}

}  // namespace

int main() {
    // Odd frame count with mono 24-bit: the data chunk is padded.
    if (round_trip(WavWriter::Format::Int24, 1, 4801)) return 1;
    if (round_trip(WavWriter::Format::Int24, 2, 4800)) return 1;
    if (round_trip(WavWriter::Format::Float32, 1, 4801)) return 1;
    if (round_trip(WavWriter::Format::Float32, 2, 11)) return 1;
    if (test_refuses_what_is_not_wav()) return 1;
    std::printf("wav_reader: ok\n");
    return 0;
}
