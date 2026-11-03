// SPDX-License-Identifier: GPL-3.0-or-later
//
// The convolver's two inner loops, written once over a two-double lane type
// and instantiated per architecture (src/arch/<arch>/convolver_kernels.cpp,
// chosen by CMake). Every lane computes the expression the scalar loop
// does, in the same order, so the generic and SSE2 builds give the same
// bits (test_dsp_convolver_generic runs the same tests on the generic one).
// Internal to common/dsp.
#pragma once

#include <cstdint>

#include "sonora/rt.h"

namespace sonora::dsp {

// a += x * h, bin by bin, for complex spectra held as split real and
// imaginary arrays; the two-partition form adds x0 * h0 then x1 * h1, so
// the accumulator is read and written once for the two.
void spectral_mac(double* ar, double* ai, const double* xr, const double* xi, const double* hr,
                  const double* hi, std::uint32_t bins) noexcept SONORA_RT;
void spectral_mac2(double* ar, double* ai, const double* x0r, const double* x0i, const double* h0r,
                   const double* h0i, const double* x1r, const double* x1i, const double* h1r,
                   const double* h1i, std::uint32_t bins) noexcept SONORA_RT;
// sum h[j] w[j] for n a multiple of 4, as four running sums
// ((s0 + s1) + (s2 + s3)), s_i taking every fourth product from i.
double head_dot(const double* h, const double* w, std::uint32_t n) noexcept SONORA_RT;

namespace convolver_detail {

template <class V>
inline void mac2(double* ar, double* ai, const double* x0r, const double* x0i, const double* h0r,
                 const double* h0i, const double* x1r, const double* x1i, const double* h1r,
                 const double* h1i, std::uint32_t bins, bool two) noexcept {
    std::uint32_t k = 0;
    for (; k + 2 <= bins; k += 2) {
        const V ar0 = V::set(x0r + k), ai0 = V::set(x0i + k);
        const V br0 = V::set(h0r + k), bi0 = V::set(h0i + k);
        V sr = V::set(ar + k) + (ar0 * br0 - ai0 * bi0);
        V si = V::set(ai + k) + (ar0 * bi0 + ai0 * br0);
        if (two) {
            const V ar1 = V::set(x1r + k), ai1 = V::set(x1i + k);
            const V br1 = V::set(h1r + k), bi1 = V::set(h1i + k);
            sr = sr + (ar1 * br1 - ai1 * bi1);
            si = si + (ar1 * bi1 + ai1 * br1);
        }
        sr.store(ar + k);
        si.store(ai + k);
    }
    for (; k < bins; ++k) {
        double sr = ar[k] + (x0r[k] * h0r[k] - x0i[k] * h0i[k]);
        double si = ai[k] + (x0r[k] * h0i[k] + x0i[k] * h0r[k]);
        if (two) {
            sr = sr + (x1r[k] * h1r[k] - x1i[k] * h1i[k]);
            si = si + (x1r[k] * h1i[k] + x1i[k] * h1r[k]);
        }
        ar[k] = sr;
        ai[k] = si;
    }
}

template <class V>
inline double dot(const double* h, const double* w, std::uint32_t n) noexcept {
    V a(0.0), b(0.0);
    for (std::uint32_t j = 0; j < n; j += 4) {
        a = a + V::set(h + j) * V::set(w + j);
        b = b + V::set(h + j + 2) * V::set(w + j + 2);
    }
    return (a.get(0) + a.get(1)) + (b.get(0) + b.get(1));
}

}  // namespace convolver_detail

}  // namespace sonora::dsp
