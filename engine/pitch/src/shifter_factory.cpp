// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one place that knows whether Rubber Band was found: the build sets
// SONORA_PITCH_RUBBERBAND to 0 or 1 (engine/pitch/CMakeLists.txt).
#include "sonora/pitch_render.h"

#if SONORA_PITCH_RUBBERBAND
#include "rubberband_shifter.h"
#endif

namespace sonora {

bool pitch_shifter_built() { return SONORA_PITCH_RUBBERBAND != 0; }

std::unique_ptr<pitch::PitchShifter> make_pitch_shifter() {
#if SONORA_PITCH_RUBBERBAND
    return std::make_unique<RubberBandPitchShifter>();
#else
    return nullptr;
#endif
}

}  // namespace sonora
