// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LSTM's product in AVX2 (neural/lstm_kernels.h): eight terms of a row
// per instruction. Compiled with AVX2 for this file alone, without FMA
// (cmake/lstm_kernel.cmake, the options neural_avx2.cpp has), and called
// only when neural_simd.cpp has found AVX2 at run time.
//
// The rules of neural_avx2.cpp hold here: no header with inline functions
// or templates of its own (an AVX2 copy of an inline the baseline files
// also use could be the one the linker keeps), helpers in an anonymous
// namespace, no fused multiply-add. And the order lstm_kernels.h defines:
// lane j of a row's accumulator takes terms j, j + 8, ..., the tail is
// loaded masked (its dead lanes add 0 x 0 = +0), and the halves are
// reduced 128 then 64 then 32 bits. Same IEEE operations as
// lstm_matvec_generic, so the same bits (test_dsp_lstm holds it).
#include <immintrin.h>

#include <cstddef>

#include "sonora/rt.h"  // macros only

namespace sonora::dsp::neural::detail::avx2 {

namespace {

// ((p0 + p4) + (p2 + p6)) + ((p1 + p5) + (p3 + p7)).
inline float reduce(__m256 p) noexcept {
    const __m128 q = _mm_add_ps(_mm256_castps256_ps128(p), _mm256_extractf128_ps(p, 1));
    const __m128 r = _mm_add_ps(q, _mm_movehl_ps(q, q));
    return _mm_cvtss_f32(_mm_add_ss(r, _mm_shuffle_ps(r, r, 1)));
}

// R rows share each load of x: R = 4 keeps four accumulators, x and a
// weight vector well inside the sixteen ymm registers.
template <int R>
void rows(const float* w, std::size_t nin, const float* x, float* out) noexcept {
    __m256 acc[R];
    for (int r = 0; r < R; ++r) acc[r] = _mm256_setzero_ps();
    std::size_t i = 0;
    for (; i + 8 <= nin; i += 8) {
        const __m256 xv = _mm256_loadu_ps(x + i);
        for (int r = 0; r < R; ++r)
            acc[r] = _mm256_add_ps(acc[r], _mm256_mul_ps(_mm256_loadu_ps(w + static_cast<std::size_t>(r) * nin + i), xv));
    }
    if (i < nin) {
        // Lane j live when j < rest; a masked-off lane loads +0 and reads
        // no memory, so the last row's tail never reads past the weights.
        const int rest = static_cast<int>(nin - i);
        const __m256i mask = _mm256_cmpgt_epi32(_mm256_set1_epi32(rest), _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7));
        const __m256 xv = _mm256_maskload_ps(x + i, mask);
        for (int r = 0; r < R; ++r)
            acc[r] = _mm256_add_ps(
                acc[r], _mm256_mul_ps(_mm256_maskload_ps(w + static_cast<std::size_t>(r) * nin + i, mask), xv));
    }
    for (int r = 0; r < R; ++r) out[r] = reduce(acc[r]);
}

}  // namespace

void lstm_matvec(const float* w, int nrows, int nin, const float* x, float* out) noexcept SONORA_RT {
    const std::size_t sw = static_cast<std::size_t>(nin);
    int o = 0;
    for (; o + 4 <= nrows; o += 4) rows<4>(w + static_cast<std::size_t>(o) * sw, sw, x, out + o);
    for (; o < nrows; ++o) rows<1>(w + static_cast<std::size_t>(o) * sw, sw, x, out + o);
}

}  // namespace sonora::dsp::neural::detail::avx2
