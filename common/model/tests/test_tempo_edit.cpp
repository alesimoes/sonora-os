// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tempo operations (ADR-0050): set, remove, ramp, scale, meter; each leaves
// the map valid and the grid where MusicalMap reads it, and a refusal leaves
// the map as it was.
#include <cmath>

#include "model_test.h"
#include "sonora/model/musical_map.h"
#include "sonora/model/tempo_edit.h"

using namespace model_test;

namespace {

constexpr std::uint32_t kRate = 48000;
// One bar of 4/4 at 120 bpm.
constexpr Frames kBar = 96000;

std::vector<TempoEntry> base() { return {TempoEntry{0, 120.0f, {{4, 4}}}}; }

bool valid(const std::vector<TempoEntry>& m) {
    std::string where;
    return tempo_map_problem(kRate, m, where).empty();
}

int test_set_and_remove() {
    auto m = base();
    std::string why;
    CHECK(tempo_set_at(m, kRate, 4 * kBar, 90.0f, why));
    CHECK(m.size() == 2 && m[1].at == 4 * kBar && m[1].bpm == 90.0f && m[1].meter[0] == 4);
    // The same place again changes the entry, it does not add one.
    CHECK(tempo_set_at(m, kRate, 4 * kBar, 100.0f, why));
    CHECK(m.size() == 2 && m[1].bpm == 100.0f);
    CHECK(tempo_set_at(m, kRate, 0, 110.0f, why));
    CHECK(m[0].bpm == 110.0f && m.size() == 2);

    const auto before = m;
    CHECK(!tempo_set_at(m, kRate, kBar, 0.5f, why));
    CHECK(mentions(why, "1 to 1000"));
    CHECK(!tempo_set_at(m, kRate, -1, 100.0f, why));
    CHECK(m == before);

    CHECK(!tempo_remove_at(m, kRate, 0, why));
    CHECK(mentions(why, "first tempo"));
    CHECK(!tempo_remove_at(m, kRate, 5, why));
    CHECK(tempo_remove_at(m, kRate, 4 * kBar, why));
    CHECK(m.size() == 1);
    return 0;
}

int test_ramp() {
    auto m = base();
    std::string why;
    // Four bars from 120 down to 60, the tempo then held at 60.
    CHECK(tempo_ramp(m, kRate, 0, 4 * kBar, 120.0f, 60.0f, why));
    CHECK(valid(m));
    CHECK(m.size() > 8 && m.size() <= kMaxRampSteps + 1);
    CHECK(m.front().at == 0 && m.front().bpm == 120.0f);
    CHECK(m.back().at == 4 * kBar && m.back().bpm == 60.0f);
    for (std::size_t i = 1; i < m.size(); ++i) CHECK(m[i].bpm <= m[i - 1].bpm);
    // The second to last step has reached the end tempo: no step is left
    // above it once the ramp is over.
    CHECK(m[m.size() - 2].bpm == 60.0f);

    // Slower than a flat 120, so the bar lines after it move later; the
    // frames of what follows do not change, only how they are counted.
    MusicalMap flat, ramped;
    std::string w;
    CHECK(flat.build(kRate, base(), w) && ramped.build(kRate, m, w));
    Ticks tf = 0, tr = 0;
    CHECK(flat.tick_at(4 * kBar, tf) && ramped.tick_at(4 * kBar, tr));
    CHECK(tr < tf);
    // Linear in time: halfway through the ramp the tempo is near the mean.
    const TempoEntry& mid = m[tempo_entry_in_force(m, 2 * kBar)];
    CHECK(std::fabs(mid.bpm - 90.0f) < 4.0f);

    // A ramp later in the map keeps what is before it and after it.
    auto n = base();
    CHECK(tempo_set_at(n, kRate, 8 * kBar, 100.0f, why));
    CHECK(tempo_ramp(n, kRate, 2 * kBar, 4 * kBar, 120.0f, 140.0f, why));
    CHECK(n.front().at == 0 && n[1].at == 2 * kBar);
    CHECK(n.back().at == 8 * kBar && n.back().bpm == 100.0f);
    CHECK(valid(n));
    return 0;
}

int test_ramp_refusals() {
    auto m = base();
    std::string why;
    Frames at = 0;
    CHECK(meter_set_near(m, kRate, 2 * kBar, 3, 4, at, why));
    const auto before = m;
    CHECK(!tempo_ramp(m, kRate, 0, 4 * kBar, 120.0f, 90.0f, why));
    CHECK(mentions(why, "meter change"));
    CHECK(!tempo_ramp(m, kRate, 4 * kBar, 4 * kBar, 120.0f, 90.0f, why));
    CHECK(!tempo_ramp(m, kRate, 0, kBar, 120.0f, 2000.0f, why));
    CHECK(m == before);
    // Each side of the meter change ramps.
    CHECK(tempo_ramp(m, kRate, at, at + 2 * kBar, 120.0f, 90.0f, why));
    CHECK(valid(m));
    return 0;
}

int test_scale() {
    auto m = base();
    std::string why;
    CHECK(tempo_set_at(m, kRate, 4 * kBar, 100.0f, why));
    // Double time over bars 2 to 3: the tempo before and after is untouched.
    CHECK(tempo_scale(m, kRate, 2 * kBar, 3 * kBar, 2.0, why));
    CHECK(valid(m));
    CHECK(m[tempo_entry_in_force(m, kBar)].bpm == 120.0f);
    CHECK(m[tempo_entry_in_force(m, 2 * kBar)].bpm == 240.0f);
    CHECK(m[tempo_entry_in_force(m, 3 * kBar)].bpm == 120.0f);
    CHECK(m[tempo_entry_in_force(m, 4 * kBar)].bpm == 100.0f);

    const auto before = m;
    CHECK(!tempo_scale(m, kRate, 0, kBar, 20.0, why));
    CHECK(mentions(why, "1 to 1000"));
    CHECK(!tempo_scale(m, kRate, 0, kBar, 0.0, why));
    CHECK(!tempo_scale(m, kRate, kBar, kBar, 2.0, why));
    CHECK(m == before);
    return 0;
}

int test_meter() {
    auto m = base();
    std::string why;
    Frames at = 0;
    // Just past bar 3 snaps back to its line; the new meter starts a bar there.
    CHECK(meter_set_near(m, kRate, 2 * kBar + 5000, 3, 4, at, why));
    CHECK(at == 2 * kBar && m.size() == 2 && m[1].meter[0] == 3 && m[1].bpm == 120.0f);
    MusicalMap map;
    CHECK(map.build(kRate, m, why));
    Ticks t = 0;
    CHECK(map.tick_at(at, t));
    const BarsBeats b = map.bars_beats_at(t);
    CHECK(b.bar == 3 && b.beat == 1 && b.tick == 0);

    // The meter holds through the tempo-only entries after it: a ramp's
    // steps do not start 4/4 bars again.
    auto r = base();
    CHECK(tempo_ramp(r, kRate, 4 * kBar, 6 * kBar, 120.0f, 90.0f, why));
    CHECK(meter_set_near(r, kRate, 2 * kBar, 7, 8, at, why));
    for (const TempoEntry& e : r) CHECK((e.at < at) == (e.meter[0] == 4));
    CHECK(r.back().meter[1] == 8);
    // Removing the change gives the followers the old meter back.
    CHECK(tempo_remove_at(r, kRate, at, why));
    for (const TempoEntry& e : r) CHECK(e.meter[0] == 4 && e.meter[1] == 4);

    CHECK(!meter_set_near(m, kRate, 0, 0, 4, at, why));
    CHECK(!meter_set_near(m, kRate, 0, 4, 3, at, why));
    CHECK(mentions(why, "power") || mentions(why, "over 1, 2, 4"));
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    int run = 0;
    const auto t = [&](int (*f)()) {
        ++run;
        failed += f();
    };
    t(test_set_and_remove);
    t(test_ramp);
    t(test_ramp_refusals);
    t(test_scale);
    t(test_meter);
    if (failed != 0) {
        std::fprintf(stderr, "model_tempo_edit: %d of %d failed\n", failed, run);
        return 1;
    }
    std::printf("model_tempo_edit: %d passed\n", run);
    return 0;
}
