// SPDX-License-Identifier: GPL-3.0-or-later
//
// Flush-to-zero and denormals-are-zero for the audio thread.
//
// A recursive filter decaying into silence walks its state through the
// subnormal range, where x86 arithmetic runs one to two orders of magnitude
// slower. That is a CPU spike at the quietest moment of a session, so every
// Processor::process enables FTZ/DAZ for its duration (ADR-0011 §3.2,
// "Denormals").
//
// The implementation is chosen by CMake from the target architecture
// (src/arch/<arch>/), never by #ifdef (ADR-0011 §2.3, ADR-0008 §1). The
// generic one does nothing and says so through flush_to_zero_supported();
// processors keep their state in double, which pushes the subnormal range
// down to 1e-308 and makes the generic case slow only in theory.
#pragma once

#include <cstdint>

#include "sonora/rt.h"

namespace sonora::dsp {

// The floating-point control state before enable_flush_to_zero().
struct FpState {
    std::uint32_t bits = 0;
};

FpState enable_flush_to_zero() noexcept SONORA_RT;
void restore_fp_state(FpState state) noexcept SONORA_RT;
bool flush_to_zero_supported() noexcept;
// Whether FTZ is in force on this thread right now. For tests.
bool flush_to_zero_active() noexcept;

// Enables FTZ/DAZ for a scope and restores the caller's state after, so a
// host thread that wanted IEEE behaviour gets it back.
class DenormalGuard {
public:
    DenormalGuard() noexcept SONORA_RT : saved_(enable_flush_to_zero()) {}
    ~DenormalGuard() { restore_fp_state(saved_); }
    DenormalGuard(const DenormalGuard&) = delete;
    DenormalGuard& operator=(const DenormalGuard&) = delete;

private:
    FpState saved_;
};

}  // namespace sonora::dsp
