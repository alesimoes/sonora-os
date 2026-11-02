// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two triode stages in lockstep, x86-64: TriodeStage::solve
// (triode_solve.h) over the SSE2 lane type of f64x2.h. Each lane gives the
// bits tick() gives (test_triode checks it): packing halves the
// instructions of the solve, not its precision.
#include "../../triode_solve.h"
#include "f64x2.h"

namespace sonora::dsp {

void triode_tick_pair(TriodeStage* const* st, const double* vin, double* out) noexcept {
    TriodeStage::solve<lanes_sse2::F64x2>(st, vin, out);
}

}  // namespace sonora::dsp
