// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tape's two channels in lockstep, x86-64: TapeSolve (tape_solve.h)
// over the SSE2 lane type of f64x2.h, the one triode_pair.cpp and
// transformer_pair.cpp use. Each lane gives the bits
// TapeHysteresis::tick() gives (test_dsp_tape checks it): packing halves
// the instructions of the solve, not its precision.
#include <emmintrin.h>

#include "../../tape_solve.h"
#include "f64x2.h"

namespace sonora::dsp {

namespace {

using V2 = lanes_sse2::F64x2;

// The one operation F64x2 lacks: (bits(t) + 1023) << 52 in each 64-bit
// lane, tape_detail::exp2_bits in one register.
struct V2Bits {
    static V2 exp2_magic(V2 t) noexcept {
        const __m128i u = _mm_add_epi64(_mm_castpd_si128(t.x), _mm_set_epi32(0, 1023, 0, 1023));
        return V2(_mm_castsi128_pd(_mm_slli_epi64(u, 52)));
    }
};

}  // namespace

void tape_run_pair(TapeHysteresis& a, TapeHysteresis& b, double* xa, double* xb, std::uint32_t n) noexcept SONORA_RT {
    TapeHysteresis* st[2] = {&a, &b};
    double* x[2] = {xa, xb};
    TapeSolve<V2, V2Bits>::run(st, x, n);
}

}  // namespace sonora::dsp
