// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Rubber Band adapter (ADR-0136 §3, Unverified 3), built only with the
// library: for every algorithm the output is exactly the map's length, with
// markers at the input's first and last frames too, it is not silent, and a
// cancel stops it.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "sonora/elastic/library_stretcher.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using namespace sonora;

constexpr std::uint32_t kRate = 48000;

stretch::Planar tone(std::int64_t frames) {
    stretch::Planar p(2, std::vector<float>(static_cast<std::size_t>(frames)));
    for (std::int64_t i = 0; i < frames; ++i) {
        const float v = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979 * 440.0 * i / kRate));
        p[0][static_cast<std::size_t>(i)] = v;
        p[1][static_cast<std::size_t>(i)] = -v;
    }
    return p;
}

double rms(const std::vector<float>& x) {
    double e = 0.0;
    for (const float v : x) e += static_cast<double>(v) * v;
    return std::sqrt(e / static_cast<double>(x.size()));
}

int test_exact_length() {
    const elastic::StretcherFactory make = elastic::library_stretcher();
    CHECK(static_cast<bool>(make));
    const stretch::Planar in = tone(kRate);
    const std::vector<std::vector<stretch::Marker>> maps{
        {{kRate - 1, kRate * 3 / 2 - 1}},                  // ratio 1.5, marker on the last frame
        {{1, 1}, {kRate / 2, kRate / 4}},                  // first frame, then twice as fast
        {{12000, 30000}, {36000, 40000}},                  // two markers, the rest at speed
    };
    for (const auto& markers : maps) {
        stretch::WarpMap map;
        std::string why;
        CHECK(map.set(kRate, markers, why));
        for (const stretch::Algorithm a :
             {stretch::Algorithm::Polyphonic, stretch::Algorithm::Rhythmic,
              stretch::Algorithm::Monophonic, stretch::Algorithm::Varispeed}) {
            std::atomic<bool> cancel{false};
            std::unique_ptr<stretch::Stretcher> s = make(kRate, cancel);
            stretch::Planar out;
            CHECK(s->process(in, map, a, out, why));
            CHECK(out.size() == 2);
            CHECK(static_cast<std::int64_t>(out[0].size()) == map.output_length());
            CHECK(static_cast<std::int64_t>(out[1].size()) == map.output_length());
            CHECK(rms(out[0]) > 0.1);
        }
    }
    return 0;
}

int test_cancel() {
    std::atomic<bool> cancel{true};
    std::unique_ptr<stretch::Stretcher> s = elastic::library_stretcher()(kRate, cancel);
    stretch::WarpMap map;
    std::string why;
    CHECK(map.set(kRate, {{kRate / 2, kRate}}, why));
    stretch::Planar out;
    CHECK(!s->process(tone(kRate), map, stretch::Algorithm::Polyphonic, out, why));
    return 0;
}

}  // namespace

int main() {
    if (test_exact_length() != 0) return 1;
    if (test_cancel() != 0) return 1;
    std::printf("elastic_rubberband: ok\n");
    return 0;
}
