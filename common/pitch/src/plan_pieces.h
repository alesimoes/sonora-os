// SPDX-License-Identifier: GPL-3.0-or-later
//
// Internal to common/pitch: how a note and its edit become constant-shift
// pieces (see plan.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sonora/pitch/plan.h"

namespace sonora::pitch {

struct Piece {
    uint64_t start = 0;
    uint64_t end = 0;
    int32_t cents = 0;
};

// One piece when the edit leaves drift and vibrato alone, otherwise pieces of
// piece_frames contour frames each.
std::vector<Piece> pieces_for(const Note& note, const NoteEdit& edit, std::size_t piece_frames);

}  // namespace sonora::pitch
