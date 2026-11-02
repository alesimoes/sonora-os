// SPDX-License-Identifier: GPL-3.0-or-later
//
// The output stage's two channels in lockstep, portable:
// OutputStage::process_lanes (transformer_solve.h) over the two-double
// lane type of f64x2.h. Each lane gives the bits the one-channel step
// gives (test_dsp_transformer checks it); the pair gains two independent
// dependency chains for the core to overlap, with no branch between them.
#include "../../transformer_solve.h"
#include "f64x2.h"

namespace sonora::dsp {

void transformer_process_pair(OutputStage& st, const double* const* in, double* const* out, std::uint32_t steps,
                              const double* sag, std::uint32_t steps_per_sag) noexcept SONORA_RT {
    st.process_lanes<lanes_generic::F64x2>(in, out, steps, sag, steps_per_sag);
}

}  // namespace sonora::dsp
