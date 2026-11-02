// SPDX-License-Identifier: GPL-3.0-or-later
//
// Portable fallback: standard C++ has no way to set FTZ/DAZ. Callers learn
// that from flush_to_zero_supported(); the denormal test reports it rather
// than passing silently. AArch64 with GCC/Clang has its own
// (src/arch/aarch64/denormals.cpp, FPCR.FZ).
#include "sonora/dsp/denormals.h"

namespace sonora::dsp {

FpState enable_flush_to_zero() noexcept { return FpState{}; }
void restore_fp_state(FpState) noexcept {}
bool flush_to_zero_supported() noexcept { return false; }
bool flush_to_zero_active() noexcept { return false; }

}  // namespace sonora::dsp
