// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tape's two channels in lockstep, portable: TapeSolve (tape_solve.h)
// over the shared two-lane type of f64x2.h, the one triode_pair.cpp and
// transformer_pair.cpp use. The operations are TapeHysteresis::tick()'s,
// lane by lane, so the results are the same bits; what the pair gains is
// two independent dependency chains for the core to overlap.
#include "../../tape_solve.h"
#include "f64x2.h"

namespace sonora::dsp {

namespace {

using V2 = lanes_generic::F64x2;

// The one operation F64x2 lacks, lane by lane: tape_detail::exp2_bits.
struct V2Bits {
    static V2 exp2_magic(V2 t) noexcept {
        return V2(tape_detail::exp2_bits(t.x[0]), tape_detail::exp2_bits(t.x[1]));
    }
};

}  // namespace

void tape_run_pair(TapeHysteresis& a, TapeHysteresis& b, double* xa, double* xb, std::uint32_t n) noexcept SONORA_RT {
    TapeHysteresis* st[2] = {&a, &b};
    double* x[2] = {xa, xb};
    TapeSolve<V2, V2Bits>::run(st, x, n);
}

}  // namespace sonora::dsp
