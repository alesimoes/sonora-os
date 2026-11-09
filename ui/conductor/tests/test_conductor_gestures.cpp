// SPDX-License-Identifier: GPL-3.0-or-later
//
// Conductor-ruler gestures (ADR-0140): hit and miss, the slop, the drag's
// clamp and snap, the first entry that cannot go, a meter on the nearest bar
// line, a drag across a ramp's staircase.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/model/musical_map.h"
#include "sonora/model/tempo_edit.h"
#include "sonora/ui_conductor/conductor.h"

using namespace sonora;
using namespace sonora::model;
using namespace sonora::ui::conductor;

#define OK(cond)                                                                 \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr Frames kBar = 4 * kRate / 2;  // 120 bpm 4/4

std::vector<TempoEntry> base() { return {TempoEntry{}}; }

bool valid(const std::vector<TempoEntry>& m) {
    std::string where;
    return tempo_map_problem(kRate, m, where).empty();
}

int test_hit() {
    std::string why;
    auto m = base();
    OK(tempo_set_at(m, kRate, kBar * 2, 90.0f, why));
    const double fpp = 100.0;  // mark 1 at x = 1920
    OK(hit_test(m, kRate, fpp, 0.0) == std::optional<std::size_t>(0));
    OK(hit_test(m, kRate, fpp, 1920.0) == std::optional<std::size_t>(1));
    OK(!hit_test(m, kRate, fpp, 500.0));
    OK(!hit_test({}, kRate, fpp, 0.0));
    OK(!hit_test(m, kRate, 0.0, 0.0));
    return 0;
}

int test_slop() {
    std::string why;
    auto m = base();
    OK(tempo_set_at(m, kRate, kBar * 2, 90.0f, why));
    OK(hit_test(m, kRate, 100.0, 1914.0) == std::optional<std::size_t>(1));
    OK(hit_test(m, kRate, 100.0, 1926.0) == std::optional<std::size_t>(1));
    OK(!hit_test(m, kRate, 100.0, 1913.5));
    OK(!hit_test(m, kRate, 100.0, 1926.5));
    // Two marks inside one slop: the nearer wins.
    OK(tempo_set_at(m, kRate, kBar * 2 + 300, 95.0f, why));
    OK(hit_test(m, kRate, 100.0, 1921.0) == std::optional<std::size_t>(1));
    OK(hit_test(m, kRate, 100.0, 1922.5) == std::optional<std::size_t>(2));
    return 0;
}

int test_drag_clamp() {
    auto m = base();
    TempoDrag d = drag_tempo(m, 0, 1e9, false);
    OK(d.ok && d.bpm == kMaxBpm);
    d = drag_tempo(m, 0, -1e9, true);
    OK(d.ok && d.bpm == kMinBpm);
    d = drag_tempo(m, 0, 10.0, false);  // 120 + 1.0
    OK(d.ok && d.bpm == 121.0f);
    d = drag_tempo(m, 0, 0.37, false);  // 120.037 -> 120.04
    OK(d.ok && d.bpm == 120.04f);
    d = drag_tempo(m, 0, 4.4, true);  // 120.44 -> 120
    OK(d.ok && d.bpm == 120.0f);
    d = drag_tempo(m, 0, 6.0, true);  // 120.6 -> 121
    OK(d.ok && d.bpm == 121.0f);
    d = drag_tempo(m, 5, 1.0, false);
    OK(!d.ok && !d.why.empty());
    std::string why;
    OK(apply_tempo_drag(m, kRate, 0, 1e9, false, why) && m[0].bpm == kMaxBpm && valid(m));
    OK(!apply_tempo_drag(m, kRate, 3, 1.0, false, why) && !why.empty());
    return 0;
}

int test_first_entry() {
    std::string why;
    auto m = base();
    OK(!remove_mark(m, kRate, 0, why) && !why.empty() && m.size() == 1);
    OK(tempo_set_at(m, kRate, kBar, 100.0f, why));
    OK(remove_mark(m, kRate, 1, why) && m.size() == 1 && valid(m));
    OK(!remove_mark(m, kRate, 0, why) && m.size() == 1);
    OK(!remove_mark(m, kRate, 7, why));
    return 0;
}

int test_meter() {
    std::string why;
    auto m = base();
    Frames at = -1;
    OK(apply_meter(m, kRate, kBar * 3 + kBar / 4, 3, 4, at, why));  // nearest line: bar 4
    OK(at == kBar * 3 && m.size() == 2 && m[1].at == kBar * 3 && m[1].meter[0] == 3 && valid(m));
    OK(apply_meter(m, kRate, kBar * 5 - 100, 6, 8, at, why) && m.back().meter[1] == 8);
    auto before = m;
    OK(!apply_meter(m, kRate, 0, 4, 3, at, why) && !why.empty() && m == before);
    const MeterChoices c = meter_choices();
    OK(c.beats.size() == 32 && c.beats.front() == 1 && c.beats.back() == 32);
    OK((c.notes == std::vector<std::uint32_t>{1, 2, 4, 8, 16}));
    for (auto n : c.notes) {
        auto t = base();
        OK(apply_meter(t, kRate, kBar, 4, n, at, why) && valid(t));
    }
    return 0;
}

int test_ramp_staircase() {
    std::string why;
    auto m = base();
    OK(tempo_ramp(m, kRate, kBar, kBar * 5, 120.0f, 160.0f, why));
    OK(m.size() > 4);
    // Every step is a mark of its own: a hit finds each, in order.
    const double fpp = 50.0;
    for (std::size_t i = 0; i < m.size(); ++i) {
        const auto h = hit_test(m, kRate, fpp, static_cast<double>(m[i].at) / fpp);
        OK(h && *h == i);
    }
    const std::size_t mid = m.size() / 2;
    const auto before = m;
    OK(apply_tempo_drag(m, kRate, mid, 50.0, false, why));
    OK(m.size() == before.size() && valid(m));
    for (std::size_t i = 0; i < m.size(); ++i) {
        OK(m[i].at == before[i].at);
        if (i == mid) OK(std::fabs(m[i].bpm - (before[i].bpm + 5.0f)) <= 0.005f);
        else OK(m[i].bpm == before[i].bpm);
    }
    // The ramp's own bounds hold: dragging a step to the ceiling stays valid.
    OK(apply_tempo_drag(m, kRate, mid, 1e9, true, why) && m[mid].bpm == kMaxBpm && valid(m));
    // Removing a step leaves a map the format allows.
    OK(remove_mark(m, kRate, mid, why) && valid(m) && m.size() == before.size() - 1);
    return 0;
}

}  // namespace

int main() {
    if (test_hit() || test_slop() || test_drag_clamp() || test_first_entry() || test_meter() ||
        test_ramp_staircase()) {
        return 1;
    }
    std::printf("ui conductor: ok\n");
    return 0;
}
