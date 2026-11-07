// SPDX-License-Identifier: GPL-3.0-or-later
//
// The overview file: what a builder writes, a PeakFile reads back at every
// zoom, a WAV on disk gives the same file, and a file that is not ours is
// refused rather than drawn.
#include "sonora/peak_file.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/wav_writer.h"

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            return 1;                                                                    \
        }                                                                                \
    } while (0)

namespace {

using sonora::PeakBuilder;
using sonora::PeakFile;

// Channel 0 is silent except one spike of 0.8 at frame 10000 and a dip of
// -0.5 at frame 300000; channel 1 is a constant 0.25.
constexpr int64_t kFrames = 400000;

float sample(int ch, int64_t f) {
    if (ch == 1) return 0.25f;
    if (f == 10000) return 0.8f;
    if (f == 300000) return -0.5f;
    return 0.0f;
}

std::vector<float> signal() {
    std::vector<float> in(static_cast<std::size_t>(kFrames) * 2);
    for (int64_t f = 0; f < kFrames; ++f) {
        in[static_cast<std::size_t>(f) * 2] = sample(0, f);
        in[static_cast<std::size_t>(f) * 2 + 1] = sample(1, f);
    }
    return in;
}

bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

int check_file(const PeakFile& p) {
    CHECK(p.channels() == 2);
    CHECK(p.frames() == kFrames);
    float lo[8], hi[8];
    // Fine zoom: a bucket of 256 frames holds the spike, the next one not.
    p.read(0, 9984, 256, 2, lo, hi);
    CHECK(near(hi[0], 0.8f) && near(lo[0], 0.0f));
    CHECK(near(hi[1], 0.0f));
    // Coarse zoom (65536 frames a bucket) still reaches both extremes.
    p.read(0, 0, 65536, 7, lo, hi);
    CHECK(near(hi[0], 0.8f));
    CHECK(near(lo[4], -0.5f) && near(hi[4], 0.0f));   // frame 300000 is bucket 4
    CHECK(near(hi[6], 0.0f));                          // 393216.. holds nothing
    // A whole-file bucket.
    p.read(0, 0, 1 << 20, 1, lo, hi);
    CHECK(near(hi[0], 0.8f) && near(lo[0], -0.5f));
    // The other channel, and a bucket not aligned to the base.
    p.read(1, 1000, 100, 1, lo, hi);
    CHECK(near(lo[0], 0.25f) && near(hi[0], 0.25f));
    // Outside the source is silence, never a clamp to the last bucket.
    p.read(0, kFrames + 5000, 256, 1, lo, hi);
    CHECK(lo[0] == 0.0f && hi[0] == 0.0f);
    p.read(0, -100000, 256, 1, lo, hi);
    CHECK(lo[0] == 0.0f && hi[0] == 0.0f);
    // A channel the file does not have.
    p.read(5, 0, 256, 1, lo, hi);
    CHECK(lo[0] == 0.0f && hi[0] == 0.0f);
    return 0;
}

}  // namespace

int main() {
    const std::string path = "test_peak_file.spk";
    std::remove(path.c_str());
    std::string error;
    {
        PeakBuilder b(2);
        const std::vector<float> in = signal();
        // In odd-sized pieces, so a bucket straddles two add() calls.
        int64_t at = 0;
        for (int64_t n : {1000, 77777, 123, 300000}) {
            const int64_t m = std::min<int64_t>(n, kFrames - at);
            b.add(in.data() + static_cast<std::size_t>(at) * 2, m);
            at += m;
        }
        b.add(in.data() + static_cast<std::size_t>(at) * 2, kFrames - at);
        CHECK(b.frames() == kFrames);
        CHECK(b.write(path, error));
    }
    PeakFile p;
    CHECK(p.open(path, error));
    if (check_file(p) != 0) return 1;

    // The same file from a WAV on disk.
    const std::string wav = "test_peak_file.wav";
    const std::string from_wav = "test_peak_file_wav.spk";
    std::remove(wav.c_str());
    {
        sonora::WavWriter w;
        CHECK(w.open(wav, 48000, 2, sonora::WavWriter::Format::Float32, 0));
        const std::vector<float> in = signal();
        CHECK(w.write(in.data(), static_cast<std::size_t>(kFrames)));
        CHECK(w.close());
    }
    CHECK(sonora::build_peak_file(wav, from_wav, error));
    PeakFile q;
    CHECK(q.open(from_wav, error));
    if (check_file(q) != 0) return 1;

    // Refused: missing, and not ours.
    PeakFile bad;
    CHECK(!bad.open("test_peak_file_missing.spk", error) && !error.empty());
    {
        std::FILE* f = std::fopen("test_peak_file_junk.spk", "wb");
        CHECK(f != nullptr);
        std::fputs("RIFFnotapeakfile", f);
        std::fclose(f);
    }
    CHECK(!bad.open("test_peak_file_junk.spk", error));
    float lo, hi;
    bad.read(0, 0, 256, 1, &lo, &hi);   // an unopened file reads as silence
    CHECK(lo == 0.0f && hi == 0.0f);

    // The path convention.
    CHECK(sonora::peak_file_path("/s", "Audio Files/a.wav") == "/s/Peaks/Audio Files/a.wav.spk");
    std::remove(path.c_str());
    std::remove(wav.c_str());
    std::remove(from_wav.c_str());
    std::remove("test_peak_file_junk.spk");
    return 0;
}
