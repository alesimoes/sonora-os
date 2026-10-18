// SPDX-License-Identifier: GPL-3.0-or-later
//
// Contract file (ADR-0007 §4.2, §6). Owners: rt-engineer and driver-engineer
// jointly. Everything in this header is on the audio thread's side of the
// boundary.
#pragma once

#include <cstdint>

#include "sonora/rt.h"

namespace sonora::backend {

// One period of audio, as the backend hands it to the engine. Every pointer
// is owned by the backend and valid only for the duration of the process()
// call that receives it.
struct AudioBlock {
    // inputs[i] is channel i in the order the backend's ports were
    // registered, nframes long. It may be nullptr when nothing is connected;
    // a non-null pointer does not mean connected. Ask
    // AudioBackend::connected_input_count().
    const float* const* inputs;
    uint32_t input_count;
    // outputs[i] is nframes long. The callback writes every frame of every
    // output it is given, silence included: a backend buffer holds whatever
    // the last cycle left in it.
    float* const* outputs;
    uint32_t output_count;
    uint32_t nframes;
    // The backend's frame counter at the start of this block. Never
    // decreases. Equals the previous frame_time + nframes unless frames were
    // lost or the clock source changed.
    uint64_t frame_time;
    // CLOCK_MONOTONIC (std::chrono::steady_clock on Linux), in nanoseconds,
    // at the start of this period as the backend's server measured it: for
    // JACK, jack_get_cycle_times' current_usecs. The engine stamps the
    // plugin hosts' clock with it (ADR-0012 §1.1, auditor L2), so the
    // callback never reads a clock itself; on a machine whose clocksource
    // is not tsc that read would be a real syscall. Never decreases. 0 when
    // the backend could not tell for this period; the engine then carries
    // the last one forward by the period.
    uint64_t callback_ns;
};

// Implemented by the engine (engine/core's Session); called by a backend.
class ProcessCallback {
public:
    // Called once per period on a thread the backend controls, normally its
    // real-time thread, never concurrently with itself. The rules of
    // CLAUDE.md apply to everything it reaches: no allocation, no lock, no
    // I/O, no logging, no throw.
    virtual void process(const AudioBlock& block) noexcept SONORA_RT = 0;

protected:
    // A backend holds a reference and never owns or deletes the callback.
    ~ProcessCallback() = default;
};

}  // namespace sonora::backend
