// SPDX-License-Identifier: GPL-3.0-or-later
//
// The WaveNet-style dilated convolution stack that NAM-class amp captures
// use (ADR-0011 §4.3, stage N3), as a fixed-architecture kernel.
//
// The architecture is the one the NAM project publishes in its open `.nam`
// format (MIT, github.com/sdatkinson/neural-amp-modeler; the trainer's
// "WaveNet" with its "standard", "lite", "feather" and "nano" sizes):
// - A stack of layer arrays. Array a re-channels its input (the guitar
//   sample for the first array, the previous array's last layer output
//   after) to `channels` with a 1x1 convolution without bias.
// - Each layer: a causal dilated convolution of `kernel_size` taps,
//   tap k reading the input dilation x (kernel_size - 1 - k) samples back,
//   plus a bias, plus the guitar sample times a per-channel "input mixin";
//   then the activation (or, gated, activation(top half) x sigmoid(bottom
//   half)); then a residual 1x1 back to `channels`, added to the layer's
//   input. Every layer's activated output is summed into the array's head.
// - The array's head is a 1x1 convolution (bias per `head_bias`) to
//   `head_size` channels, which seeds the next array's head sum. The last
//   array's single head channel times the head scale is the output.
// The arithmetic is our own C++17 (ADR-0011 §4.3: the NAM reference
// implementation is the test oracle, never linked). The weight order is the
// format's (plugins/native/ampcab/src/nam_file.cpp reads it; WaveNet::load
// documents it).
//
// Only what a `.nam` file of format 0.5 says is supported: one input
// channel, condition_size 1, no bottleneck, grouped convolution, FiLM,
// head1x1 or post-stack head. Anything else is refused at load with the
// reason, never run approximately.
//
// Real-time contract: load() and prepare() allocate; reset() and process()
// do fixed arithmetic only (no allocation, no lock, no throw).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "sonora/rt.h"

namespace sonora::dsp::neural {

// The activations a `.nam` file names by string. LeakyHardtanh and PReLU
// carry parameters in their JSON form and are not supported yet.
enum class Activation {
    tanh,        // "Tanh"
    fast_tanh,   // "Fasttanh"
    hard_tanh,   // "Hardtanh"
    relu,        // "ReLU"
    leaky_relu,  // "LeakyReLU", negative slope 0.01
    sigmoid,     // "Sigmoid"
    silu,        // "SiLU"
    hard_swish,  // "Hardswish"
    softsign,    // "Softsign"
};

// The format's name for it, and back. False when the name is unknown.
const char* activation_name(Activation a) noexcept;
bool activation_from_name(const std::string& name, Activation* out) noexcept;

// Applies `a` in place to n values. Exposed for the kernel test.
void apply_activation(Activation a, float* x, int n) noexcept SONORA_RT;

struct LayerArraySpec {
    int input_size = 1;      // 1 for the first array, the previous array's channels after
    int condition_size = 1;  // always 1: the condition is the guitar sample
    int channels = 0;
    int head_size = 0;       // the next array's channels; 1 for the last array
    int kernel_size = 0;
    std::vector<int> dilations;
    Activation activation = Activation::tanh;
    bool gated = false;
    bool head_bias = false;
};

struct WaveNetSpec {
    std::vector<LayerArraySpec> arrays;
};

// "" when the spec is one this kernel runs; otherwise why it is not.
std::string wavenet_check(const WaveNetSpec& spec);
// The number of weights a file must carry for `spec`, the trailing head
// scale included.
std::size_t wavenet_weight_count(const WaveNetSpec& spec);
// Samples of history the output depends on, past the current one.
int wavenet_receptive_field(const WaveNetSpec& spec);

class WaveNet {
public:
    WaveNet() = default;
    WaveNet(const WaveNet&) = delete;
    WaveNet& operator=(const WaveNet&) = delete;

    // Not real-time. Takes the spec and the flat weight list in the
    // format's order, the head scale last. False, with *error set when
    // given, if the spec is unsupported or the count is wrong; the kernel
    // is then unloaded. Weight order, per layer array:
    //   re-channel  [channels][input_size]
    //   per layer   conv [G*channels][channels][kernel_size], conv bias [G*channels],
    //               input mixin [G*channels][1], residual 1x1 [channels][channels],
    //               residual bias [channels]          (G = 2 when gated, else 1)
    //   head        [head_size][channels], head bias [head_size] when head_bias
    bool load(const WaveNetSpec& spec, const std::vector<float>& weights, std::string* error);
    bool loaded() const { return !arrays_.empty(); }

    // Not real-time. Sizes every buffer for blocks of up to max_frames and
    // computes the prewarmed state reset() restores: the network after
    // receptive_field() + 1 samples of silence, as the reference
    // implementation's prewarm leaves it.
    void prepare(int max_frames);

    // Restores the prewarmed state. Fills each layer's history with its
    // channel's steady value: no allocation, O(history) stores.
    void reset() noexcept SONORA_RT;

    // in and out may alias. Any frame count; blocks over max_frames are
    // run in pieces. Non-finite input is read as silence and input is held
    // to +-16 (24 dB over full scale), so the output stays finite.
    void process(const float* in, float* out, int frames) noexcept SONORA_RT;

    int receptive_field() const { return receptive_field_; }
    int max_frames() const { return max_frames_; }
    float head_scale() const { return head_scale_; }

private:
    struct Layer {
        int dilation = 1;
        int lookback = 0;            // dilation * (kernel_size - 1)
        int capacity = 0;            // history frames per channel
        int write = 0;               // where this block's frame 0 goes
        std::vector<float> conv_w;   // [o][k][j], o < G*C (the file's is [o][j][k])
        std::vector<float> conv_b;   // [o]
        std::vector<float> mixin_w;  // [o]
        std::vector<float> res_w;    // [o][j], C x C
        std::vector<float> res_b;    // [o]
        std::vector<float> history;  // [c][capacity], the layer's input
        std::vector<float> steady;   // [c], the prewarmed value
    };
    struct Array {
        LayerArraySpec spec;
        std::vector<float> rechannel_w;  // [C][input_size]
        std::vector<float> head_w;       // [H][C]
        std::vector<float> head_b;       // [H] or empty
        std::vector<Layer> layers;
        std::vector<float> head_sum;     // [C][max_frames]
        std::vector<float> head_out;     // [H][max_frames]
        std::vector<float> layer_out;    // [C][max_frames], the last layer's output
    };

    void run(const float* in, float* out, int frames) noexcept SONORA_RT;
    // Makes room for `frames` at each layer's write position.
    static void make_room(Layer& layer, int channels, int frames) noexcept SONORA_RT;

    std::vector<Array> arrays_;
    std::vector<float> condition_;  // [max_frames]
    std::vector<float> z_;          // [2*C_max][max_frames]
    std::vector<float> acc_;        // [C_max][max_frames]
    std::vector<const float*> inputs_;  // one product's input rows, K x C at most
    float head_scale_ = 0.0f;
    int max_frames_ = 0;
    int receptive_field_ = 0;
};

}  // namespace sonora::dsp::neural
