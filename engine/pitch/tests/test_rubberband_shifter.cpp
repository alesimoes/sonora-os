// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Rubber Band adapter, built only with the library: the result has the
// input's length, the shifted note sounds within 10 cents of the request, and
// the audio outside the notes is untouched.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "rubberband_shifter.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

constexpr double kPi = 3.14159265358979323846;

// The frequency of x[a, b) from its rising zero crossings, each placed between
// samples by linear interpolation: exact enough for cents on a clean tone.
double frequency_of(const std::vector<float>& x, std::size_t a, std::size_t b, double rate) {
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (std::size_t i = a + 1; i < b; ++i) {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
            const double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0.0) first = t;
            last = t;
            ++crossings;
        }
    }
    if (crossings < 2) return 0.0;
    return rate * (crossings - 1) / (last - first);
}

int test_shift_keeps_length_and_hits_the_pitch() {
    const uint32_t rate = 48000;
    const std::size_t frames = 48000;
    const double f0 = 220.0;
    sonora::pitch::Planar in(1, std::vector<float>(frames));
    for (std::size_t i = 0; i < frames; ++i) {
        in[0][i] = 0.5f * static_cast<float>(std::sin(2.0 * kPi * f0 * i / rate));
    }
    sonora::api::PitchEditSpec spec;
    sonora::api::PitchNote n;
    n.start = 12000;
    n.end = 36000;
    n.cents = 300;
    spec.notes.push_back(n);

    sonora::RubberBandPitchShifter shifter;
    sonora::pitch::Planar out;
    std::string why;
    CHECK(shifter.shift(in, rate, spec, out, why));
    CHECK(out.size() == 1 && out[0].size() == frames);

    const double got = frequency_of(out[0], 18000, 30000, rate);
    const double want_cents = 300.0;
    const double cents = 1200.0 * std::log2(got / f0);
    std::printf("measured %.3f Hz, %.2f cents (asked %.0f)\n", got, cents, want_cents);
    CHECK(std::fabs(cents - want_cents) < 10.0);
    // Outside the note and its crossfade the input comes back as it was.
    for (std::size_t i = 0; i < 11000; ++i) CHECK(out[0][i] == in[0][i]);
    for (std::size_t i = 37000; i < frames; ++i) CHECK(out[0][i] == in[0][i]);
    return 0;
}

}  // namespace

int main() {
    const int failed = test_shift_keeps_length_and_hits_the_pitch();
    if (failed == 0) std::printf("ok\n");
    return failed == 0 ? 0 : 1;
}
