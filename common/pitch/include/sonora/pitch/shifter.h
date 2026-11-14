// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pitch shifter seam (time.pitch, ADR-0143 section 4). The real one wraps
// Rubber Band in engine/pitch; here is the interface the engine codes to and a
// deterministic fake for tests. Offline only: never called from `process`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/api/pitch.h"

namespace sonora::pitch {

// One vector per channel, all the same length.
using Planar = std::vector<std::vector<float>>;

class PitchShifter {
public:
    virtual ~PitchShifter() = default;
    // Renders `input` with every note of `spec` shifted; `output` has the same
    // channels and the same length as `input` (the shift never ripples). False
    // with `why` leaves `output` untouched.
    virtual bool shift(const Planar& input, uint32_t rate, const api::PitchEditSpec& spec,
                       Planar& output, std::string& why) = 0;
};

// Applies each note's gain to its span and leaves the pitch alone, so a test
// can see which frames the engine asked to change. Records every call.
class FakePitchShifter : public PitchShifter {
public:
    struct Call {
        std::size_t channels = 0;
        std::size_t frames = 0;
        uint32_t rate = 0;
        api::PitchEditSpec spec;
    };

    bool shift(const Planar& input, uint32_t rate, const api::PitchEditSpec& spec, Planar& output,
               std::string& why) override;

    const std::vector<Call>& calls() const { return calls_; }
    // When set, the next calls refuse with this reason.
    void refuse_with(const std::string& why) { refusal_ = why; }

private:
    std::vector<Call> calls_;
    std::string refusal_;
};

}  // namespace sonora::pitch
