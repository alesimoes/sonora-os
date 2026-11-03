// SPDX-License-Identifier: GPL-3.0-or-later
//
// The WaveNet's kernels in AVX2 (neural/kernels.h), eight floats a lane.
// Compiled with AVX2 enabled for this file alone (CMakeLists.txt) and
// called only when neural_simd.cpp has found AVX2 at run time.
//
// Two rules keep this file safe and bit-exact:
// - It includes no header with inline functions or templates of its own
//   (no <algorithm>, no vec_math.h) and keeps every helper in an anonymous
//   namespace. An inline function compiled here with AVX2 and also in a
//   baseline file is one symbol to the linker, which may keep this copy
//   for every caller and run AVX2 on a processor without it.
// - No fused multiply-add (no -mfma; MSVC's /fp:precise does not contract)
//   and every operation in the generic kernel's order: each product term
//   is a multiply then an add, each sum starts at 0 and takes its terms in
//   input order, and the tanh and sigmoid below are vec_math.h's, step for
//   step. Same IEEE operations, same bits (test_dsp_neural holds it).
#include <immintrin.h>

#include <cstddef>
#include <cstdint>

#include "sonora/rt.h"  // macros only

namespace sonora::dsp::neural::detail::avx2 {

namespace {

// R output rows by eight frames per pass: R accumulators stay in registers
// over the whole input loop, and each input vector is loaded once for R
// multiply-adds (the generic kernel stores every partial sum back to its
// row). R = 8 keeps eight accumulators, the input and a broadcast weight
// inside the sixteen ymm registers.
template <int R>
void tile(const float* w, std::size_t nin, const float* const* in, int n, float* out,
          std::size_t out_stride) noexcept {
    int t = 0;
    for (; t + 8 <= n; t += 8) {
        __m256 acc[R];
        for (int r = 0; r < R; ++r) acc[r] = _mm256_setzero_ps();
        for (std::size_t i = 0; i < nin; ++i) {
            const __m256 x = _mm256_loadu_ps(in[i] + t);
            for (int r = 0; r < R; ++r)
                acc[r] = _mm256_add_ps(acc[r], _mm256_mul_ps(_mm256_set1_ps(w[static_cast<std::size_t>(r) * nin + i]), x));
        }
        for (int r = 0; r < R; ++r) _mm256_storeu_ps(out + static_cast<std::size_t>(r) * out_stride + t, acc[r]);
    }
    // The last n % 8 frames one at a time, in the same order.
    for (; t < n; ++t) {
        for (int r = 0; r < R; ++r) {
            float s = 0.0f;
            for (std::size_t i = 0; i < nin; ++i) s += w[static_cast<std::size_t>(r) * nin + i] * in[i][t];
            out[static_cast<std::size_t>(r) * out_stride + t] = s;
        }
    }
}

// vec_math.h's exp_nonpositive, eight at a time.
inline __m256 exp_nonpositive(__m256 y) noexcept {
    const __m256 t = _mm256_mul_ps(y, _mm256_set1_ps(1.44269504088896341f));
    const __m256i n = _mm256_cvttps_epi32(_mm256_sub_ps(t, _mm256_set1_ps(0.5f)));
    const __m256 fn = _mm256_cvtepi32_ps(n);
    const __m256 r = _mm256_sub_ps(_mm256_sub_ps(y, _mm256_mul_ps(fn, _mm256_set1_ps(0.693359375f))),
                                   _mm256_mul_ps(fn, _mm256_set1_ps(-2.12194440054690583e-4f)));
    __m256 p = _mm256_set1_ps(1.0f / 5040.0f);
    p = _mm256_add_ps(_mm256_mul_ps(p, r), _mm256_set1_ps(1.0f / 720.0f));
    p = _mm256_add_ps(_mm256_mul_ps(p, r), _mm256_set1_ps(1.0f / 120.0f));
    p = _mm256_add_ps(_mm256_mul_ps(p, r), _mm256_set1_ps(1.0f / 24.0f));
    p = _mm256_add_ps(_mm256_mul_ps(p, r), _mm256_set1_ps(1.0f / 6.0f));
    p = _mm256_add_ps(_mm256_mul_ps(p, r), _mm256_set1_ps(0.5f));
    p = _mm256_add_ps(_mm256_mul_ps(p, r), _mm256_set1_ps(1.0f));
    p = _mm256_add_ps(_mm256_mul_ps(p, r), _mm256_set1_ps(1.0f));
    const __m256i bits = _mm256_slli_epi32(_mm256_add_epi32(n, _mm256_set1_epi32(127)), 23);
    return _mm256_mul_ps(p, _mm256_castsi256_ps(bits));
}

inline __m256 sign_mask() noexcept { return _mm256_set1_ps(-0.0f); }

// The step of a Horner sum, a multiply then an add.
inline __m256 horner(__m256 acc, __m256 s, float c) noexcept {
    return _mm256_add_ps(_mm256_mul_ps(acc, s), _mm256_set1_ps(c));
}

// vec_math.h's tanh_approx, coefficient for coefficient. MINPS(a, b) is
// a < b ? a : b and MAXPS(a, b) is a > b ? a : b, the ternaries there.
inline __m256 tanh8(__m256 x) noexcept {
    const __m256 m = _mm256_min_ps(x, _mm256_set1_ps(7.9f));
    const __m256 v = _mm256_max_ps(m, _mm256_set1_ps(-7.9f));
    const __m256 s = _mm256_mul_ps(v, v);
    __m256 p = _mm256_set1_ps(-1.167722603e-13f);
    p = horner(p, s, 6.393623131e-11f);
    p = horner(p, s, -2.234931351e-08f);
    p = horner(p, s, 1.164718196e-05f);
    p = horner(p, s, 3.149628639e-03f);
    p = horner(p, s, 1.312453151e-01f);
    p = horner(p, s, 1.000000119e+00f);
    __m256 q = _mm256_set1_ps(2.613467223e-04f);
    q = horner(q, s, 2.467588335e-02f);
    q = horner(q, s, 4.645786583e-01f);
    q = horner(q, s, 1.0f);
    return _mm256_div_ps(_mm256_mul_ps(v, p), q);
}

// vec_math.h's sigmoid_approx; -(m) is a sign flip, exact.
inline __m256 sigmoid8(__m256 x) noexcept {
    const __m256 ax = _mm256_andnot_ps(sign_mask(), x);
    const __m256 m = _mm256_min_ps(ax, _mm256_set1_ps(80.0f));
    const __m256 e = exp_nonpositive(_mm256_xor_ps(m, sign_mask()));
    const __m256 s = _mm256_div_ps(_mm256_set1_ps(1.0f), _mm256_add_ps(_mm256_set1_ps(1.0f), e));
    return _mm256_blendv_ps(_mm256_mul_ps(e, s), s, _mm256_cmp_ps(x, _mm256_setzero_ps(), _CMP_GE_OQ));
}

// The last n % 8 values go through the same eight-lane function, padded:
// each lane is independent, so a value's bits do not depend on where in
// the block it falls.
template <__m256 (*F)(__m256)>
void map(float* x, int n) noexcept {
    int i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(x + i, F(_mm256_loadu_ps(x + i)));
    if (i < n) {
        alignas(32) float pad[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        const int rest = n - i;
        for (int j = 0; j < rest; ++j) pad[j] = x[i + j];
        _mm256_store_ps(pad, F(_mm256_load_ps(pad)));
        for (int j = 0; j < rest; ++j) x[i + j] = pad[j];
    }
}

}  // namespace

void matmul_rows(const float* w, int rows, int nin, const float* const* in, int n, float* out,
                 std::size_t out_stride) noexcept SONORA_RT {
    const std::size_t sw = static_cast<std::size_t>(nin);
    int o = 0;
    for (; o + 8 <= rows; o += 8)
        tile<8>(w + static_cast<std::size_t>(o) * sw, sw, in, n, out + static_cast<std::size_t>(o) * out_stride, out_stride);
    for (; o + 4 <= rows; o += 4)
        tile<4>(w + static_cast<std::size_t>(o) * sw, sw, in, n, out + static_cast<std::size_t>(o) * out_stride, out_stride);
    for (; o < rows; ++o)
        tile<1>(w + static_cast<std::size_t>(o) * sw, sw, in, n, out + static_cast<std::size_t>(o) * out_stride, out_stride);
}

void tanh_rows(float* x, int n) noexcept SONORA_RT { map<tanh8>(x, n); }

void sigmoid_rows(float* x, int n) noexcept SONORA_RT { map<sigmoid8>(x, n); }

}  // namespace sonora::dsp::neural::detail::avx2
