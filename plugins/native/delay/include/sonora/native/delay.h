// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stereo delay (ADR-0011 §6, N0): times in milliseconds or as note values
// (straight, dotted, triplet) synced to the host's tempo, or to the tempo
// parameter when the host gives none (§4.4: until the tempo map exists);
// feedback through a high-pass and a low-pass in the loop; ping-pong; mix.
//
// Delay times are whole samples, so an echo lands exactly where the note
// value says. A change of time crossfades between the old and the new tap
// over 30 ms (a raised cosine) instead of gliding: no pitch sweep, no
// click, and automation of the time or the tempo is safe at any rate.
// The delay is the effect, not latency: the dry path is undelayed and the
// processor reports 0.
//
// References: U. Zolzer (ed.), *DAFX*, 2nd ed., Wiley, 2011, ch. 2;
// J. Dattorro, "Effect Design Part 2", JAES 45(10), 1997.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_delay();

namespace delay {
enum : std::uint32_t {
    kBypass = 0,
    kSync,
    kTimeL,
    kNoteL,
    kLink,
    kTimeR,
    kNoteR,
    kTempo,
    kFeedback,
    kPingPong,
    kHpf,
    kLpf,
    kMix,
    kParamCount,
};
// Note values in quarter-note beats, shortest first; the parameter is an
// index into this table. "T" is triplet (2/3), "D" dotted (3/2).
constexpr double kNoteBeats[] = {
    1.0 / 16,  // 1/64
    1.0 / 12,  // 1/32 T
    1.0 / 8,   // 1/32
    1.0 / 6,   // 1/16 T
    3.0 / 16,  // 1/32 D
    1.0 / 4,   // 1/16
    1.0 / 3,   // 1/8 T
    3.0 / 8,   // 1/16 D
    1.0 / 2,   // 1/8
    2.0 / 3,   // 1/4 T
    3.0 / 4,   // 1/8 D
    1.0,       // 1/4
    4.0 / 3,   // 1/2 T
    3.0 / 2,   // 1/4 D
    2.0,       // 1/2
    8.0 / 3,   // 1/1 T
    3.0,       // 1/2 D
    4.0,       // 1/1
};
constexpr std::uint32_t kNoteCount = sizeof(kNoteBeats) / sizeof(kNoteBeats[0]);
constexpr double kMaxSeconds = 4.0;
}  // namespace delay

}  // namespace sonora::native
