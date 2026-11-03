// SPDX-License-Identifier: GPL-3.0-or-later
//
// The convolver's inner loops, x86-64: convolver_kernels.h over SSE2
// (f64x2.h), the x86-64 baseline. At -O2 without -ffast-math the compilers
// leave the plain loops scalar (they cannot prove the accumulator does not
// alias the spectra), and the multiply-add over the partitions is most of a
// long response's cost.
#include "../../convolver_kernels.h"
#include "f64x2.h"

namespace sonora::dsp {

void spectral_mac(double* ar, double* ai, const double* xr, const double* xi, const double* hr,
                  const double* hi, std::uint32_t bins) noexcept SONORA_RT {
    convolver_detail::mac2<lanes_sse2::F64x2>(ar, ai, xr, xi, hr, hi, nullptr, nullptr, nullptr, nullptr, bins,
                                               false);
}

void spectral_mac2(double* ar, double* ai, const double* x0r, const double* x0i, const double* h0r,
                   const double* h0i, const double* x1r, const double* x1i, const double* h1r,
                   const double* h1i, std::uint32_t bins) noexcept SONORA_RT {
    convolver_detail::mac2<lanes_sse2::F64x2>(ar, ai, x0r, x0i, h0r, h0i, x1r, x1i, h1r, h1i, bins, true);
}

double head_dot(const double* h, const double* w, std::uint32_t n) noexcept SONORA_RT {
    return convolver_detail::dot<lanes_sse2::F64x2>(h, w, n);
}

}  // namespace sonora::dsp
