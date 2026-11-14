// SPDX-License-Identifier: GPL-3.0-or-later
//
// Groove templates (ADR-0115): per grid slot of a beat, a timing offset and a
// gain factor. They come from a swing preset or from the onsets of a
// reference region, and groove_moves() turns one into the list of region
// moves that puts regions on it. Pure values and functions over MusicalMap;
// never on the audio thread (the engine sees ordinary MoveRegion results).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "sonora/model/model.h"
#include "sonora/model/musical_map.h"

namespace sonora::groove {

constexpr std::size_t kMaxGrooveSlots = 64;
constexpr double kMaxSlotOffset = 0.5;

struct GrooveSlot {
    double offset = 0.0;  // fraction of a slot, within +-kMaxSlotOffset
    double gain = 1.0;    // carried for the template; moves do not use it
};

struct GrooveTemplate {
    std::size_t slots_per_beat = 0;
    std::vector<GrooveSlot> slots;  // slots_per_beat entries
};

struct GrooveMove {
    model::Id region = 0;
    model::Frames position = 0;
};

struct GrooveResult {
    std::vector<GrooveMove> moves;  // only regions whose position changes
    std::size_t skipped = 0;        // moves left out because they would overlap
};

// Odd slots of each pair are delayed so the pair splits at `percent` (50 is
// straight, 66 is triplet swing). `slots_per_beat` is even, 2..64; percent
// 50..75 keeps the offset inside half a slot.
bool groove_swing(double percent, std::size_t slots_per_beat, GrooveTemplate& out,
                  std::string& why);

// Folds each onset (timeline frame) onto the beat grid in force there and
// takes the median offset per slot; a slot without onsets stays 0.
bool groove_from_onsets(const model::MusicalMap& map, const std::vector<model::Frames>& onsets,
                        std::size_t slots_per_beat, GrooveTemplate& out, std::string& why);

// `regions` are the regions of one playlist. Each moves to its nearest slot
// plus offset * strength (percent, 0 < strength <= 100). A move that would
// overlap the neighbour's resulting position, or go before 0, is skipped and
// counted. Refuses strength 0, no regions, an invalid template and an empty
// map, with the reason.
bool groove_moves(const model::MusicalMap& map, const GrooveTemplate& tmpl,
                  const std::vector<model::Region>& regions, double strength,
                  GrooveResult& out, std::string& why);

}  // namespace sonora::groove
