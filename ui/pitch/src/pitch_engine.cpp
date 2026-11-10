// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/ui_pitch/pitch_engine.h"

#include <algorithm>

namespace sonora::ui_pitch {
namespace {

bool contains(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

}  // namespace

PitchCapability pitch_capability(const std::vector<std::string>& implemented_commands,
                                 const std::vector<std::string>& capabilities) {
    PitchCapability c;
    if (!contains(implemented_commands, kSampleEditCommand)) {
        c.reason = "This engine does not edit samples (no sample.edit), so it cannot render a "
                   "pitch edit. Pitch editing is off.";
        return c;
    }
    if (!contains(capabilities, api::kCapabilityPitch)) {
        c.reason = "This engine was built without a pitch shifter, so it cannot render a pitch "
                   "edit. Pitch editing is off.";
        return c;
    }
    c.available = true;
    return c;
}

std::string pitch_edit_argument(uint64_t region_id, uint64_t region_position,
                                uint64_t region_length, const api::PitchEditSpec& spec) {
    return std::string(api::kPitchVerb) + ' ' + std::to_string(region_id) + ' ' +
           std::to_string(region_position) + ' ' + std::to_string(region_position + region_length) +
           " all " + api::format_pitch_param(spec);
}

}  // namespace sonora::ui_pitch
