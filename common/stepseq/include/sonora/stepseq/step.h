#pragma once

// One cell of a step pattern (ADR-0169). Plain data, copied whole between the
// UI's editing copy and the audio snapshot, so it carries no pointers.

#include <cstdint>

namespace sonora::stepseq {

// Nudge is in ticks of this resolution so it survives tempo changes.
constexpr int kTicksPerBeat = 960;

constexpr int kMaxVelocity = 127;
constexpr int kMaxProbability = 100;
constexpr int kMaxPitchSemitones = 24;

struct Step {
    bool on = false;
    std::uint8_t velocity = 100;    // 0..127
    std::uint8_t probability = 100; // percent, 0..100
    float gate = 0.5f;              // fraction of one step, (0, 1]
    std::int8_t pitch_semitones = 0; // -24..+24
    std::int16_t nudge_ticks = 0;   // clamped by Pattern to half a step
};

// Brings a step into its documented ranges; the editor calls it on input so
// the player never has to defend against out-of-range data.
inline Step clamped(Step s) noexcept {
    if (s.velocity > kMaxVelocity) s.velocity = kMaxVelocity;
    if (s.probability > kMaxProbability) s.probability = kMaxProbability;
    if (!(s.gate > 0.0f)) s.gate = 1.0f / 64.0f; // also catches NaN
    if (s.gate > 1.0f) s.gate = 1.0f;
    if (s.pitch_semitones > kMaxPitchSemitones) s.pitch_semitones = kMaxPitchSemitones;
    if (s.pitch_semitones < -kMaxPitchSemitones) s.pitch_semitones = -kMaxPitchSemitones;
    return s;
}

} // namespace sonora::stepseq
