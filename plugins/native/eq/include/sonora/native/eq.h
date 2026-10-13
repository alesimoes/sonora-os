// SPDX-License-Identifier: GPL-3.0-or-later
//
// Parametric EQ: high-pass and low-pass at 6 to 48 dB/oct, a low and a high
// shelf, four bells, output gain (ADR-0011 §6, N0).
//
// Every band is a matched second-order design (common/dsp/biquad.h,
// Vicanek 2016) run as a trapezoidal SVF whose coefficients are ramped, so
// automation never zippers or blows up.
//
// Two modes, a reconfiguring parameter:
// - "analog matched" (default): the bands run at twice the session rate
//   inside a linear-phase half-band oversampler. Every band is within
//   0.1 dB of its analog prototype from 20 Hz to 20 kHz at 44.1 and 48 kHz
//   (ADR-0011 §3.2). Latency: the oversampler's, reported exactly.
// - "zero latency": the same designs at the session rate. No latency, but
//   no single second-order section can hold 0.1 dB for a wide bell above a
//   few kHz at 44.1 or 48 kHz (common/dsp/tests/test_filters.cpp); the
//   deviation is reported, not hidden.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_eq();
const Descriptor& eq_descriptor();

// Parameter indices, fixed by the table (append-only).
namespace eq {
enum : std::uint32_t {
    kBypass = 0,
    kMode,
    kOutput,
    kHpfOn,
    kHpfFreq,
    kHpfSlope,
    kLpfOn,
    kLpfFreq,
    kLpfSlope,
    kLowShelfOn,
    kLowShelfFreq,
    kLowShelfGain,
    kLowShelfQ,
    kHighShelfOn,
    kHighShelfFreq,
    kHighShelfGain,
    kHighShelfQ,
    kBell1On,  // then freq, gain, q; bells 2-4 follow at +4 each
    kParamCount = kBell1On + 16,
};
constexpr std::uint32_t bell(int b, int field) { return kBell1On + 4 * b + field; }
// Slope choices, in dB/oct.
constexpr int kSlopes[] = {6, 12, 18, 24, 36, 48};
}  // namespace eq

}  // namespace sonora::native
