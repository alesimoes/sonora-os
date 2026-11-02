// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two triode stages in lockstep, portable: TriodeStage::solve
// (triode_solve.h) over the two-double lane type of f64x2.h. Each lane
// gives the bits tick() gives (test_triode checks it); the pair gains two
// independent dependency chains for the core to overlap.
#include "../../triode_solve.h"
#include "f64x2.h"

namespace sonora::dsp {

void triode_tick_pair(TriodeStage* const* st, const double* vin, double* out) noexcept {
    TriodeStage::solve<lanes_generic::F64x2>(st, vin, out);
}

}  // namespace sonora::dsp
