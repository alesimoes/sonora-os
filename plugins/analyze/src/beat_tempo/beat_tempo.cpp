// SPDX-License-Identifier: GPL-3.0-or-later
//
// tempo_from_beats (ADR-0114): a tempo map from detected beats. Pure and off
// the audio thread. The format's first entry must sit at frame 0 and bars are
// counted from it (session-format.md), so a first beat after 0 needs a lead-in
// entry whose single bar lasts exactly until that beat: the first beat then
// starts a bar and the grid is anchored on it. The lead-in's tempo differs
// from the take's; it is silence or count-in, not music.
#include "sonora/analyze/beat_tempo.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "sonora/model/musical_map.h"
#include "sonora/model/tempo_edit.h"

namespace sonora::analyze {
namespace {

using sonora::model::Frames;
using sonora::model::TempoEntry;

// Entries closer than this in tempo are one entry: a drifting hand is not
// followed beat for beat, only where it moved (ADR-0114).
constexpr double kMergeTolerance = 0.005;

bool fail(std::string& why, const std::string& reason) {
    why = reason;
    return false;
}

bool check_input(const std::vector<std::int64_t>& beats, std::uint32_t rate, double bpm, Frames origin,
                 std::array<std::uint32_t, 2> meter, std::string& why) {
    if (beats.size() < 2) return fail(why, "needs at least two beats");
    if (rate == 0) return fail(why, "sample rate must be greater than 0");
    if (!(std::isfinite(bpm) && bpm >= sonora::model::kMinBpm && bpm <= sonora::model::kMaxBpm)) {
        return fail(why, "bpm must be from 1 to 1000");
    }
    if (meter[0] == 0 || meter[0] > 0xFFFF || !sonora::model::is_meter_denominator(meter[1])) {
        return fail(why, "meter is outside the format");
    }
    for (std::size_t i = 0; i < beats.size(); ++i) {
        if (origin + beats[i] < 0) return fail(why, "a beat falls before the start of the session");
        if (i > 0 && beats[i] <= beats[i - 1]) return fail(why, "beats must be in ascending order");
    }
    return true;
}

// The entry that makes `first` (> 0) the start of a bar: one bar of m beats
// ending exactly there.
bool lead_in(Frames first, double bpm, std::uint32_t rate, std::array<std::uint32_t, 2> meter,
             std::vector<TempoEntry>& map, std::string& why) {
    const double m = std::max(1.0, std::round(static_cast<double>(first) * bpm / (60.0 * rate)));
    if (m > 0xFFFF) return fail(why, "the first beat is too long after the start to anchor a bar");
    const double lead_bpm = m * 60.0 * rate / static_cast<double>(first);
    if (lead_bpm < sonora::model::kMinBpm || lead_bpm > sonora::model::kMaxBpm) {
        return fail(why, "the first beat is too close to the start to anchor a bar");
    }
    map.push_back({0, static_cast<float>(lead_bpm), {{static_cast<std::uint32_t>(m), meter[1]}}});
    return true;
}

// Groups of `group` beats from beat 0; an entry at each group's first beat
// carries the tempo of the intervals in it; the last beat closes the last group.
void follow_groups(const std::vector<std::int64_t>& beats, Frames origin, std::uint32_t rate,
                   std::array<std::uint32_t, 2> meter, std::size_t group, std::vector<TempoEntry>& map) {
    const std::size_t lead = map.size();
    const std::size_t n = beats.size();
    for (std::size_t s = 0; s + 1 < n; s += group) {
        std::size_t e = std::min(s + group, n - 1);
        const double per_beat = static_cast<double>(beats[e] - beats[s]) / static_cast<double>(e - s);
        const double bpm = 60.0 * rate / per_beat;
        if (map.size() > lead && std::fabs(bpm - map.back().bpm) <= kMergeTolerance * map.back().bpm) {
            if (e == n - 1) break;
            continue;
        }
        map.push_back({origin + beats[s], static_cast<float>(bpm), meter});
        if (e == n - 1) break;
    }
}

}  // namespace

bool tempo_from_beats(const std::vector<std::int64_t>& beats, std::uint32_t sample_rate, double bpm,
                      sonora::model::Frames origin, std::array<std::uint32_t, 2> meter, TempoFromBeatsMode mode,
                      std::vector<sonora::model::TempoEntry>& out, std::string& why) {
    if (!check_input(beats, sample_rate, bpm, origin, meter, why)) return false;
    const Frames first = origin + beats.front();
    std::vector<TempoEntry> map;
    if (first > 0 && !lead_in(first, bpm, sample_rate, meter, map, why)) return false;
    const std::size_t lead = map.size();

    if (mode == TempoFromBeatsMode::constant) {
        map.push_back({first, static_cast<float>(bpm), meter});
    } else {
        std::size_t group = meter[0];
        for (;;) {
            map.resize(lead);
            follow_groups(beats, origin, sample_rate, meter, group, map);
            if (map.size() <= sonora::model::kMaxRampSteps || group >= beats.size()) break;
            group *= 2;
        }
    }

    std::string where;
    const std::string problem = sonora::model::tempo_map_problem(sample_rate, map, where);
    if (!problem.empty()) return fail(why, where + ": " + problem);
    out = std::move(map);
    why.clear();
    return true;
}

}  // namespace sonora::analyze
