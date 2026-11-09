// SPDX-License-Identifier: GPL-3.0-or-later
//
// The amp suite's captures, made here, not recorded: an identity network
// (the seam's test double, so the resampler, the latency and the level are
// measured with nothing nonlinear in the way) and small LSTMs with
// deterministic weights in the NAM layout. Nothing here is a capture of a
// real amplifier.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sonora/ampcab/nam_file.h"
#include "sonora/dsp/neural/lstm.h"
#include "sonora/native/amp.h"

namespace sonora::native::suites::amp_test {

class IdentityModel final : public native::amp::NeuralModel {
public:
    void prepare(int, double) override {}
    void reset() noexcept override {}
    void process(const float* in, float* out, int frames) noexcept override {
        for (int i = 0; i < frames; ++i) out[i] = in[i];
    }
};

inline native::amp::CaptureSource identity(double rate, bool has_loudness = false, double loudness_db = 0.0) {
    native::amp::CaptureSource s;
    s.make = [](std::string*) { return std::unique_ptr<native::amp::NeuralModel>(new IdentityModel); };
    s.rate = rate;
    s.has_loudness = has_loudness;
    s.loudness_db = loudness_db;
    return s;
}

// An LSTM of `layers` x `hidden` with weights from a fixed LCG, scaled so
// the network is mildly nonlinear at a DI's level, at `rate`, with
// metadata.loudness -20 dB.
inline ampcab::NamModel lstm(int layers, int hidden, double rate = 48000.0, std::uint32_t seed = 7u) {
    ampcab::NamModel m;
    m.version = "0.5.4";
    m.architecture = "LSTM";
    m.lstm.num_layers = layers;
    m.lstm.input_size = 1;
    m.lstm.hidden_size = hidden;
    m.weights.resize(dsp::neural::lstm_weight_count(m.lstm));
    std::uint32_t s = seed;
    for (float& w : m.weights) {
        s = s * 1664525u + 1013904223u;
        w = static_cast<float>((static_cast<double>(s >> 8) / 16777216.0 - 0.5) * 0.8);
    }
    m.sample_rate = rate;
    m.sample_rate_given = true;
    m.has_loudness = true;
    m.loudness_db = -20.0;
    return m;
}

}  // namespace sonora::native::suites::amp_test
