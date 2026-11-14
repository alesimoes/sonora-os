// SPDX-License-Identifier: GPL-3.0-or-later
//
// The peak-envelope onset picker that stands in for the beat detector
// (ADR-0136 §4): decaying hits over a quiet floor are found within one 5 ms
// hop, counted from the region's first frame, and silence has none.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sonora/elastic/onset_source.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::elastic::PeakOnsetSource;
using sonora::stretch::Planar;

constexpr std::uint32_t kRate = 48000;

// Two channels; a 1 kHz hit decaying over 80 ms at each of `hits`.
Planar hits_at(std::int64_t frames, const std::vector<std::int64_t>& hits) {
    Planar p(2, std::vector<float>(static_cast<std::size_t>(frames), 0.0f));
    for (const std::int64_t h : hits) {
        for (std::int64_t i = 0; i < kRate / 5 && h + i < frames; ++i) {
            const double t = static_cast<double>(i) / kRate;
            const float v = static_cast<float>(0.8 * std::exp(-t / 0.08) *
                                               std::sin(2.0 * 3.14159265358979 * 1000.0 * t));
            p[0][static_cast<std::size_t>(h + i)] += v;
            p[1][static_cast<std::size_t>(h + i)] += 0.5f * v;
        }
    }
    return p;
}

int test_finds_hits() {
    // Hits at 0.25 s, 0.75 s and 1.3 s of the region, which starts 1000
    // frames into the audio (a handle before it).
    const std::int64_t first = 1000;
    const std::vector<std::int64_t> want{12000, 36000, 62400};
    std::vector<std::int64_t> at;
    for (const std::int64_t w : want) at.push_back(first + w);
    const Planar audio = hits_at(first + 96000, at);
    PeakOnsetSource picker;
    std::vector<std::int64_t> found;
    std::string why;
    CHECK(picker.find(audio, first, 96000, kRate, found, why));
    CHECK(found.size() == want.size());
    for (std::size_t i = 0; i < want.size(); ++i) {
        CHECK(std::llabs(found[i] - want[i]) <= kRate / 200);
    }
    return 0;
}

int test_silence_and_bad_input() {
    PeakOnsetSource picker;
    std::vector<std::int64_t> found{5};
    std::string why;
    const Planar quiet(1, std::vector<float>(48000, 0.0f));
    CHECK(picker.find(quiet, 0, 48000, kRate, found, why) && found.empty());
    CHECK(!picker.find(quiet, 0, 48001, kRate, found, why));
    CHECK(!picker.find(Planar{}, 0, 10, kRate, found, why));
    return 0;
}

}  // namespace

int main() {
    if (test_finds_hits() != 0) return 1;
    if (test_silence_and_bad_input() != 0) return 1;
    std::printf("elastic_onsets: ok\n");
    return 0;
}
