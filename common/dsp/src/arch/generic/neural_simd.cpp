// SPDX-License-Identifier: GPL-3.0-or-later
//
// No SIMD kernel table on this architecture: the WaveNet runs the generic
// one, which the compiler vectorises at the baseline width.
#include "../../neural/kernels.h"

namespace sonora::dsp::neural::detail {

const Kernels* simd_kernels() noexcept { return nullptr; }

}  // namespace sonora::dsp::neural::detail
