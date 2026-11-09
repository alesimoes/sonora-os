// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LSTM's matrix-vector product, one function per instruction set.
// Internal to common/dsp (ADR-0111 §4).
//
// The WaveNet's matmul_rows (kernels.h) vectorises over a block's frames;
// an LSTM step has one frame, a product of [4H][in + H] weights by one
// vector, so it needs its own product. The generic one is in
// src/neural/lstm.cpp, the AVX2 one in src/arch/x86_64/lstm_avx2.cpp,
// chosen at run time the way the WaveNet's table is (#342): never by
// #ifdef (ADR-0008 §1).
//
// Both give the same bits, because the summation order is part of the
// definition, not of the instruction set: row o's dot product keeps eight
// partial sums, lane j taking the terms i = j, j + 8, j + 16, ... in that
// order, each a multiply then an add (no fusion); a lane past the last
// term adds +0. The eight are then reduced as
//   ((p0 + p4) + (p2 + p6)) + ((p1 + p5) + (p3 + p7)),
// the order AVX2's 128-bit halves and then 64-bit halves give.
// test_dsp_lstm holds the two products to the same bits.
#pragma once

#include "sonora/rt.h"

namespace sonora::dsp::neural::detail {

// out[o] = row o of w ([rows][nin], row-major) dot x[0..nin), for o < rows,
// in the order above.
using LstmMatvec = void (*)(const float* w, int rows, int nin, const float* x, float* out) noexcept SONORA_RT;

void lstm_matvec_generic(const float* w, int rows, int nin, const float* x, float* out) noexcept SONORA_RT;

// The SIMD product for this architecture, or nullptr when it has none.
// Called only once simd_kernels() (kernels.h) has returned a table, which
// means the processor runs that instruction set: both come from the same
// check (src/arch/<arch>/neural_simd.cpp).
LstmMatvec lstm_simd_matvec() noexcept;

}  // namespace sonora::dsp::neural::detail
