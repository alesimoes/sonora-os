#pragma once

// One lane of a step pattern (ADR-0169): what it triggers and how loud.

#include <cstdint>

namespace sonora::stepseq {

// The id of an existing audio region (or its source sample) in the session
// model; 0 means the row triggers nothing. The pattern never owns audio.
using SourceId = std::uint64_t;
constexpr SourceId kNoSource = 0;

struct Row {
    SourceId source = kNoSource;
    float gain_db = 0.0f; // -inf..+12 dB, applied by whoever voices the trigger
    float pan = 0.0f;     // -1 left .. +1 right
    bool mute = false;
    bool solo = false;
};

} // namespace sonora::stepseq
