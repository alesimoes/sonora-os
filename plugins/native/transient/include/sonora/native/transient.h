// SPDX-License-Identifier: GPL-3.0-or-later
//
// Transient shaper (ADR-0011 N0, plugins.stock): raises or lowers the attack
// and the sustain of a signal independently of its level, which is how a
// close-miked snare is given more crack or a roomy drum bus is tightened
// without a compressor's threshold.
//
// Differential envelope detection: a fast and a slow peak follower run on
// the rectified signal; their ratio in dB is positive while the level is
// rising faster than the slow follower can follow (the attack), negative
// while it falls faster than the slow one (the decay and release), and near
// zero for a steady tone. Because the ratio is taken in dB, the result does
// not depend on the input level. The attack and sustain gains are applied
// by their weights, outside a 2 dB dead zone that keeps the detectors'
// ripple on a low steady tone from modulating it, and smoothed over 2 ms.
// The design and its constants are ours, measured in tests/suite.cpp; it
// follows no commercial unit's output. No look-ahead and no latency: the
// first 1 ms of an onset is not boosted.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_transient();

namespace transient {
enum : std::uint32_t {
    kBypass = 0,
    kAttack,
    kSustain,
    kSpeed,
    kLink,
    kGain,
    kParamCount,
};
}  // namespace transient

}  // namespace sonora::native
