// SPDX-License-Identifier: GPL-3.0-or-later
//
// The offline scorer on synthetic voices with a known f0 (ADR-0077): a glide
// and a vibrato tone, so no dataset audio is needed. The bars are stated at
// each check.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "tuner_eval_run.h"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace {
using namespace sonora::native::tuner_eval;

constexpr double kFs = 44100.0;
constexpr double kPi = 3.14159265358979323846;

// f0(t) of the fixture, rendered as a harmonic tone (4 partials, as a voice
// has more than a sine) by integrating the frequency.
template <class F0>
Clip render(double seconds, F0 f0, std::vector<PitchFrame>& ref) {
    Clip c;
    c.fs = kFs;
    const std::size_t n = static_cast<std::size_t>(seconds * kFs);
    c.samples.resize(n);
    double phase = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kFs;
        phase += 2.0 * kPi * f0(t) / kFs;
        double s = 0.0;
        for (int h = 1; h <= 4; ++h) s += std::sin(h * phase) / h;
        c.samples[i] = static_cast<float>(0.25 * s);
    }
    for (double t = 0.0; t < seconds; t += 0.01) ref.push_back({t, f0(t), true});
    return c;
}

double glide(double t) { return 150.0 * std::pow(2.0, t / 2.0); }  // one octave in 2 s
double vibrato(double t) { return 220.0 * std::pow(2.0, 30.0 * std::sin(2.0 * kPi * 5.5 * t) / 1200.0); }

int csv_parsing() {
    const auto f = parse_f0_csv("time,f0\n0.00,0\n0.01,220.5\r\n0.02,0.0\nnot,a line\n0.03,221\n");
    CHECK(f.size() == 4);
    CHECK(!f[0].voiced && f[0].hz == 0.0);
    CHECK(f[1].voiced && std::fabs(f[1].hz - 220.5) < 1e-9 && std::fabs(f[1].time_s - 0.01) < 1e-9);
    CHECK(f[3].voiced);
    return 0;
}

int detector_tracks_known_f0() {
    Options opt;
    opt.key = 0;
    opt.scale = 0;  // chromatic: every note is in scale
    for (int which = 0; which < 2; ++which) {
        TrackSet set;
        std::vector<PitchFrame> ref;
        set.clips.push_back(which == 0 ? render(2.0, glide, ref) : render(2.0, vibrato, ref));
        set.references.push_back(ref);
        const SetReport rep = evaluate(set, opt);
        // Medium range (100-800 Hz) covers both fixtures. Bar: RPA >= 0.9 at
        // 50 cents on a clean harmonic tone (the first window is not full).
        CHECK(rep.detector[1].rpa >= 0.9);
        CHECK(rep.detector[1].rca >= rep.detector[1].rpa);
        CHECK(rep.detector[1].voicing_recall >= 0.9);
    }
    return 0;
}

int processor_puts_the_glide_in_scale() {
    Options opt;
    opt.key = 0;
    opt.scale = 1;  // C major
    TrackSet set;
    std::vector<PitchFrame> ref;
    set.clips.push_back(render(2.0, glide, ref));
    set.references.push_back(ref);
    const SetReport rep = evaluate(set, opt);
    // Bar: >= 0.7 of the voiced output within 25 cents of a C-major note.
    // An untuned glide would sit in a 25-cent band about 0.5 of the time.
    std::fprintf(stderr, "in_scale_fraction %.3f\n", rep.processor.in_scale_fraction);
    CHECK(rep.processor.in_scale_fraction >= 0.7);
    return 0;
}

int missing_directory_exits_two() {
    Options opt;
    opt.dataset = "vocadito";
    opt.data_dir = "/nonexistent/sonora-eval-data";
    const RunResult r = run_dataset(opt);
    CHECK(r.exit_code == 2);
    CHECK(!r.message.empty());
    CHECK(r.json.empty());
    opt.dataset = "nope";
    CHECK(run_dataset(opt).exit_code == 1);
    return 0;
}

}  // namespace

int main() {
    if (int rc = csv_parsing()) return rc;
    if (int rc = detector_tracks_known_f0()) return rc;
    if (int rc = processor_puts_the_glide_in_scale()) return rc;
    if (int rc = missing_directory_exits_two()) return rc;
    std::puts("native_tuner_eval_test: ok");
    return 0;
}
