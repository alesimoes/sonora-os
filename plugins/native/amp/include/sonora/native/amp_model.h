// SPDX-License-Identifier: GPL-3.0-or-later
//
// The amp's network behind one interface (ADR-0111 §5): common/dsp/neural's
// WaveNet or LSTM, chosen once from the file's architecture. The amp calls
// it once per block, never per sample (the LSTM's recurrence is the hot
// loop; a virtual call inside it would cost what the arithmetic does).
//
// prepare() allocates and is for a control thread; reset() and process()
// are the kernels' real-time contract (fixed arithmetic, finite output for
// any input).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "sonora/rt.h"

namespace sonora::ampcab {
struct NamModel;
}

namespace sonora::native::amp {

class NeuralModel {
public:
    virtual ~NeuralModel() = default;
    // Not real-time. `rate` is the capture's: the kernel runs there.
    virtual void prepare(int max_frames, double rate) = 0;
    virtual void reset() noexcept SONORA_RT = 0;
    // in and out may alias.
    virtual void process(const float* in, float* out, int frames) noexcept SONORA_RT = 0;
    // Samples at the capture's rate. 0 for both kernels: out[t] depends on
    // in[0..t] and nothing is looked ahead.
    virtual std::uint32_t latency() const noexcept { return 0; }
};

// The kernel for a parsed `.nam` (ampcab/nam_file.h), weights loaded, not
// yet prepared. Null with *error set (one sentence) when the kernel refuses
// it; the reader has already checked the same, so that is a guard.
std::unique_ptr<NeuralModel> make_neural_model(const ampcab::NamModel& model, std::string* error);

}  // namespace sonora::native::amp
