// SPDX-License-Identifier: GPL-3.0-or-later
//
// The output stage's two channels in lockstep, x86-64: the two-lane V of
// OutputStage::process_lanes (transformer_solve.h) as one SSE2 register.
// SSE2 is part of the x86-64 baseline and its packed double arithmetic is
// the same IEEE arithmetic the compiler uses for scalar double, so each lane
// gives the bits the one-channel step gives (test_dsp_transformer checks
// it): packing halves the instructions of the step, not its precision.
//
// Comparisons are ordered (false on NaN), like C++'s.
#include <emmintrin.h>

#include "../../transformer_solve.h"

namespace sonora::dsp {

namespace {

struct L2 {
    static constexpr int kLanes = 2;
    struct Mask {
        __m128d m;
        Mask operator&(Mask o) const noexcept { return {_mm_and_pd(m, o.m)}; }
        bool lane(int i) const noexcept { return ((_mm_movemask_pd(m) >> i) & 1) != 0; }
    };
    __m128d x;
    L2() noexcept : x(_mm_setzero_pd()) {}
    explicit L2(double v) noexcept : x(_mm_set1_pd(v)) {}
    explicit L2(__m128d v) noexcept : x(v) {}
    // Two 8-byte loads, not one 16-byte one: the lanes were written one
    // double at a time just before, and a 16-byte load of two 8-byte
    // stores cannot be store-forwarded (triode_pair.cpp measured the stall).
    static L2 pack(const double* p) noexcept { return L2(_mm_loadh_pd(_mm_load_sd(p), p + 1)); }
    double get(int i) const noexcept { return i == 0 ? _mm_cvtsd_f64(x) : _mm_cvtsd_f64(_mm_unpackhi_pd(x, x)); }
    L2 operator+(L2 o) const noexcept { return L2(_mm_add_pd(x, o.x)); }
    L2 operator-(L2 o) const noexcept { return L2(_mm_sub_pd(x, o.x)); }
    L2 operator*(L2 o) const noexcept { return L2(_mm_mul_pd(x, o.x)); }
    L2 operator/(L2 o) const noexcept { return L2(_mm_div_pd(x, o.x)); }
    L2 neg() const noexcept { return L2(_mm_xor_pd(x, _mm_set1_pd(-0.0))); }
    L2 abs() const noexcept { return L2(_mm_andnot_pd(_mm_set1_pd(-0.0), x)); }
    L2 sqrt() const noexcept { return L2(_mm_sqrt_pd(x)); }
    Mask operator<(L2 o) const noexcept { return {_mm_cmplt_pd(x, o.x)}; }
    Mask operator>(L2 o) const noexcept { return {_mm_cmpgt_pd(x, o.x)}; }
    Mask operator<=(L2 o) const noexcept { return {_mm_cmple_pd(x, o.x)}; }
    static L2 select(Mask m, L2 a, L2 b) noexcept { return L2(_mm_or_pd(_mm_and_pd(m.m, a.x), _mm_andnot_pd(m.m, b.x))); }
};

}  // namespace

void transformer_process_pair(OutputStage& st, const double* const* in, double* const* out, std::uint32_t steps,
                              const double* sag, std::uint32_t steps_per_sag) noexcept SONORA_RT {
    st.process_lanes<L2>(in, out, steps, sag, steps_per_sag);
}

}  // namespace sonora::dsp
