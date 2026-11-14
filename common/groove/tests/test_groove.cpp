// SPDX-License-Identifier: GPL-3.0-or-later
//
// Groove templates (ADR-0115): swing values, extraction from onsets, the
// strength scale, the overlap skip, refusals, and the round trip from an
// extracted template back onto the grid it came from.
#include <cmath>
#include <cstdio>

#include "sonora/groove/groove.h"

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            return 1;                                                               \
        }                                                                           \
    } while (0)

using namespace sonora::groove;
using sonora::model::Frames;
using sonora::model::MusicalMap;
using sonora::model::Region;
using sonora::model::TempoEntry;

namespace {

// 120 bpm in 4/4 at 48 kHz: a beat is 24000 frames, an eighth slot 12000.
constexpr Frames kBeat = 24000;
constexpr Frames kSlot = 12000;

MusicalMap make_map() {
    MusicalMap m;
    std::string why;
    m.build(48000, {TempoEntry{0, 120.0f, {{4, 4}}}}, why);
    return m;
}

Region region(sonora::model::Id id, Frames pos, Frames len) {
    Region r;
    r.id = id;
    r.position = pos;
    r.length = len;
    return r;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

int test_swing() {
    GrooveTemplate t;
    std::string why;
    CHECK(groove_swing(50, 2, t, why));
    CHECK(t.slots.size() == 2 && near(t.slots[0].offset, 0) && near(t.slots[1].offset, 0));
    CHECK(groove_swing(66, 2, t, why));
    CHECK(near(t.slots[0].offset, 0) && near(t.slots[1].offset, 0.32));
    CHECK(groove_swing(75, 4, t, why));
    CHECK(t.slots.size() == 4 && near(t.slots[1].offset, 0.5) && near(t.slots[2].offset, 0) &&
          near(t.slots[3].offset, 0.5));
    const GrooveTemplate keep = t;
    CHECK(!groove_swing(49, 2, t, why) && !why.empty());
    CHECK(!groove_swing(76, 2, t, why));
    CHECK(!groove_swing(60, 3, t, why));
    CHECK(!groove_swing(60, 0, t, why));
    CHECK(!groove_swing(60, 66, t, why));
    CHECK(t.slots.size() == keep.slots.size());  // refusals leave `out` alone
    return 0;
}

int test_extraction() {
    const MusicalMap map = make_map();
    GrooveTemplate t;
    std::string why;
    // Even: on the grid.
    CHECK(groove_from_onsets(map, {0, kSlot, kBeat, kBeat + kSlot}, 2, t, why));
    CHECK(near(t.slots[0].offset, 0) && near(t.slots[1].offset, 0));
    // Swung: the off-beats sit 0.32 of a slot late.
    const Frames late = static_cast<Frames>(0.32 * kSlot);
    CHECK(groove_from_onsets(map, {0, kSlot + late, kBeat, kBeat + kSlot + late, 2 * kBeat + kSlot + late},
                             2, t, why));
    CHECK(near(t.slots[0].offset, 0) && near(t.slots[1].offset, 0.32));
    // Sparse: only slot 1 has onsets (and an outlier the median ignores);
    // slot 0 stays 0.
    CHECK(groove_from_onsets(map,
                             {kSlot + 2400, kBeat + kSlot + 2400, 2 * kBeat + kSlot + 2400,
                              3 * kBeat + kSlot - 3000},
                             2, t, why));
    CHECK(near(t.slots[0].offset, 0) && near(t.slots[1].offset, 0.2));
    // An onset just before the beat belongs to slot 0 with a negative offset.
    CHECK(groove_from_onsets(map, {kBeat - 1200}, 2, t, why));
    CHECK(near(t.slots[0].offset, -0.1) && near(t.slots[1].offset, 0));
    // Refusals.
    CHECK(!groove_from_onsets(map, {}, 2, t, why) && !why.empty());
    CHECK(!groove_from_onsets(map, {0}, 0, t, why));
    CHECK(!groove_from_onsets(map, {0}, 65, t, why));
    CHECK(!groove_from_onsets(MusicalMap(), {0}, 2, t, why));
    return 0;
}

int test_strength() {
    const MusicalMap map = make_map();
    GrooveTemplate t;
    std::string why;
    CHECK(groove_swing(66, 2, t, why));
    const std::vector<Region> rs = {region(1, kSlot, 1000)};
    GrooveResult r;
    CHECK(groove_moves(map, t, rs, 100, r, why));
    CHECK(r.moves.size() == 1 && r.moves[0].region == 1 && r.moves[0].position == kSlot + 3840);
    CHECK(groove_moves(map, t, rs, 50, r, why));
    CHECK(r.moves.size() == 1 && r.moves[0].position == kSlot + 1920);
    // A region on a slot with offset 0 does not move, and is not skipped.
    CHECK(groove_moves(map, t, {region(2, kBeat, 1000)}, 100, r, why));
    CHECK(r.moves.empty() && r.skipped == 0);
    // A start a little off the slot is pulled onto it plus the offset.
    CHECK(groove_moves(map, t, {region(3, kSlot - 500, 1000)}, 100, r, why));
    CHECK(r.moves.size() == 1 && r.moves[0].position == kSlot + 3840);
    return 0;
}

int test_overlap_skip() {
    const MusicalMap map = make_map();
    GrooveTemplate t;
    std::string why;
    CHECK(groove_swing(66, 2, t, why));
    // A would end at 15840 + 12000, past B at 24000: skipped. C has room.
    const std::vector<Region> rs = {region(1, kSlot, 12000), region(2, kBeat, 6000),
                                    region(3, kBeat + kSlot, 4000)};
    GrooveResult r;
    CHECK(groove_moves(map, t, rs, 100, r, why));
    CHECK(r.skipped == 1);
    CHECK(r.moves.size() == 1 && r.moves[0].region == 3 && r.moves[0].position == kBeat + kSlot + 3840);
    return 0;
}

int test_refusals() {
    const MusicalMap map = make_map();
    GrooveTemplate t;
    std::string why;
    CHECK(groove_swing(66, 2, t, why));
    GrooveResult r;
    const std::vector<Region> rs = {region(1, kSlot, 1000)};
    CHECK(!groove_moves(map, t, rs, 0, r, why) && !why.empty());
    CHECK(!groove_moves(map, t, rs, -5, r, why));
    CHECK(!groove_moves(map, t, rs, 101, r, why));
    why.clear();
    CHECK(!groove_moves(map, t, {}, 100, r, why) && !why.empty());
    CHECK(!groove_moves(MusicalMap(), t, rs, 100, r, why));
    GrooveTemplate bad = t;
    bad.slots.pop_back();
    CHECK(!groove_moves(map, bad, rs, 100, r, why));
    bad = t;
    bad.slots[1].offset = 0.9;
    CHECK(!groove_moves(map, bad, rs, 100, r, why));
    bad.slots_per_beat = 0;
    bad.slots.clear();
    CHECK(!groove_moves(map, bad, rs, 100, r, why));
    return 0;
}

int test_round_trip() {
    const MusicalMap map = make_map();
    GrooveTemplate t;
    std::string why;
    // A swung reference, taken to a straight region set, lands where the
    // reference's onsets are.
    std::vector<Frames> ref;
    for (int b = 0; b < 4; ++b) {
        ref.push_back(b * kBeat);
        ref.push_back(b * kBeat + kSlot + 3840);
    }
    CHECK(groove_from_onsets(map, ref, 2, t, why));
    std::vector<Region> straight;
    for (int b = 0; b < 4; ++b) {
        straight.push_back(region(10 + 2 * b, b * kBeat, 2000));
        straight.push_back(region(11 + 2 * b, b * kBeat + kSlot, 2000));
    }
    GrooveResult r;
    CHECK(groove_moves(map, t, straight, 100, r, why));
    CHECK(r.skipped == 0 && r.moves.size() == 4);
    for (const auto& m : r.moves) {
        const int b = static_cast<int>((m.region - 11) / 2);
        CHECK(m.position == b * kBeat + kSlot + 3840);
    }
    return 0;
}

}  // namespace

int main() {
    if (test_swing()) return 1;
    if (test_extraction()) return 1;
    if (test_strength()) return 1;
    if (test_overlap_skip()) return 1;
    if (test_refusals()) return 1;
    if (test_round_trip()) return 1;
    std::puts("groove_test: ok");
    return 0;
}
