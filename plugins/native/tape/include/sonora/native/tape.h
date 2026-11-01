// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tape: record and playback through magnetic tape (ADR-0011 §6, N2
// slice 4), from the published physics (common/dsp tape.h cites each law).
//
//   input -> drive -> record pre-emphasis -> Jiles-Atherton hysteresis
//         -> playback de-emphasis (exact inverse) -> head gap and spacing
//         loss -> [oversampler down] -> head bump -> [wow and flutter]
//         -> output
//
// What the parameters mean, measured by tests/suite.cpp:
// - Drive: the record level. At 0 dB a full-scale input drives the tape
//   into mild compression; towards +24 dB the third harmonic rises and
//   the high frequencies, pre-emphasised, saturate first.
// - Speed: 7.5, 15 or 30 in/s. Sets the head's gap and spacing loss, the
//   head bump's frequency and the equalisation time constant.
// - EQ: NAB or IEC record and playback time constants (30 in/s is AES
//   17.5 us for both). The linear response is flat whichever is chosen,
//   by construction; the choice moves where the tape saturates.
// - Head bump: 0 to 100 %, the low-frequency ripple of the playback head
//   (up to 3 dB), at the frequency the speed gives.
// - Motion, wow, flutter: with motion on, the playback is read through a
//   delay modulated by a 0.6 Hz sine (wow) and a 9 Hz sine (flutter); the
//   amounts are the peak speed deviation of each, in percent. Motion
//   changes the latency (a fixed delay that holds the swing), so it
//   reconfigures; off, there is no delay at all.
// - Quality: the oversampling of the nonlinearity (2x, 4x, 8x), with its
//   latency. Reconfigures.
//
// Not yet (planning/BACKLOG.md): tape hiss and the level of the machine's
// own noise, print-through, bias and its own level as parameters, the
// record-head gap loss, azimuth and stereo crosstalk, and a drift component
// of the speed.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_tape();

namespace tape {
enum : std::uint32_t {
    kBypass = 0,
    kDrive,
    kOutput,
    kSpeed,
    kEq,
    kBump,
    kMotion,
    kWow,
    kFlutter,
    kQuality,
    kParamCount,
};
constexpr std::uint32_t kFactors[] = {2, 4, 8};
// Frequencies of the two motion sines (Hz).
constexpr double kWowHz = 0.6;
constexpr double kFlutterHz = 9.0;
// The ends of the wow and flutter ranges (percent peak speed deviation).
constexpr double kWowMaxPercent = 0.3;
constexpr double kFlutterMaxPercent = 0.2;
}  // namespace tape

}  // namespace sonora::native
