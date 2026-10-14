// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two triode stages in lockstep, x86-64: the two-lane V of
// TriodeStage::solve (triode_solve.h) as one SSE2 register. SSE2 is part
// of the x86-64 baseline and its packed double arithmetic is the same IEEE
// arithmetic the compiler uses for scalar double, so each lane gives the
// bits tick() gives (test_triode checks it): packing halves the
// instructions of the solve, not its precision.
//
// The semantics of each operation match the generic V2 and tick()'s V1:
// max(a, b) is a > b ? a : b, which is what MAXPD computes for (a, b);
// comparisons are ordered (false on NaN), like C++'s.
#include <emmintrin.h>

#include "../../triode_solve.h"

namespace sonora::dsp {

namespace {

struct V2 {
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
    V2() noexcept : x(_mm_setzero_pd()) {}
    explicit V2(double v) noexcept : x(_mm_set1_pd(v)) {}
    explicit V2(__m128d v) noexcept : x(v) {}
    // Two 8-byte loads, not one 16-byte one: the lanes are written one
    // double at a time just before (per-lane law results, per-stage
    // state), and a 16-byte load of two 8-byte stores cannot be
    // store-forwarded, which stalled every pass (measured: the pair was
    // slower than two scalar ticks with _mm_loadu_pd here).
    static V2 load(const double* p) noexcept { return V2(_mm_loadh_pd(_mm_load_sd(p), p + 1)); }
    static V2 set(const double* p) noexcept { return V2(_mm_set_pd(p[1], p[0])); }
    // p is a 16-byte aligned pair of equal values (TriodeStage::dup_).
    static V2 dup(const double* p) noexcept { return V2(_mm_load_pd(p)); }
    double get(int i) const noexcept { return i == 0 ? _mm_cvtsd_f64(x) : _mm_cvtsd_f64(_mm_unpackhi_pd(x, x)); }
    void store(double* p) const noexcept { _mm_storeu_pd(p, x); }
    static Mask none() noexcept { return {_mm_setzero_pd()}; }
    V2 operator+(V2 o) const noexcept { return V2(_mm_add_pd(x, o.x)); }
    V2 operator-(V2 o) const noexcept { return V2(_mm_sub_pd(x, o.x)); }
    V2 operator*(V2 o) const noexcept { return V2(_mm_mul_pd(x, o.x)); }
    V2 operator/(V2 o) const noexcept { return V2(_mm_div_pd(x, o.x)); }
    V2 neg() const noexcept { return V2(_mm_xor_pd(x, _mm_set1_pd(-0.0))); }
    V2 abs() const noexcept { return V2(_mm_andnot_pd(_mm_set1_pd(-0.0), x)); }
    static V2 max(V2 a, V2 b) noexcept { return V2(_mm_max_pd(a.x, b.x)); }
    Mask operator>(V2 o) const noexcept { return {_mm_cmpgt_pd(x, o.x)}; }
    Mask operator<(V2 o) const noexcept { return {_mm_cmplt_pd(x, o.x)}; }
    Mask operator==(V2 o) const noexcept { return {_mm_cmpeq_pd(x, o.x)}; }
    static V2 select(Mask m, V2 a, V2 b) noexcept {
        return V2(_mm_or_pd(_mm_and_pd(m.m, a.x), _mm_andnot_pd(m.m, b.x)));
    }
};

}  // namespace

void triode_tick_pair(TriodeStage* const* st, const double* vin, double* out) noexcept {
    TriodeStage::solve<V2>(st, vin, out);
}

}  // namespace sonora::dsp
