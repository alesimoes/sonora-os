// SPDX-License-Identifier: GPL-3.0-or-later
//
// PitchPlan: shift, scale snap at amount 0/50/100, drift and vibrato scale as
// pieces, the 64-note limit, the region cut, and the refusals. Notes are built
// by hand so the plan is tested apart from the tracker.
#include <cmath>

#include "sonora/pitch/plan.h"
#include "synth.h"

using namespace sonora::pitch;
using namespace pitchtest;
namespace api = sonora::api;

namespace {

constexpr uint32_t kRate = 16000;
constexpr std::size_t kHop = 80;  // 5 ms

// A note of `seconds` from `start`, its contour given in cents per frame time.
Note make_note(std::size_t start, double seconds, double median,
               const std::function<double(double)>& dev = [](double) { return 0.0; }) {
    Note n;
    n.hop = kHop;
    n.rate = kRate;
    n.start = start;
    n.end = start + std::size_t(seconds * kRate);
    n.median_cents = median;
    for (std::size_t i = 0; i < (n.end - n.start) / kHop; ++i) {
        n.contour_cents.push_back(float(median + dev(double(i * kHop) / kRate)));
    }
    return n;
}

int test_shift() {
    PitchPlan plan({make_note(1000, 0.5, 0.0)});
    plan.edit(0).shift_cents = 100.4;
    plan.edit(0).formant_cents = -50;
    plan.edit(0).gain_db = -3.0;
    api::PitchEditSpec spec;
    std::string why;
    CHECK(plan.to_spec(100000, spec, why));
    CHECK(spec.notes.size() == 1);
    CHECK(spec.notes[0].start == 1000 && spec.notes[0].end == 9000);
    CHECK(spec.notes[0].cents == 100 && spec.notes[0].formant_cents == -50);
    CHECK(spec.notes[0].gain_db == -3.0);
    CHECK(api::validate_pitch_spec(spec, why));
    return 0;
}

int test_snap() {
    // 130 cents above A4: the nearest C major degree is B (200), 70 away.
    for (const double amount : {0.0, 50.0, 100.0}) {
        PitchPlan plan({make_note(0, 0.5, 130.0)});
        std::string why;
        CHECK(plan.snap_to_scale(0, Scale::Major, amount, why));
        CHECK(std::fabs(plan.edit(0).shift_cents - 0.7 * amount) < 1e-9);
    }
    // Chromatic snap goes to the semitone: 130 -> 100.
    PitchPlan chroma({make_note(0, 0.5, 130.0)});
    std::string why;
    CHECK(chroma.snap_to_scale(0, Scale::Chromatic, 100.0, why));
    CHECK(std::fabs(chroma.edit(0).shift_cents + 30.0) < 1e-9);
    // A minor (key 9) holds A, B, C, D, E, F, G: 130 -> B (200)? No, 100 is A#
    // (not in it), so 200 is the nearest degree at 70 and 0 at 130.
    PitchPlan minor({make_note(0, 0.5, 130.0)});
    CHECK(minor.snap_to_scale(9, Scale::NaturalMinor, 100.0, why));
    CHECK(std::fabs(minor.edit(0).shift_cents - 70.0) < 1e-9);
    // Already on a degree: nothing to move, and a snap twice is the same as once.
    PitchPlan again({make_note(0, 0.5, 130.0)});
    CHECK(again.snap_to_scale(0, Scale::Major, 100.0, why));
    const double once = again.edit(0).shift_cents;
    CHECK(again.snap_to_scale(0, Scale::Major, 100.0, why));
    CHECK(again.edit(0).shift_cents == once);
    // The spec of an amount-0 snap has nothing to change.
    PitchPlan zero({make_note(0, 0.5, 130.0)});
    CHECK(zero.snap_to_scale(0, Scale::Major, 0.0, why));
    api::PitchEditSpec spec;
    CHECK(!zero.to_spec(100000, spec, why) && why.find("nothing to change") != std::string::npos);
    // Refusals.
    CHECK(!zero.snap_to_scale(12, Scale::Major, 50.0, why));
    CHECK(!zero.snap_to_scale(0, Scale::Major, 101.0, why));
    CHECK(!zero.snap_to_scale(0, Scale::Major, std::nan(""), why));
    return 0;
}

int test_drift_pieces() {
    // 120 cents of straight drift across one second, flattened.
    PitchPlan plan({make_note(0, 1.0, 60.0, [](double t) { return 120.0 * t - 60.0; })});
    plan.edit(0).drift_scale = 0.0;
    api::PitchEditSpec spec;
    std::string why;
    CHECK(plan.to_spec(kRate, spec, why));
    CHECK(spec.notes.size() > 3 && spec.notes.size() <= api::kMaxPitchNotes);
    CHECK(api::validate_pitch_spec(spec, why));
    // The corrections undo the drift: first piece shifts up, last down, and
    // the middle one by about nothing.
    CHECK(spec.notes.front().cents > 40 && spec.notes.back().cents < -40);
    CHECK(spec.notes.front().start == 0 && spec.notes.back().end == 16000);
    for (std::size_t i = 1; i < spec.notes.size(); ++i) {
        CHECK(spec.notes[i].start == spec.notes[i - 1].end);  // contiguous
    }
    return 0;
}

int test_vibrato_pieces() {
    const auto vib = [](double t) { return 50.0 * std::sin(2.0 * kPi * 3.0 * t); };
    PitchPlan removed({make_note(0, 1.0, 0.0, vib)});
    removed.edit(0).vibrato_scale = 0.0;
    api::PitchEditSpec spec;
    std::string why;
    CHECK(removed.to_spec(kRate, spec, why));
    // The piece nearest a crest is shifted back by about its height.
    int32_t biggest = 0;
    for (const api::PitchNote& n : spec.notes) biggest = std::max(biggest, std::abs(n.cents));
    CHECK(biggest > 30 && biggest <= 50);
    PitchPlan kept({make_note(0, 1.0, 0.0, vib)});
    kept.edit(0).gain_db = 1.0;
    CHECK(kept.to_spec(kRate, spec, why));
    CHECK(spec.notes.size() == 1);  // vibrato untouched: one piece
    return 0;
}

int test_limit_and_region() {
    // 40 notes of 100 ms with drift removed: more than 64 pieces at 50 ms, so
    // the pieces grow until the list fits.
    std::vector<Note> notes;
    for (std::size_t i = 0; i < 40; ++i) {
        notes.push_back(make_note(i * 2000, 0.1, 0.0, [](double t) { return 100.0 * t; }));
    }
    PitchPlan plan(notes);
    for (std::size_t i = 0; i < plan.size(); ++i) plan.edit(i).drift_scale = 0.0;
    api::PitchEditSpec spec;
    std::string why;
    CHECK(plan.to_spec(1000000, spec, why));
    CHECK(spec.notes.size() <= api::kMaxPitchNotes);
    CHECK(api::validate_pitch_spec(spec, why));
    // 70 plain shifted notes cannot fit at all.
    std::vector<Note> many;
    for (std::size_t i = 0; i < 70; ++i) many.push_back(make_note(i * 2000, 0.1, 0.0));
    PitchPlan big(many);
    for (std::size_t i = 0; i < big.size(); ++i) big.edit(i).shift_cents = 50;
    CHECK(!big.to_spec(1000000, spec, why) && why.find("at most 64") != std::string::npos);
    // Region cut: the second note is past the end, the first is trimmed.
    PitchPlan cut({make_note(0, 0.5, 0.0), make_note(8000, 0.5, 0.0)});
    cut.edit(0).shift_cents = 10;
    cut.edit(1).shift_cents = 10;
    CHECK(cut.to_spec(5000, spec, why));
    CHECK(spec.notes.size() == 1 && spec.notes[0].end == 5000);
    CHECK(api::pitch_notes_fit(spec, 5000, why));
    return 0;
}

int test_refusals() {
    PitchPlan plan({make_note(0, 0.5, 0.0)});
    api::PitchEditSpec spec;
    std::string why;
    CHECK(!plan.to_spec(100000, spec, why));  // no edit at all
    plan.edit(0).shift_cents = 3000.0;        // clamped to the contract's range
    CHECK(plan.to_spec(100000, spec, why) && spec.notes[0].cents == 2400);
    plan.edit(0).shift_cents = std::nan("");
    CHECK(!plan.to_spec(100000, spec, why));
    plan.edit(0).shift_cents = 10;
    plan.edit(0).gain_db = 40.0;  // above the contract's +12 dB
    spec.notes.clear();
    CHECK(!plan.to_spec(100000, spec, why) && spec.notes.empty());
    return 0;
}

}  // namespace

int main() {
    if (int r = test_shift()) return r;
    if (int r = test_snap()) return r;
    if (int r = test_drift_pieces()) return r;
    if (int r = test_vibrato_pieces()) return r;
    if (int r = test_limit_and_region()) return r;
    if (int r = test_refusals()) return r;
    std::puts("pitch_plan ok");
    return 0;
}
