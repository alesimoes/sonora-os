// SPDX-License-Identifier: GPL-3.0-or-later
//
// interchange.audio.rf64: a BWF or WAV export whose finished size passes the
// limit of 32-bit sizes is closed as RF64 (EBU Tech 3306); under it, the file
// stays RIFF. The limit is lowered through OutFile so no test writes 4 GiB.
// The header is checked byte by byte, and WavReader (which reads ds64) must
// agree on frames and samples.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/export_file.h"
#include "sonora/audio_export.h"
#include "sonora/wav_reader.h"

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            return 1;                                                              \
        }                                                                          \
    } while (0)

namespace {

using namespace sonora;

constexpr uint64_t kLimit = 1000;   // bytes; stands in for 0xFFFFFFFF
constexpr uint64_t kReference = 0x123456789ull;   // past 32 bits: both halves survive

bool slurp(const std::string& path, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    uint8_t b[4096];
    std::size_t n;
    out.clear();
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) out.insert(out.end(), b, b + n);
    std::fclose(f);
    return true;
}

uint64_t le(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// Walks the chunks from offset 12; returns the tag's offset or 0.
std::size_t find_chunk(const std::vector<uint8_t>& b, const char* tag) {
    std::size_t at = 12;
    while (at + 8 <= b.size()) {
        if (std::memcmp(&b[at], tag, 4) == 0) return at;
        const uint64_t n = le(&b[at + 4], 4);
        if (n == 0xFFFFFFFFull) return 0;   // data under RF64: nothing is walked past it
        at += 8 + n + (n & 1);
    }
    return 0;
}

// A 24-bit stereo ramp, `frames` long, through OutFile with the given limit.
bool write_file(const std::string& path, ExportContainer c, uint64_t frames, uint64_t limit,
                std::string& error) {
    OutFile out(limit);
    ExportMeta meta;
    meta.description = "rf64";
    meta.time_reference = kReference;
    if (!out.open(path, c, 24, 48000, 2, meta, error)) return false;
    std::vector<int32_t> il(frames * 2);
    for (uint64_t i = 0; i < frames; ++i) {
        il[i * 2] = static_cast<int32_t>(i * 1000) - 400000;
        il[i * 2 + 1] = -static_cast<int32_t>(i * 500);
    }
    if (!out.write_int(il.data(), frames, error)) return false;
    return out.close(error);
}

int test_over_limit(ExportContainer c, const std::string& path) {
    const uint64_t frames = 400;   // 2400 data bytes and the headers: over kLimit
    std::string error;
    CHECK(write_file(path, c, frames, kLimit, error));
    std::vector<uint8_t> b;
    CHECK(slurp(path, b));
    CHECK(b.size() > kLimit);
    CHECK(std::memcmp(b.data(), "RF64", 4) == 0);
    CHECK(le(&b[4], 4) == 0xFFFFFFFFull);
    CHECK(std::memcmp(&b[8], "WAVE", 4) == 0);
    CHECK(std::memcmp(&b[12], "ds64", 4) == 0);
    CHECK(le(&b[16], 4) == 28);
    CHECK(le(&b[20], 8) == b.size() - 8);   // riffSize
    CHECK(le(&b[28], 8) == frames * 6);     // dataSize
    CHECK(le(&b[36], 8) == frames);         // sampleCount
    CHECK(le(&b[44], 4) == 0);              // tableLength
    const std::size_t data = find_chunk(b, "data");
    CHECK(data != 0);
    CHECK(le(&b[data + 4], 4) == 0xFFFFFFFFull);
    CHECK(data + 8 + frames * 6 == b.size());

    WavReader r;
    CHECK(r.open(path, error));
    CHECK(r.frames() == static_cast<int64_t>(frames));
    CHECK(r.channels() == 2);
    if (c == ExportContainer::Bwf) {
        CHECK(find_chunk(b, "bext") != 0);
        CHECK(r.time_reference() == kReference);
        CHECK(r.description() == "rf64");
    }
    std::vector<float> s(frames * 2);
    CHECK(r.read(0, static_cast<int64_t>(frames), s.data()) == static_cast<int64_t>(frames));
    for (uint64_t i = 0; i < frames; ++i) {
        const double l = (static_cast<double>(i) * 1000 - 400000) / 8388608.0;
        const double rr = -static_cast<double>(i) * 500 / 8388608.0;
        CHECK(std::fabs(s[i * 2] - l) < 1e-6);
        CHECK(std::fabs(s[i * 2 + 1] - rr) < 1e-6);
    }
    r.close();
    std::remove(path.c_str());
    return 0;
}

int test_under_limit(ExportContainer c, const std::string& path) {
    const uint64_t frames = 20;
    std::string error;
    CHECK(write_file(path, c, frames, kLimit, error));
    std::vector<uint8_t> b;
    CHECK(slurp(path, b));
    CHECK(b.size() <= kLimit);
    CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
    CHECK(le(&b[4], 4) == b.size() - 8);
    // The reserved room stays a JUNK chunk: 8 + 28 bytes, zeroes.
    CHECK(std::memcmp(&b[12], "JUNK", 4) == 0);
    CHECK(le(&b[16], 4) == 28);
    for (std::size_t i = 0; i < 28; ++i) CHECK(b[20 + i] == 0);
    const std::size_t data = find_chunk(b, "data");
    CHECK(data != 0);
    CHECK(le(&b[data + 4], 4) == frames * 6);
    WavReader r;
    CHECK(r.open(path, error));
    CHECK(r.frames() == static_cast<int64_t>(frames));
    if (c == ExportContainer::Bwf) CHECK(r.time_reference() == kReference);
    r.close();
    std::remove(path.c_str());
    return 0;
}

// A file exactly at the limit is still RIFF; one byte less of limit is RF64.
int test_boundary(const std::string& path) {
    std::string error;
    CHECK(write_file(path, ExportContainer::Wav, 1, ~uint64_t{0}, error));
    std::vector<uint8_t> b;
    CHECK(slurp(path, b));
    const uint64_t exact = b.size();
    CHECK(write_file(path, ExportContainer::Wav, 1, exact, error));
    CHECK(slurp(path, b));
    CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
    CHECK(write_file(path, ExportContainer::Wav, 1, exact - 1, error));
    CHECK(slurp(path, b));
    CHECK(std::memcmp(b.data(), "RF64", 4) == 0);
    std::remove(path.c_str());
    return 0;
}

int test_aiff_refused(const std::string& path) {
    std::string error;
    CHECK(!write_file(path, ExportContainer::Aiff, 400, kLimit, error));
    CHECK(error == path + " is over 4 GiB, which a WAV cannot hold");
    std::remove(path.c_str());
    // Under the limit an AIFF is written as before.
    CHECK(write_file(path, ExportContainer::Aiff, 20, kLimit, error));
    std::remove(path.c_str());
    return 0;
}

// export_audio with the real limit: a small file is RIFF with the JUNK room.
int test_export_audio_stays_riff(const std::string& dir) {
    const std::string src = dir + "/rf64_src.wav", dst = dir + "/rf64_dst.wav";
    std::string error;
    CHECK(write_file(src, ExportContainer::Wav, 100, 0xFFFFFFFFull, error));
    ExportSpec spec;
    spec.container = ExportContainer::Bwf;
    spec.bits = 24;
    ExportMeta meta;
    meta.time_reference = 7;
    CHECK(export_audio(src, dst, spec, meta, 0.0, error));
    std::vector<uint8_t> b;
    CHECK(slurp(dst, b));
    CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
    CHECK(std::memcmp(&b[12], "JUNK", 4) == 0);
    std::remove(src.c_str());
    std::remove(dst.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const std::string p = dir + "/rf64_test.wav";
    if (int rc = test_over_limit(ExportContainer::Bwf, p)) return rc;
    if (int rc = test_over_limit(ExportContainer::Wav, p)) return rc;
    if (int rc = test_under_limit(ExportContainer::Bwf, p)) return rc;
    if (int rc = test_under_limit(ExportContainer::Wav, p)) return rc;
    if (int rc = test_boundary(p)) return rc;
    if (int rc = test_aiff_refused(dir + "/rf64_test.aiff")) return rc;
    if (int rc = test_export_audio_stays_riff(dir)) return rc;
    std::printf("ok\n");
    return 0;
}
