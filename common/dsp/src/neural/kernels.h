// SPDX-License-Identifier: GPL-3.0-or-later
//
// The WaveNet's hot loops, one table per instruction set. Internal to
// common/dsp.
//
// The generic table (neural/kernels.cpp) is plain C++ the compiler
// vectorises at the baseline width. src/arch/<arch>/neural_simd.cpp offers
// a wider one when the processor runs it, chosen at run time because the
// appliance's x86-64 baseline has no AVX2 and a build for AVX2 alone would
// not start on an older machine (ADR-0011 §2.3: arch code chosen by CMake,
// the generic twin always beside it, no #ifdef).
//
// Every table gives the generic one's bits. Products sum each output's
// terms from 0 in input order, multiply then add, never fused; the
// activations run vec_math.h's operations in its order. So a capture
// sounds the same, to the bit, on every x86-64 machine, and the golden and
// equivalence tests do not depend on which table the CI runner picked.
// test_dsp_neural checks it table against table.
#pragma once

#include <cstddef>

#include "sonora/rt.h"

namespace sonora::dsp::neural::detail {

struct Kernels {
    const char* name;
    // out[o][t] = sum over i, in order, of w[o][i] x in[i][t], for o < rows
    // and t < n; w is [rows][nin], out rows are out_stride apart.
    void (*matmul_rows)(const float* w, int rows, int nin, const float* const* in, int n, float* out,
                        std::size_t out_stride) noexcept SONORA_RT;
    // In place over n values: vec_math.h's tanh_approx and sigmoid_approx.
    void (*tanh)(float* x, int n) noexcept SONORA_RT;
    void (*sigmoid)(float* x, int n) noexcept SONORA_RT;
};

const Kernels& generic_kernels() noexcept;

// The table the processor this runs on should use: a SIMD one when the
// arch directory has one and the processor (and the OS's saved register
// state) supports it, else nullptr. Not real-time the first time (it reads
// CPUID); WaveNet calls it in load().
const Kernels* simd_kernels() noexcept;

}  // namespace sonora::dsp::neural::detail
