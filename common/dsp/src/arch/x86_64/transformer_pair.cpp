// SPDX-License-Identifier: GPL-3.0-or-later
//
// The output stage's two channels in lockstep, x86-64:
// OutputStage::process_lanes (transformer_solve.h) over the SSE2 lane type
// of f64x2.h. Each lane gives the bits the one-channel step gives
// (test_dsp_transformer checks it): packing halves the instructions of the
// step, not its precision.
#include "../../transformer_solve.h"
#include "f64x2.h"

namespace sonora::dsp {

void transformer_process_pair(OutputStage& st, const double* const* in, double* const* out, std::uint32_t steps,
                              const double* sag, std::uint32_t steps_per_sag) noexcept SONORA_RT {
    st.process_lanes<lanes_sse2::F64x2>(in, out, steps, sag, steps_per_sag);
}

}  // namespace sonora::dsp
