// SPDX-License-Identifier: GPL-3.0-or-later
//
// Note segmentation: a split on a step and on a gap, median, drift and vibrato
// depth, and the refusals.
#include <cmath>

#include "sonora/pitch/segment.h"
#include "synth.h"

using namespace sonora::pitch;
using namespace pitchtest;

namespace {

constexpr uint32_t kRate = 16000;

bool segment(const std::vector<float>& x, std::vector<Note>& notes, std::string& why) {
    Contour c;
    return YinTracker().track(x, kRate, c, why) && segment_notes(c, notes, why);
}

int test_step() {
    // A3 for 0.5 s, then E4 (700 cents up) for 0.5 s, no gap.
    const auto step = [](double t) { return t < 0.5 ? -1200.0 : -500.0; };
    std::vector<Note> notes;
    std::string why;
    CHECK(segment(tone(1.0, kRate, step), notes, why));
    CHECK(notes.size() == 2);
    CHECK(std::fabs(notes[0].median_cents + 1200.0) < 10.0);
    CHECK(std::fabs(notes[1].median_cents + 500.0) < 10.0);
    CHECK(notes[0].end == notes[1].start);  // touching, never overlapping
    CHECK(std::fabs(double(notes[0].end) / kRate - 0.5) < 0.03);
    return 0;
}

int test_gap() {
    const auto flat = [](double) { return 0.0; };
    std::vector<Note> notes;
    std::string why;
    // The same pitch twice with 100 ms of silence between: two notes.
    CHECK(segment(tone(1.0, kRate, flat, 0.5, [](double t) { return t < 0.4 || t > 0.5; }), notes, why));
    CHECK(notes.size() == 2);
    CHECK(notes[0].end < notes[1].start);
    // A 15 ms dropout is bridged: one note.
    CHECK(segment(tone(1.0, kRate, flat, 0.5, [](double t) { return t < 0.4 || t > 0.415; }), notes, why));
    CHECK(notes.size() == 1);
    return 0;
}

int test_vibrato() {
    // A4 with 40 cents of 5.5 Hz vibrato: one note, depth near 40.
    const auto vib = [](double t) { return 40.0 * std::sin(2.0 * kPi * 5.5 * t); };
    std::vector<Note> notes;
    std::string why;
    CHECK(segment(tone(1.5, kRate, vib), notes, why));
    CHECK(notes.size() == 1);
    std::fprintf(stderr, "vibrato depth %.1f, median %.1f, drift %.1f\n", notes[0].vibrato_depth_cents,
                 notes[0].median_cents, notes[0].drift_cents);
    CHECK(std::fabs(notes[0].vibrato_depth_cents - 40.0) < 8.0);
    CHECK(std::fabs(notes[0].median_cents) < 10.0);
    CHECK(std::fabs(notes[0].drift_cents) < 20.0);
    // A steady tone has none.
    CHECK(segment(tone(1.0, kRate, [](double) { return 0.0; }), notes, why));
    CHECK(notes[0].vibrato_depth_cents < 5.0);
    return 0;
}

int test_drift() {
    // 100 cents of glide over a second stays one note and reads as drift.
    std::vector<Note> notes;
    std::string why;
    CHECK(segment(tone(1.0, kRate, [](double t) { return 100.0 * t; }), notes, why));
    CHECK(notes.size() == 1);
    CHECK(std::fabs(notes[0].drift_cents - 100.0) < 15.0);
    CHECK(std::fabs(notes[0].median_cents - 50.0) < 10.0);
    return 0;
}

int test_span() {
    std::vector<Note> notes;
    std::string why;
    CHECK(segment(tone(1.0, kRate, [](double) { return 0.0; }, 0.5, [](double t) { return t > 0.2 && t < 0.8; }),
                  notes, why));
    CHECK(notes.size() == 1);
    CHECK(std::fabs(double(notes[0].start) / kRate - 0.2) < 0.02);
    CHECK(std::fabs(double(notes[0].end) / kRate - 0.8) < 0.02);
    CHECK(notes[0].hop == 80 && notes[0].rate == kRate);
    return 0;
}

int test_refusals() {
    std::vector<Note> notes;
    std::string why;
    // Too few voiced frames: 50 ms of tone in a second of silence.
    CHECK(!segment(tone(1.0, kRate, [](double) { return 0.0; }, 0.5, [](double t) { return t < 0.05; }), notes, why));
    CHECK(why.find("too few voiced") != std::string::npos);
    CHECK(notes.empty());
    // Noise and silence.
    CHECK(!segment(noise(1.0, kRate), notes, why));
    CHECK(!segment(std::vector<float>(kRate, 0.0f), notes, why));
    // Enough voiced time but no note of the minimum length: 8 bursts of 20 ms.
    CHECK(!segment(tone(1.0, kRate, [](double) { return 0.0; }, 0.5,
                        [](double t) { return std::fmod(t, 0.125) < 0.02; }),
                   notes, why));
    return 0;
}

}  // namespace

int main() {
    if (int r = test_step()) return r;
    if (int r = test_gap()) return r;
    if (int r = test_vibrato()) return r;
    if (int r = test_drift()) return r;
    if (int r = test_span()) return r;
    if (int r = test_refusals()) return r;
    std::puts("pitch_segment ok");
    return 0;
}
