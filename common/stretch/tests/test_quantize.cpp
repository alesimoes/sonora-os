// SPDX-License-Identifier: GPL-3.0-or-later
// quantize_plan: even, swung and sparse onsets, strengths, never-crossing
// markers, refusals. 120 bpm 4/4 at 48 kHz: a beat is 24000 frames, a
// sixteenth (grid 0.25) is 6000.
#include <cstdio>

#include "sonora/stretch/quantize.h"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

using namespace sonora::stretch;
using sonora::model::MusicalMap;
using sonora::model::TempoEntry;

namespace {

MusicalMap make_map() {
    MusicalMap m;
    std::string why;
    m.build(48000, {TempoEntry{0, 120.0f, {{4, 4}}}}, why);
    return m;
}

QuantizeRequest req(std::vector<std::int64_t> onsets, std::uint32_t strength = 100) {
    QuantizeRequest r;
    r.onsets = std::move(onsets);
    r.region_length = 96000;
    r.strength = strength;
    return r;
}

bool never_crosses(const WarpMap& w) {
    std::int64_t s = 0, t = 0;
    for (const Marker& m : w.markers()) {
        if (m.source <= s || m.target <= t) return false;
        s = m.source;
        t = m.target;
    }
    return true;
}

}  // namespace

int main() {
    const MusicalMap map = make_map();
    std::string why;
    WarpMap w;

    // Even onsets, slightly late: all land on the grid.
    CHECK(quantize_plan(req({6100, 11900, 18200}), map, w, why));
    CHECK(w.markers().size() == 3);
    CHECK((w.markers()[0] == Marker{6100, 6000}));
    CHECK((w.markers()[1] == Marker{11900, 12000}));
    CHECK((w.markers()[2] == Marker{18200, 18000}));
    CHECK(never_crosses(w));
    CHECK(w.output_length() == 18000 + (96000 - 18200));

    // Swung eighths (long-short): the off beat is pulled from 16000 to 18000
    // on the 16th grid (nearest line), strength 100.
    CHECK(quantize_plan(req({16100}), map, w, why));
    CHECK((w.markers()[0] == Marker{16100, 18000}));

    // Strength 50 moves half way; 100 all the way.
    CHECK(quantize_plan(req({6400}, 50), map, w, why));
    CHECK((w.markers()[0] == Marker{6400, 6200}));
    CHECK(quantize_plan(req({6400}, 100), map, w, why));
    CHECK((w.markers()[0] == Marker{6400, 6000}));

    // Sparse: one onset in a long region.
    CHECK(quantize_plan(req({50500}), map, w, why));
    CHECK(w.markers().size() == 1);
    CHECK((w.markers()[0] == Marker{50500, 48000}));

    // Region not at the session start: the grid is the session's.
    QuantizeRequest off = req({1000});
    off.region_start = 5000;  // session frame 6000: already on a line
    CHECK(quantize_plan(off, map, w, why));
    CHECK((w.markers()[0] == Marker{1000, 1000}));

    // Two onsets that collapse onto one line: the second is dropped, the
    // markers still strictly increase. Unsorted input is accepted.
    CHECK(quantize_plan(req({6200, 5900, 100000, 0, -4}), map, w, why));
    CHECK(w.markers().size() == 1);
    CHECK(never_crosses(w));

    // Refusals leave the output untouched.
    const WarpMap before = w;
    CHECK(!quantize_plan(req({}), map, w, why) && !why.empty());
    CHECK(!quantize_plan(req({96000, 100000}), map, w, why));  // none inside
    CHECK(!quantize_plan(req({6000}, 0), map, w, why));
    CHECK(!quantize_plan(req({6000}, 101), map, w, why));
    QuantizeRequest bad = req({6000});
    bad.grid_beats = 0.0;
    CHECK(!quantize_plan(bad, map, w, why));
    bad.grid_beats = -1.0;
    CHECK(!quantize_plan(bad, map, w, why));
    CHECK(!quantize_plan(req({6000}), MusicalMap{}, w, why));
    QuantizeRequest empty = req({6000});
    empty.region_length = 0;
    CHECK(!quantize_plan(empty, map, w, why));
    CHECK(w.markers() == before.markers());
    return 0;
}
