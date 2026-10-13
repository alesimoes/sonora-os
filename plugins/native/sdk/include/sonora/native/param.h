// SPDX-License-Identifier: GPL-3.0-or-later
//
// The parameter table (ADR-0011 §2.4). Each processor declares one static
// table; its LV2 and VST3 parameter info, the JSON descriptor the UI reads
// and the preset schema are all generated from it, so they cannot drift.
//
// Ids are append-only, like ADR-0007's ParamId: an id is never renamed,
// reused or removed, and a table only grows at the end. Renaming one breaks
// every saved session and needs a state migration with a version bump.
#pragma once

#include <cstdint>

#include "sonora/rt.h"

namespace sonora::native {

enum class Unit : std::uint8_t {
    None,
    Decibel,
    Hertz,
    Milliseconds,
    Percent,
    Ratio,  // n:1
    Q,
    Bpm,
    Choice,  // an index into `choices`
    Toggle,  // 0 or 1
};

enum class Taper : std::uint8_t {
    Linear,
    Log,      // smoothed and displayed on a log scale; min > 0
    Stepped,  // integer values; Choice and Toggle are stepped
};

struct ParamInfo {
    const char* id;  // stable, append-only
    const char* name;
    Unit unit;
    Taper taper;
    float min, max, def;
    // Ramp length for a change. Zero only for stepped values that do not
    // reach the audio as a step (they are declicked instead, below).
    float smoothing_ms;
    bool automatable;
    // Takes effect at the next prepare() and may change latency
    // (ADR-0011 §1.5: latency changes only on a non-RT reconfiguration).
    bool reconfigures;
    // A structural switch (routing, filter order, detector type) that would
    // step the audio: the base fades the output out, applies it, and fades
    // back in, rather than click.
    bool declick;
    const char* format;  // printf-style display format, e.g. "%.1f dB"
    const char* const* choices;
    std::uint32_t choice_count;
};

// Clamps to the range and rounds stepped values; rejects non-finite input
// by returning the default.
float sanitize(const ParamInfo& p, float value) noexcept SONORA_RT;
// Position in [0, 1] on the parameter's taper, and back. For hosts and
// for full-range sweeps in the zipper test.
double to_normalized(const ParamInfo& p, double value) noexcept;
double from_normalized(const ParamInfo& p, double normalized) noexcept;

// Every table starts with this entry, index 0. The base class implements
// it: latency-compensated (ADR-0011 §1.5) and crossfaded.
extern const ParamInfo kBypassParam;

}  // namespace sonora::native
