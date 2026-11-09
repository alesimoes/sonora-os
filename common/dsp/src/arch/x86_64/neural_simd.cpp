// SPDX-License-Identifier: GPL-3.0-or-later
//
// x86-64: the AVX2 kernel table (neural_avx2.cpp) when this processor and
// the OS run AVX2, else none and the WaveNet keeps the generic one. This
// file is compiled for the x86-64 baseline: it must run, and answer no, on
// a processor without AVX2.
#include "../../neural/kernels.h"
#include "../../neural/lstm_kernels.h"
#include "cpu.h"

namespace sonora::dsp::neural::detail {

namespace avx2 {
void matmul_rows(const float* w, int rows, int nin, const float* const* in, int n, float* out,
                 std::size_t out_stride) noexcept SONORA_RT;
void tanh_rows(float* x, int n) noexcept SONORA_RT;
void sigmoid_rows(float* x, int n) noexcept SONORA_RT;
// lstm_avx2.cpp.
void lstm_matvec(const float* w, int rows, int nin, const float* x, float* out) noexcept SONORA_RT;
}  // namespace avx2

namespace {
const Kernels kAvx2 = {"avx2", avx2::matmul_rows, avx2::tanh_rows, avx2::sigmoid_rows};
}  // namespace

const Kernels* simd_kernels() noexcept { return sonora::dsp::arch::cpu_runs_avx2() ? &kAvx2 : nullptr; }

// No CPU check here: the LSTM asks only once simd_kernels() has returned
// kAvx2 (lstm_kernels.h), and asks per sample, where CPUID does not belong.
LstmMatvec lstm_simd_matvec() noexcept { return &avx2::lstm_matvec; }

}  // namespace sonora::dsp::neural::detail
