// SPDX-License-Identifier: GPL-3.0-or-later
//
// Phaser and flanger (ADR-0011 §6, N0), one processor with two modes.
//
// Phaser: a cascade of 2 to 12 first-order allpass stages whose break
// frequency sweeps exponentially around a centre; the notches appear where
// the cascade's phase reaches an odd multiple of 180 degrees against the dry
// signal. Each stage is H(z) = (a + z^-1) / (1 + a z^-1) with
// a = (tan(pi f / fs) - 1) / (tan(pi f / fs) + 1), which puts exactly -90
// degrees at f (the bilinear map is exact at the design frequency; the
// warping above it only bends where the upper notches sit). J. O. Smith,
// "Introduction to Digital Filters", first-order allpass; the time-varying
// coefficient is the same design recomputed per sample.
//
// Flanger: a short delay swept exponentially, read with third-order
// Lagrange interpolation (Laakso et al. 1996), summed with the dry signal.
//
// Both: signed feedback, an LFO (sine or triangle) whose right channel is
// offset by the spread, mix. No latency: the dry path is direct.
#pragma once

#include <memory>

#include "sonora/native/processor.h"

namespace sonora::native {

std::unique_ptr<Processor> make_phaser();

namespace phaser {
enum : std::uint32_t {
    kBypass = 0,
    kMode,
    kRate,
    kDepth,
    kFrequency,
    kDelay,
    kStages,
    kSpread,
    kFeedback,
    kShape,
    kMix,
    kParamCount,
};
}  // namespace phaser

}  // namespace sonora::native
