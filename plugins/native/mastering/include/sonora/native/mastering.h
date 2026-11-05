// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mastering console (ADR-0011 N4, ADR-0046): the master chain as one
// processor, built from our own stages rather than a second implementation
// of them:
//
//   input -> EQ -> compressor -> saturation -> width -> gain -> limiter
//         -> loudness meter (-> slow auto-level trim on the gain)
//
// - EQ: the Sonora EQ in its zero-latency mode, used as a low shelf
//   (100 Hz), two bells (Low-mid, High-mid; the frequency is a parameter,
//   the Q is fixed at 0.7) and a high shelf (10 kHz). A mastering EQ is a
//   handful of broad moves, which is what the rule-based suggestion pass
//   (plugins/analyze) proposes.
// - Compressor: the Sonora compressor, 12 dB knee, linked, slow by
//   default; ratio 1 is a transparent setting.
// - Saturation: the Sonora saturator at its eco quality, soft curve; Mix 0
//   is the dry signal exactly.
// - Width: mid/side, 0 (mono) to 200 %; 100 % is the identity to the bit.
// - Gain: -12 to +12 dB ahead of the limiter, plus the auto-level trim.
// - Limiter: the Sonora true-peak limiter, which never exceeds its ceiling.
// - Auto-level: measures the short-term loudness (BS.1770 / EBU Tech 3341)
//   of the OUTPUT and moves a trim of at most +-12 dB, by at most
//   0.9 dB per second, toward Target. It starts after the first 3 s of
//   programme (the short-term window), is off by default, and is stateful:
//   a render with it on depends on the programme before it. Gain + trim is
//   the gain ahead of the limiter.
//
// Latency is the sum of the stages' (the limiter's look-ahead and
// true-peak guard, the saturator's oversampler), reported exactly and fixed
// at prepare().
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_mastering();
const Descriptor& mastering_descriptor();

namespace mastering {
enum : std::uint32_t {
    kBypass = 0,
    kLowGain,  // low shelf at 100 Hz
    kLowMidFreq,
    kLowMidGain,
    kHighMidFreq,
    kHighMidGain,
    kHighGain,  // high shelf at 10 kHz
    kCompThreshold,
    kCompRatio,
    kCompAttack,
    kCompRelease,
    kCompMix,
    kSatDrive,
    kSatMix,
    kWidth,
    kGain,
    kCeiling,
    kLimitRelease,
    kTargetLufs,
    kAutoLevel,
    kParamCount,
};
constexpr double kLowShelfHz = 100.0;
constexpr double kHighShelfHz = 10000.0;
constexpr double kBellQ = 0.7;
// Auto-level: every 100 ms the trim target moves by kTrimGain times the
// error in LU, limited to kTrimStepDb (so 0.9 dB per second at most), within
// +-kTrimRangeDb.
constexpr double kTrimRangeDb = 12.0;
constexpr double kTrimStepDb = 0.09;
constexpr double kTrimGain = 0.03;
}  // namespace mastering

}  // namespace sonora::native
