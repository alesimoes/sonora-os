// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PitchShifter over Rubber Band (GPL-2.0-or-later; ADR-0143 section 4),
// built only when pkg-config finds it. Offline: each note is rendered on its
// own with its pitch scale and some context either side, then laid over the
// untouched input with a short crossfade at its edges, so the result has
// exactly the input's length and nothing outside the notes changes.
#pragma once

#include "sonora/pitch/shifter.h"

namespace sonora {

class RubberBandPitchShifter : public pitch::PitchShifter {
public:
    bool shift(const pitch::Planar& input, uint32_t rate, const api::PitchEditSpec& spec,
               pitch::Planar& output, std::string& why) override;
};

}  // namespace sonora
