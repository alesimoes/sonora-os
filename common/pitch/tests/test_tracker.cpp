// SPDX-License-Identifier: GPL-3.0-or-later
//
// YinTracker: a sine and a glide within 10 cents, silence and noise unvoiced,
// a different A4, and the refusals.
#include <algorithm>
#include <cmath>

#include "sonora/pitch/tracker.h"
#include "synth.h"

using namespace sonora::pitch;
using namespace pitchtest;

namespace {

// Largest error in cents over the frames away from the ends.
double worst_error(const Contour& c, const std::function<double(double)>& want, double a4_shift = 0) {
    double worst = 0.0;
    for (std::size_t i = 12; i + 12 < c.frames.size(); ++i) {
        if (!c.frames[i].voiced) return 1e9;
        const double t = double(i * c.hop) / c.rate;
        worst = std::max(worst, std::fabs(double(c.frames[i].cents) - want(t) - a4_shift));
    }
    return worst;
}

int test_sine() {
    YinTracker yin;
    std::string why;
    for (const uint32_t rate : {16000u, 48000u}) {
        for (const double hz : {110.0, 220.0, 330.0, 440.0}) {
            const double cents = 1200.0 * std::log2(hz / 440.0);
            Contour c;
            CHECK(yin.track(tone(1.0, rate, [&](double) { return cents; }), rate, c, why));
            CHECK(c.hop == std::size_t(std::lround(0.005 * rate)));
            const double err = worst_error(c, [&](double) { return cents; });
            std::fprintf(stderr, "sine %g Hz at %u: worst %.2f cents\n", hz, rate, err);
            CHECK(err < 10.0);
        }
    }
    return 0;
}

int test_glide() {
    YinTracker yin;
    std::string why;
    // One octave up over three seconds, 400 cents a second.
    const auto glide = [](double t) { return -1200.0 + 400.0 * t; };
    Contour c;
    CHECK(yin.track(tone(3.0, 22050, glide), 22050, c, why));
    const double err = worst_error(c, glide);
    std::fprintf(stderr, "glide: worst %.2f cents\n", err);
    CHECK(err < 10.0);
    return 0;
}

int test_unvoiced() {
    YinTracker yin;
    std::string why;
    Contour c;
    CHECK(yin.track(std::vector<float>(16000, 0.0f), 16000, c, why));
    CHECK(c.voiced_count() == 0);
    CHECK(yin.track(noise(1.0, 16000), 16000, c, why));
    CHECK(c.voiced_count() == 0);
    // A tone with a hole in the middle: the hole is unvoiced, the rest voiced.
    CHECK(yin.track(tone(1.0, 16000, [](double) { return 0.0; }, 0.5,
                         [](double t) { return t < 0.4 || t > 0.6; }),
                    16000, c, why));
    CHECK(!c.frames[100].voiced);
    CHECK(c.frames[40].voiced && c.frames[160].voiced);
    return 0;
}

int test_a4() {
    YinConfig cfg;
    cfg.a4_hz = 432.0;
    YinTracker yin(cfg);
    std::string why;
    Contour c;
    CHECK(yin.track(tone(1.0, 16000, [](double) { return 0.0; }), 16000, c, why));  // 440 Hz
    CHECK(c.a4_hz == 432.0);
    // 440 Hz is 31.77 cents above a 432 Hz A4.
    CHECK(worst_error(c, [](double) { return 1200.0 * std::log2(440.0 / 432.0); }) < 10.0);
    return 0;
}

int test_refusals() {
    std::string why;
    Contour c;
    c.hop = 99;
    const std::vector<float> x(16000, 0.1f);
    CHECK(!YinTracker().track(x, 4000, c, why) && !why.empty());
    CHECK(c.hop == 99);  // untouched on refusal
    YinConfig bad;
    bad.hop_ms = 1.0;
    CHECK(!YinTracker(bad).track(x, 16000, c, why));
    bad = YinConfig{};
    bad.a4_hz = 300.0;
    CHECK(!YinTracker(bad).track(x, 16000, c, why));
    std::vector<float> nan = x;
    nan[5] = std::nanf("");
    CHECK(!YinTracker().track(nan, 16000, c, why));
    // Shorter than a frame: still tracks, without a voiced frame.
    CHECK(YinTracker().track(std::vector<float>(10, 0.0f), 16000, c, why));
    CHECK(c.voiced_count() == 0);
    return 0;
}

}  // namespace

int main() {
    if (int r = test_sine()) return r;
    if (int r = test_glide()) return r;
    if (int r = test_unvoiced()) return r;
    if (int r = test_a4()) return r;
    if (int r = test_refusals()) return r;
    std::puts("pitch_tracker ok");
    return 0;
}
