// SPDX-License-Identifier: GPL-3.0-or-later
//
// From detected beats to a tempo map (ADR-0114, applied as ADR-0050's
// SetSettings). A pure function: the interface calls it on a report it
// read and hands the map to the existing tempo operations' checks. The
// definition belongs to the model slice of time.beat_detection; this is
// the contract only.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/model/model.h"

namespace sonora::analyze {

enum class TempoFromBeatsMode {
    // One entry at `origin` + the first beat, at the report's global bpm.
    constant,
    // One entry per bar from the beat grid, so a drifting take keeps its
    // bars on the beats; offered only for a result that is not stable.
    follow,
};

// `beats` are source frames at `sample_rate` (a BeatResult's); `origin` is
// the session frame of source frame 0. `meter` is carried by every entry
// produced. Writes the whole new map to `out`, checked against
// tempo_map_problem, or returns false with `why` and leaves `out` alone
// (fewer than two beats, a beat before -origin, a bpm outside the format).
bool tempo_from_beats(const std::vector<std::int64_t>& beats, std::uint32_t sample_rate, double bpm,
                      sonora::model::Frames origin, std::array<std::uint32_t, 2> meter, TempoFromBeatsMode mode,
                      std::vector<sonora::model::TempoEntry>& out, std::string& why);

}  // namespace sonora::analyze
