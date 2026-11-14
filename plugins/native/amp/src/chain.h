// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the amp's audio thread runs for one capture: the network at its own
// rate, between the two resampler stages when the session is at another
// one (amp_resampler.h). Mono in, mono out. Built, prepared and timed on a
// control thread (build(), time_chain()); process() and reset() are
// real-time.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sonora/native/amp_model.h"
#include "sonora/dsp/resample.h"
#include "sonora/rt.h"

namespace sonora::native::amp {

class ModelChain {
public:
    // Not real-time. Prepares `model` for blocks of up to max_frames session
    // samples. False with *error set when the rates cannot be converted.
    bool build(std::unique_ptr<NeuralModel> model, double capture_rate, double session_rate,
               std::uint32_t max_frames, std::string* error);

    // Session samples, exact: the resampler pair's delay, or the network's
    // own when no conversion is needed.
    std::uint32_t latency() const noexcept { return latency_; }
    bool resampling() const noexcept { return plan_.resample; }
    std::uint32_t max_frames() const noexcept { return max_frames_; }

    void reset() noexcept SONORA_RT;
    // frames <= max_frames(). in and out may alias.
    void process(const float* in, float* out, std::uint32_t frames) noexcept SONORA_RT;

private:
    std::unique_ptr<NeuralModel> model_;
    dsp::RatePlan plan_;
    dsp::Resampler up_, down_;
    std::vector<float> capture_;   // the network's block at the capture rate
    std::vector<float> fifo_;      // session-rate outputs not yet handed out
    std::uint32_t fill_ = 0;
    std::uint32_t latency_ = 0;
    std::uint32_t max_frames_ = 0;
};

// The cost of a chain on this machine, ADR-0011 §3.5's way: per-block
// wall-clock time of `blocks` blocks of `block` frames of a guitar-like
// test signal, the per-block minimum of `passes` runs, as p99.9 and max.
struct ChainCost {
    double p999_us = 0.0;
    double max_us = 0.0;
    // A run's total time was already over blocks x abort_max_us, so its
    // mean, and therefore its max, was over the bar: the timing stopped.
    bool stopped_early = false;
};
// Not real-time; leaves the chain reset.
ChainCost time_chain(ModelChain& chain, std::uint32_t block, int blocks, int passes, double abort_max_us);

}  // namespace sonora::native::amp
