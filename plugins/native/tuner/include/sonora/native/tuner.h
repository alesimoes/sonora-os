// SPDX-License-Identifier: GPL-3.0-or-later
//
// Vocal tuner (ADR-0011 §6, the AI stack's real-time half): classical pitch
// detection and pitch shifting, snapped to a key. Nothing in it is learned.
// The key and scale are parameters; the session's key reaches them as
// ordinary parameter automation (ADR-0011 §4), never as hidden processing.
//
//   input -> mono sum -> YIN detector (tuner_pitch.h) -> median of three
//         -> note in the scale -> correction in cents, eased by Speed
//   input -> PSOLA shifter at the eased ratio -> output (latency-matched)
//
// What the parameters mean:
// - Key and Scale: the notes the voice is pulled to. Chromatic pulls to the
//   nearest semitone. A note is kept until the voice is clearly nearer
//   another, so a voice at the middle of a gap does not flip.
// - Speed: the time constant of the correction in milliseconds. 0 is hard
//   (the pitch is placed on the note as soon as it is heard); a few hundred
//   leaves the singer's own glides and vibrato mostly alone.
// - Amount: how much of the distance to the note is corrected (100 %: all).
// - Range: the lowest and highest pitch it looks for. It sets the shifter's
//   grain reach and therefore the latency (reconfigures): a lower range
//   costs more latency. Low 65-500 Hz, medium 100-800 Hz, high 160-1100 Hz.
// - Reference: the pitch of A4 the notes are measured from.
//
// Where the input has no pitch (breath, consonants, silence), the shifter
// is not engaged: the output is the input delayed by exactly the latency.
// Where it has, the delay of a corrected voice wanders within one period of
// the latency (the grains are taken from the voice's own pitch cycles);
// with Amount at 0 it is exactly the latency.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_tuner();

namespace tuner {
enum : std::uint32_t {
    kBypass = 0,
    kKey,
    kScale,
    kSpeed,
    kAmount,
    kRange,
    kReference,
    kParamCount,
};
// Range -> lowest and highest pitch searched (Hz).
constexpr double kRangeMinHz[] = {65.0, 100.0, 160.0};
constexpr double kRangeMaxHz[] = {500.0, 800.0, 1100.0};
// Semitones the chooser keeps the previous note over a nearer one.
constexpr double kHysteresisSemitones = 0.3;
// The detector's result is acted on after this many voiced frames in a row
// (a false positive on noise is isolated), and a lost pitch is held for
// kHoldSeconds before the correction is released over kReleaseSeconds.
constexpr int kConfirmFrames = 2;
constexpr double kHoldSeconds = 0.06;
constexpr double kReleaseSeconds = 0.03;
// The most the correction ever moves a note (cents).
constexpr double kMaxCents = 600.0;
}  // namespace tuner

}  // namespace sonora::native
