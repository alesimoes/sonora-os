// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LSTM that NAM-class amp captures use beside the WaveNet (ADR-0011
// §4.3, stage N3; ADR-0111), as a fixed-architecture kernel.
//
// The architecture is the NAM trainer's "LSTM" as its open `.nam` format
// stores it (MIT, github.com/sdatkinson/neural-amp-modeler; the reference
// is NeuralAmpModelerCore's NAM/lstm.cpp, the test oracle, never linked):
// - num_layers stacked PyTorch LSTM cells. Layer 0 reads the guitar
//   sample (input_size 1), layer l > 0 reads layer l - 1's hidden state.
// - Per sample and layer, with x the layer's input and h its last hidden
//   state: z = W [x; h] + b, z split in four H-row gates in PyTorch's
//   order i, f, g, o; c = sigmoid(f) c + sigmoid(i) tanh(g);
//   h = sigmoid(o) tanh(c).
// - The output is head_w . h_last + head_b. No head scale, no input scale.
// The arithmetic is our own C++17. Its tanh and sigmoid are vec_math.h's
// rational approximations and its products the run-time SIMD table the
// WaveNet uses (src/neural/kernels.h), so every table gives the same bits.
//
// Real-time contract (the WaveNet's): load() and prepare() allocate;
// reset() and process() do fixed arithmetic only (no allocation, no lock,
// no throw).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "sonora/rt.h"

namespace sonora::dsp::neural {

// The `.nam` config of an LSTM: {"num_layers", "input_size", "hidden_size"}.
struct LstmSpec {
    int num_layers = 0;
    int input_size = 1;  // always 1: one input channel
    int hidden_size = 0;
};

// The caps lstm_check holds a spec to. Per sample the kernel does about
// lstm_weight_count() multiply-adds, so the weight cap bounds both what
// load() copies and the work a block can ask for; whether a spec that
// passes fits class E on this machine is the processor's load-time
// benchmark (ADR-0111 §5), not this check.
constexpr int kLstmMaxLayers = 4;
constexpr int kLstmMaxHidden = 128;
constexpr std::size_t kLstmMaxWeights = 131072;

// "" when the spec is one this kernel runs; otherwise one sentence saying
// why it is not.
std::string lstm_check(const LstmSpec& spec);

// The number of weights a file must carry for `spec`, in the format's
// order: per layer W [4H][in + H] (in = 1 for layer 0, H after), bias [4H]
// (the trainer's bias_ih + bias_hh, summed on export), initial hidden h
// [H], initial cell c [H]; then the head [1][H] and its bias [1].
// 0 when num_layers or hidden_size is under 1; the head (H + 1) is the
// last term. load() requires lstm_check() to pass first.
std::size_t lstm_weight_count(const LstmSpec& spec);

namespace detail {
struct Kernels;
}  // namespace detail

class Lstm {
public:
    Lstm() noexcept;
    Lstm(const Lstm&) = delete;
    Lstm& operator=(const Lstm&) = delete;

    // Not real-time. Takes the spec and the flat weight list in the
    // format's order (lstm_weight_count). False, with *error set when
    // given, if the spec is refused by lstm_check or the count is wrong;
    // the kernel is then unloaded.
    bool load(const LstmSpec& spec, const std::vector<float>& weights, std::string* error);
    bool loaded() const { return !layers_.empty(); }

    // Not real-time. Sizes every buffer for blocks of up to max_frames
    // (held to 1024; process() runs longer blocks in pieces) and computes
    // the prewarmed state reset() restores: the network from the file's
    // initial h and c after max(1, int(0.5 x sample_rate)) samples of
    // silence, as the reference's prewarm leaves it. sample_rate is the
    // capture's rate, the one the kernel runs at.
    void prepare(int max_frames, double sample_rate);

    // Restores the prewarmed state: O(num_layers x H) stores.
    void reset() noexcept SONORA_RT;

    // in and out may alias. Any frame count. Non-finite input is read as
    // silence and input is held to +-16 (24 dB over full scale), so the
    // output stays finite. Zero latency: out[t] depends on in[0..t].
    void process(const float* in, float* out, int frames) noexcept SONORA_RT;

    // Not real-time. As WaveNet::allow_simd: the SIMD table or the generic
    // one, same bits; allowed by default.
    void allow_simd(bool allowed) noexcept;
    // "generic" or the SIMD table's name ("avx2"), for the record.
    const char* kernels_name() const noexcept;

    int max_frames() const { return max_frames_; }
    const LstmSpec& spec() const { return spec_; }

private:
    struct Layer {
        int input_size = 1;
        std::vector<float> w;        // [4H][input_size + H], the file's order
        std::vector<float> b;        // [4H]
        std::vector<float> h0;       // [H], the file's initial hidden state
        std::vector<float> c0;       // [H], the file's initial cell state
        std::vector<float> xh;       // [input_size + H]: this sample's input, then h
        std::vector<float> c;        // [H]
        std::vector<float> warm_h;   // [H], the prewarmed h reset() restores
        std::vector<float> warm_c;   // [H], the prewarmed c
    };

    // One sample through every layer and the head.
    float step(float x) noexcept SONORA_RT;

    LstmSpec spec_;
    std::vector<Layer> layers_;
    std::vector<float> head_w_;              // [H]
    float head_b_ = 0.0f;
    std::vector<float> gates_;               // [4H]
    std::vector<const float*> inputs_;       // [input_size + H], the product's input rows
    const detail::Kernels* kernels_;         // never null
    bool allow_simd_ = true;
    int max_frames_ = 0;
};

}  // namespace sonora::dsp::neural
