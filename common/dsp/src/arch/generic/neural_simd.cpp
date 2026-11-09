// SPDX-License-Identifier: GPL-3.0-or-later
//
// No SIMD kernel table on this architecture: the WaveNet runs the generic
// one, which the compiler vectorises at the baseline width; the LSTM its
// generic product.
#include "../../neural/kernels.h"
#include "../../neural/lstm_kernels.h"

namespace sonora::dsp::neural::detail {

const Kernels* simd_kernels() noexcept { return nullptr; }

LstmMatvec lstm_simd_matvec() noexcept { return nullptr; }

}  // namespace sonora::dsp::neural::detail
