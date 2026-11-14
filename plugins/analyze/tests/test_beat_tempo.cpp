// SPDX-License-Identifier: GPL-3.0-or-later
//
// tempo_from_beats (ADR-0114): constant and follow modes, merge, cap,
// refusals, and the MusicalMap round trip.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sonora/analyze/beat_tempo.h"
#include "sonora/model/musical_map.h"
#include "sonora/model/tempo_edit.h"

using namespace sonora::analyze;
using sonora::model::TempoEntry;

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}
constexpr std::uint32_t kRate = 48000;
const std::array<std::uint32_t, 2> k44{{4, 4}};

std::vector<std::int64_t> grid(std::int64_t start, double period, int n) {
    std::vector<std::int64_t> b;
    for (int i = 0; i < n; ++i) b.push_back(start + std::llround(period * i));
    return b;
}

void constant_round_trip() {
    const auto b = grid(24000, 24000.0, 64);  // 120 bpm, first beat at 0.5 s
    std::vector<TempoEntry> map;
    std::string why;
    check(tempo_from_beats(b, kRate, 120.0, 0, k44, TempoFromBeatsMode::constant, map, why), "constant ok");
    check(map.size() == 2 && map[0].at == 0 && map[1].at == 24000, "lead-in then the first beat");
    sonora::model::MusicalMap mm;
    check(mm.build(kRate, map, why), "map builds");
    sonora::model::Ticks t0 = 0;
    check(mm.tick_at(24000, t0), "tick at first beat");
    check(mm.bars_beats_at(t0).bar == 2 && mm.bars_beats_at(t0).beat == 1, "first beat is a downbeat");
    bool within = true;
    for (std::size_t n = 0; n < b.size(); ++n) {
        sonora::model::Frames f = 0;
        const sonora::model::Ticks t = t0 + static_cast<sonora::model::Ticks>(n) * sonora::model::kTicksPerQuarter;
        if (!mm.frame_at(t, f) || std::llabs(f - b[n]) > 1) within = false;
    }
    check(within, "beat n within 1 frame of the detected frame");
}

void constant_at_zero_and_origin() {
    std::vector<TempoEntry> map;
    std::string why;
    check(tempo_from_beats(grid(0, 24000.0, 8), kRate, 120.0, 0, k44, TempoFromBeatsMode::constant, map, why) &&
              map.size() == 1 && map[0].bpm == 120.0f,
          "first beat at 0: one entry");
    check(tempo_from_beats(grid(0, 24000.0, 8), kRate, 120.0, 48000, k44, TempoFromBeatsMode::constant, map, why) &&
              map.size() == 2 && map[1].at == 48000,
          "origin shifts the anchor");
}

std::vector<std::int64_t> drifting(int n, double from_bpm, double to_bpm) {
    std::vector<std::int64_t> b;
    double f = 0;
    for (int i = 0; i < n; ++i) {
        b.push_back(std::llround(f));
        const double bpm = from_bpm + (to_bpm - from_bpm) * i / (n - 1);
        f += 60.0 * kRate / bpm;
    }
    return b;
}

void follow_staircase() {
    const auto b = drifting(64, 100.0, 140.0);
    std::vector<TempoEntry> map;
    std::string why;
    check(tempo_from_beats(b, kRate, 120.0, 0, k44, TempoFromBeatsMode::follow, map, why), "follow ok");
    check(map.size() > 4 && map.size() <= 16, "a staircase, one entry per bar");
    check(map.front().at == 0 && map.front().bpm < map.back().bpm, "tempo rises");
    for (std::size_t i = 1; i < map.size(); ++i) check(map[i].at > map[i - 1].at, "ascending");
    sonora::model::MusicalMap mm;
    check(mm.build(kRate, map, why), "builds");
    bool near = true;
    for (std::size_t n = 0; n < b.size(); n += 4) {
        sonora::model::Frames f = 0;
        const sonora::model::Ticks t = static_cast<sonora::model::Ticks>(n) * sonora::model::kTicksPerQuarter;
        if (!mm.frame_at(t, f) || std::llabs(f - b[n]) > 2) near = false;
    }
    check(near, "bar lines on the beats");
}

void follow_merges_steady() {
    std::vector<TempoEntry> map;
    std::string why;
    check(tempo_from_beats(grid(0, 24000.0, 64), kRate, 120.0, 0, k44, TempoFromBeatsMode::follow, map, why) &&
              map.size() == 1,
          "a steady take merges to one entry");
    std::vector<std::int64_t> b;
    double f = 0;
    for (int i = 0; i < 33; ++i) {
        b.push_back(std::llround(f));
        f += 24000.0 * (i < 16 ? 1.0 : 1.02);
    }
    check(tempo_from_beats(b, kRate, 120.0, 0, k44, TempoFromBeatsMode::follow, map, why) && map.size() == 2,
          "a 2 percent step is kept");
}

void follow_cap() {
    const auto b = drifting(2000, 60.0, 200.0);
    std::vector<TempoEntry> map;
    std::string why;
    check(tempo_from_beats(b, kRate, 120.0, 0, k44, TempoFromBeatsMode::follow, map, why), "long take ok");
    check(map.size() <= sonora::model::kMaxRampSteps && map.size() > 8, "capped at kMaxRampSteps");
}

void refusals() {
    std::vector<TempoEntry> map{{0, 99.0f, {{3, 4}}}};
    const auto keep = map;
    std::string why;
    auto refused = [&](const std::vector<std::int64_t>& b, double bpm, sonora::model::Frames origin,
                       const char* what) {
        why.clear();
        check(!tempo_from_beats(b, kRate, bpm, origin, k44, TempoFromBeatsMode::constant, map, why) &&
                  !why.empty() && map == keep,
              what);
    };
    refused({0}, 120.0, 0, "one beat");
    refused({100, 100, 200}, 120.0, 0, "not ascending");
    refused(grid(0, 24000.0, 8), 0.5, 0, "bpm too low");
    refused(grid(0, 24000.0, 8), 1500.0, 0, "bpm too high");
    refused(grid(0, 24000.0, 8), std::nan(""), 0, "bpm nan");
    refused(grid(0, 24000.0, 8), 120.0, -1, "beat before the start");
    refused(grid(10, 24000.0, 8), 120.0, 0, "first beat too close to start to anchor");
}

}  // namespace

int main() {
    constant_round_trip();
    constant_at_zero_and_origin();
    follow_staircase();
    follow_merges_steady();
    follow_cap();
    refusals();
    if (g_failures == 0) std::printf("analyze_beat_tempo: ok\n");
    return g_failures == 0 ? 0 : 1;
}
