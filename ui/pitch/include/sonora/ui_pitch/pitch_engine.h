// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the Pitch editor needs from the engine (time.pitch, ADR-0143 §2):
// whether it can render a pitch edit at all, and a way to send the one
// command Apply is. The editor never talks to the engine otherwise, so a test
// drives it with the fake engine behind a PitchCommandSink.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sonora/api/pitch.h"

namespace sonora::ui_pitch {

inline constexpr const char* kSampleEditCommand = "sample.edit";

// Pitch editing is available only when the engine runs sample.edit AND says
// it was built with a shifter: sample.edit alone does not imply the verb.
struct PitchCapability {
    bool available = false;
    std::string reason;  // why not, for the operator; empty when available
};

PitchCapability pitch_capability(const std::vector<std::string>& implemented_commands,
                                 const std::vector<std::string>& capabilities);

// Sends one engine command. Returns the request's sequence number, which the
// reply carries back; 0 when nothing could be sent, with `why` filled.
class PitchCommandSink {
public:
    virtual ~PitchCommandSink() = default;
    virtual uint64_t send(const std::string& command, const std::string& argument,
                          std::string& why) = 0;
};

// The sample.edit argument for a pitch edit of a whole region:
// "pitch REGION FROM TO all PARAM". FROM and TO are timeline frames, as for
// every sample.edit verb; the notes in PARAM are frames from the region's
// start (pitch.h).
std::string pitch_edit_argument(uint64_t region_id, uint64_t region_position,
                                uint64_t region_length, const api::PitchEditSpec& spec);

}  // namespace sonora::ui_pitch
