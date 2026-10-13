// SPDX-License-Identifier: GPL-3.0-or-later
//
// x86-64: FTZ (MXCSR bit 15) and DAZ (bit 6). SSE2 is part of the x86-64
// baseline, so DAZ is always present. MXCSR governs scalar double SSE
// arithmetic too, which is what the compiler emits for double on x86-64.
#include "sonora/dsp/denormals.h"

#include <xmmintrin.h>

namespace sonora::dsp {

namespace {
constexpr unsigned kFtz = 0x8000u;
constexpr unsigned kDaz = 0x0040u;
}  // namespace

FpState enable_flush_to_zero() noexcept {
    FpState saved;
    saved.bits = _mm_getcsr();
    _mm_setcsr(saved.bits | kFtz | kDaz);
    return saved;
}

void restore_fp_state(FpState state) noexcept { _mm_setcsr(state.bits); }

bool flush_to_zero_supported() noexcept { return true; }

bool flush_to_zero_active() noexcept {
    return (_mm_getcsr() & (kFtz | kDaz)) == (kFtz | kDaz);
}

}  // namespace sonora::dsp
