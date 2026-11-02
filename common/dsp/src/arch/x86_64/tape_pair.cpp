// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tape's two channels in lockstep, x86-64: the two-lane V of
// TapeSolve (tape_solve.h) as one SSE2 register. SSE2 is part of the
// x86-64 baseline, and its packed double arithmetic is the IEEE
// arithmetic the compiler uses for scalar double, so each lane gives the
// bits TapeHysteresis::tick() gives (test_dsp_tape checks it): packing
// halves the instructions of the solve, not its precision. The semantics
// of each operation are the generic V2's (arch/generic/tape_pair.cpp);
// comparisons are ordered (false on NaN), like C++'s.
#include <emmintrin.h>

#include "../../tape_solve.h"

namespace sonora::dsp {

namespace {

struct V2 {
    static constexpr int kLanes = 2;
    struct Mask {
        __m128d m;
        Mask operator&(Mask o) const noexcept { return {_mm_and_pd(m, o.m)}; }
        Mask operator|(Mask o) const noexcept { return {_mm_or_pd(m, o.m)}; }
        Mask operator~() const noexcept { return {_mm_xor_pd(m, _mm_castsi128_pd(_mm_set1_epi32(-1)))}; }
        bool any() const noexcept { return _mm_movemask_pd(m) != 0; }
    };
    __m128d x;
    V2() noexcept : x(_mm_setzero_pd()) {}
    explicit V2(double v) noexcept : x(_mm_set1_pd(v)) {}
    explicit V2(__m128d v) noexcept : x(v) {}
    // Two 8-byte loads, not one 16-byte one: the lanes were just written
    // one double at a time, and a 16-byte load of two 8-byte stores cannot
    // be store-forwarded (triode_pair.cpp measured the stall).
    static V2 from(const double* p) noexcept { return V2(_mm_loadh_pd(_mm_load_sd(p), p + 1)); }
    double get(int i) const noexcept { return i == 0 ? _mm_cvtsd_f64(x) : _mm_cvtsd_f64(_mm_unpackhi_pd(x, x)); }
    V2 operator+(V2 o) const noexcept { return V2(_mm_add_pd(x, o.x)); }
    V2 operator-(V2 o) const noexcept { return V2(_mm_sub_pd(x, o.x)); }
    V2 operator*(V2 o) const noexcept { return V2(_mm_mul_pd(x, o.x)); }
    V2 operator/(V2 o) const noexcept { return V2(_mm_div_pd(x, o.x)); }
    V2 neg() const noexcept { return V2(_mm_xor_pd(x, _mm_set1_pd(-0.0))); }
    V2 abs() const noexcept { return V2(_mm_andnot_pd(_mm_set1_pd(-0.0), x)); }
    Mask operator<(V2 o) const noexcept { return {_mm_cmplt_pd(x, o.x)}; }
    Mask operator>(V2 o) const noexcept { return {_mm_cmpgt_pd(x, o.x)}; }
    Mask operator<=(V2 o) const noexcept { return {_mm_cmple_pd(x, o.x)}; }
    Mask operator==(V2 o) const noexcept { return {_mm_cmpeq_pd(x, o.x)}; }
    static V2 select(Mask m, V2 a, V2 b) noexcept {
        return V2(_mm_or_pd(_mm_and_pd(m.m, a.x), _mm_andnot_pd(m.m, b.x)));
    }
    // (bits(t) + 1023) << 52 in each 64-bit lane: tape_detail::V1::exp2_bits.
    static V2 exp2_magic(V2 t) noexcept {
        const __m128i u = _mm_add_epi64(_mm_castpd_si128(t.x), _mm_set_epi32(0, 1023, 0, 1023));
        return V2(_mm_castsi128_pd(_mm_slli_epi64(u, 52)));
    }
    // Both lanes truncated at once (CVTTPD2DQ), as static_cast<int> does
    // one: the lanes here are in [0, 256).
    static V2 trunc(V2 u, int* idx) noexcept {
        const __m128i i = _mm_cvttpd_epi32(u.x);
        idx[0] = _mm_cvtsi128_si32(i);
        idx[1] = _mm_cvtsi128_si32(_mm_shuffle_epi32(i, 1));
        return V2(_mm_cvtepi32_pd(i));
    }
    // Two table rows of four, transposed into four lane pairs.
    static void rows4(const double* const* r, V2& c0, V2& c1, V2& c2, V2& c3) noexcept {
        const __m128d a01 = _mm_loadu_pd(r[0]), a23 = _mm_loadu_pd(r[0] + 2);
        const __m128d b01 = _mm_loadu_pd(r[1]), b23 = _mm_loadu_pd(r[1] + 2);
        c0 = V2(_mm_unpacklo_pd(a01, b01));
        c1 = V2(_mm_unpackhi_pd(a01, b01));
        c2 = V2(_mm_unpacklo_pd(a23, b23));
        c3 = V2(_mm_unpackhi_pd(a23, b23));
    }
};

}  // namespace

void tape_run_pair(TapeHysteresis& a, TapeHysteresis& b, double* xa, double* xb, std::uint32_t n) noexcept {
    TapeHysteresis* st[2] = {&a, &b};
    double* x[2] = {xa, xb};
    TapeSolve<V2>::run(st, x, n);
}

}  // namespace sonora::dsp
