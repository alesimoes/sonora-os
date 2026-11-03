// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two doubles in lockstep, x86-64: the two-lane V that triode_solve.h and
// transformer_solve.h are written over, as one SSE2 register (this type is
// the union of what the two headers use). SSE2 is part of the x86-64
// baseline and its packed double arithmetic is the same IEEE arithmetic
// the compiler uses for scalar double, so each lane gives the bits the
// one-lane step gives (test_dsp_triode and test_dsp_transformer check it):
// packing halves the instructions of a solve, not its precision.
//
// The semantics of each operation match arch/generic/f64x2.h: max(a, b) is
// a > b ? a : b, which is what MAXPD computes for (a, b); comparisons are
// ordered (false on NaN), like C++'s. Internal to common/dsp.
#pragma once

#include <emmintrin.h>

namespace sonora::dsp::lanes_sse2 {

struct F64x2 {
    static constexpr int kLanes = 2;
    struct Mask {
        __m128d m;
        Mask operator&(Mask o) const noexcept { return {_mm_and_pd(m, o.m)}; }
        Mask operator|(Mask o) const noexcept { return {_mm_or_pd(m, o.m)}; }
        Mask operator~() const noexcept { return {_mm_xor_pd(m, _mm_castsi128_pd(_mm_set1_epi32(-1)))}; }
        bool lane(int i) const noexcept { return ((_mm_movemask_pd(m) >> i) & 1) != 0; }
        bool all() const noexcept { return _mm_movemask_pd(m) == 3; }
        bool any() const noexcept { return _mm_movemask_pd(m) != 0; }
    };
    __m128d x;
    F64x2() noexcept : x(_mm_setzero_pd()) {}
    explicit F64x2(double v) noexcept : x(_mm_set1_pd(v)) {}
    explicit F64x2(__m128d v) noexcept : x(v) {}
    // Two 8-byte loads, not one 16-byte one: the lanes are written one
    // double at a time just before (per-lane law results, per-stage
    // state), and a 16-byte load of two 8-byte stores cannot be
    // store-forwarded, which stalled every pass (measured on the triode:
    // the pair was slower than two scalar ticks with _mm_loadu_pd here).
    static F64x2 load(const double* p) noexcept { return F64x2(_mm_loadh_pd(_mm_load_sd(p), p + 1)); }
    static F64x2 set(const double* p) noexcept { return F64x2(_mm_set_pd(p[1], p[0])); }
    // p is a 16-byte aligned pair of equal values (TriodeStage::dup_).
    static F64x2 dup(const double* p) noexcept { return F64x2(_mm_load_pd(p)); }
    double get(int i) const noexcept { return i == 0 ? _mm_cvtsd_f64(x) : _mm_cvtsd_f64(_mm_unpackhi_pd(x, x)); }
    void store(double* p) const noexcept { _mm_storeu_pd(p, x); }
    // Lane l takes p[l][0] into lo and p[l][1] into hi: one table node (two
    // adjacent doubles, written once at prepare, so a 16-byte load does not
    // wait on a store) per lane.
    static void gather2(const double* const* p, F64x2& lo, F64x2& hi) noexcept {
        const __m128d a = _mm_loadu_pd(p[0]), b = _mm_loadu_pd(p[1]);
        lo = F64x2(_mm_unpacklo_pd(a, b));
        hi = F64x2(_mm_unpackhi_pd(a, b));
    }
    // Each lane, in [0, 2^31), truncated towards zero (CVTTPD2DQ, as
    // static_cast<int> is): the integers into idx, their values as doubles
    // (exact) returned.
    F64x2 trunc_index(int* idx) const noexcept {
        const __m128i k = _mm_cvttpd_epi32(x);
        idx[0] = _mm_cvtsi128_si32(k);
        idx[1] = _mm_cvtsi128_si32(_mm_shuffle_epi32(k, 1));
        return F64x2(_mm_cvtepi32_pd(k));
    }
    static Mask none() noexcept { return {_mm_setzero_pd()}; }
    F64x2 operator+(F64x2 o) const noexcept { return F64x2(_mm_add_pd(x, o.x)); }
    F64x2 operator-(F64x2 o) const noexcept { return F64x2(_mm_sub_pd(x, o.x)); }
    F64x2 operator*(F64x2 o) const noexcept { return F64x2(_mm_mul_pd(x, o.x)); }
    F64x2 operator/(F64x2 o) const noexcept { return F64x2(_mm_div_pd(x, o.x)); }
    F64x2 neg() const noexcept { return F64x2(_mm_xor_pd(x, _mm_set1_pd(-0.0))); }
    F64x2 abs() const noexcept { return F64x2(_mm_andnot_pd(_mm_set1_pd(-0.0), x)); }
    F64x2 sqrt() const noexcept { return F64x2(_mm_sqrt_pd(x)); }
    static F64x2 max(F64x2 a, F64x2 b) noexcept { return F64x2(_mm_max_pd(a.x, b.x)); }
    Mask operator>(F64x2 o) const noexcept { return {_mm_cmpgt_pd(x, o.x)}; }
    Mask operator<(F64x2 o) const noexcept { return {_mm_cmplt_pd(x, o.x)}; }
    Mask operator<=(F64x2 o) const noexcept { return {_mm_cmple_pd(x, o.x)}; }
    Mask operator==(F64x2 o) const noexcept { return {_mm_cmpeq_pd(x, o.x)}; }
    static F64x2 select(Mask m, F64x2 a, F64x2 b) noexcept {
        return F64x2(_mm_or_pd(_mm_and_pd(m.m, a.x), _mm_andnot_pd(m.m, b.x)));
    }
};

}  // namespace sonora::dsp::lanes_sse2
