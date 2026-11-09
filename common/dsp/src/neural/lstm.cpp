// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LSTM kernel (lstm.h, ADR-0111): the NAM trainer's stacked PyTorch
// LSTM cells and linear head, one sample at a time.
//
// Per sample and layer, with xh = [x; h] the layer's input and last hidden
// state (S. Hochreiter and J. Schmidhuber, "Long Short-Term Memory", 1997,
// in PyTorch's torch.nn.LSTM form and gate order i, f, g, o):
//   z = W xh + b                      one product, lstm_kernels.h
//   i, f, o = sigmoid(z_i, z_f, z_o)  sigmoid(v) = 0.5 (tanh(v / 2) + 1)
//   g = tanh(z_g)
//   c = f c + i g;  h = o tanh(c)
// and the output is head_w . h_last + head_b.
//
// Why the sigmoid through tanh: one rational tanh (vec_math.h, max error
// 3.2e-7) then serves all four gates in a single pass of the table's tanh
// over the 4H gates, and v / 2 and the 0.5 (t + 1) are exact or one
// rounding, so the sigmoid's error is half the tanh's (1.6e-7), under the
// exp-based sigmoid_approx's 2.5e-7. It is the reference's own fast_sigmoid
// form (NeuralAmpModelerCore, NAM/activations.h, MIT); the reference's
// default path uses 1 / (1 + expf(-v)) and tanhf, and test_lstm_equiv holds
// our output within 1e-5 of it.
//
// The products and the tanh come from the run-time table (kernels.h,
// lstm_kernels.h), generic or AVX2, which give the same bits; everything
// else here is scalar, in one order, so the whole output is the same bits
// on either table.
#include "sonora/dsp/neural/lstm.h"

#include <algorithm>
#include <cmath>

#include "kernels.h"
#include "lstm_kernels.h"
#include "sonora/dsp/denormals.h"

namespace sonora::dsp::neural {

namespace detail {

void lstm_matvec_generic(const float* w, int rows, int nin, const float* x, float* out) noexcept SONORA_RT {
    const std::size_t stride = static_cast<std::size_t>(nin);
    for (int o = 0; o < rows; ++o) {
        const float* wr = w + static_cast<std::size_t>(o) * stride;
        float p[8] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        int i = 0;
        for (; i + 8 <= nin; i += 8)
            for (int j = 0; j < 8; ++j) p[j] += wr[i + j] * x[i + j];
        if (i < nin) {
            // The AVX2 product loads the tail masked, so its lanes past the
            // end add 0 x 0 = +0; so do these. (A lane starts at +0 and so
            // never holds -0, which makes the +0 a no-op either way.)
            const int rest = nin - i;
            for (int j = 0; j < 8; ++j) p[j] += j < rest ? wr[i + j] * x[i + j] : 0.0f;
        }
        out[o] = ((p[0] + p[4]) + (p[2] + p[6])) + ((p[1] + p[5]) + (p[3] + p[7]));
    }
}

}  // namespace detail

namespace {

// As WaveNet's: the guitar sample is held to +-16 (24 dB over full scale).
constexpr float kInputLimit = 16.0f;
// prepare() holds max_frames to this, as the WaveNet does. The LSTM has no
// per-block buffer, so it bounds nothing here but keeps the two kernels'
// contracts the same.
constexpr int kMaxBlock = 1024;
// The prewarm is half a second at the capture's rate; a rate over this is
// read as this, so a corrupt rate cannot ask for hours of warm-up.
constexpr double kMaxSampleRate = 768000.0;

// The product that goes with the table: the SIMD one exactly when the
// table is SIMD (both come from the same CPU check, lstm_kernels.h).
detail::LstmMatvec matvec_for(const detail::Kernels* k) noexcept SONORA_RT {
    if (k == &detail::generic_kernels()) return &detail::lstm_matvec_generic;
    const detail::LstmMatvec simd = detail::lstm_simd_matvec();
    return simd != nullptr ? simd : &detail::lstm_matvec_generic;
}

bool all_finite(const std::vector<float>& v, std::size_t* bad) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (!std::isfinite(v[i])) {
            *bad = i;
            return false;
        }
    return true;
}

}  // namespace

Lstm::Lstm() noexcept : kernels_(&detail::generic_kernels()) {}

void Lstm::allow_simd(bool allowed) noexcept {
    allow_simd_ = allowed;
    const detail::Kernels* simd = allowed ? detail::simd_kernels() : nullptr;
    kernels_ = simd != nullptr ? simd : &detail::generic_kernels();
}

const char* Lstm::kernels_name() const noexcept { return kernels_->name; }

bool Lstm::load(const LstmSpec& spec, const std::vector<float>& weights, std::string* error) {
    layers_.clear();
    head_w_.clear();
    gates_.clear();
    inputs_.clear();
    max_frames_ = 0;
    spec_ = LstmSpec{};
    auto fail = [error](const std::string& why) {
        if (error != nullptr) *error = why;
        return false;
    };
    const std::string refused = lstm_check(spec);
    if (!refused.empty()) return fail(refused);
    const std::size_t expected = lstm_weight_count(spec);
    if (weights.size() != expected)
        return fail("an LSTM of this shape needs " + std::to_string(expected) + " weights, the file has " +
                    std::to_string(weights.size()));
    std::size_t bad = 0;
    if (!all_finite(weights, &bad)) return fail("weight " + std::to_string(bad) + " is not a finite number");

    const std::size_t h = static_cast<std::size_t>(spec.hidden_size);
    auto it = weights.begin();
    auto take = [&it](std::vector<float>& dst, std::size_t n) {
        dst.assign(it, it + static_cast<std::ptrdiff_t>(n));
        it += static_cast<std::ptrdiff_t>(n);
    };
    std::vector<Layer> layers(static_cast<std::size_t>(spec.num_layers));
    for (std::size_t l = 0; l < layers.size(); ++l) {
        Layer& layer = layers[l];
        layer.input_size = l == 0 ? spec.input_size : spec.hidden_size;
        const std::size_t nin = static_cast<std::size_t>(layer.input_size) + h;
        take(layer.w, 4 * h * nin);
        take(layer.b, 4 * h);
        take(layer.h0, h);
        take(layer.c0, h);
        layer.xh.assign(nin, 0.0f);
        layer.c.assign(h, 0.0f);
        layer.warm_h = layer.h0;
        layer.warm_c = layer.c0;
    }
    take(head_w_, h);
    head_b_ = *it;
    layers_ = std::move(layers);
    gates_.assign(4 * h, 0.0f);
    spec_ = spec;
    allow_simd(allow_simd_);  // reads CPUID once per load, off the audio thread
    reset();
    return true;
}

void Lstm::prepare(int max_frames, double sample_rate) {
    if (layers_.empty()) return;
    max_frames_ = std::min(std::max(1, max_frames), kMaxBlock);
    // The reference's prewarm (NeuralAmpModelerCore, LSTM::GetPrewarmSamples):
    // int(0.5 x rate) samples of silence from the file's h and c, at least 1.
    const double rate = sample_rate > 0.0 ? std::min(sample_rate, kMaxSampleRate) : 0.0;
    const int warm = std::max(1, static_cast<int>(0.5 * rate));
    for (Layer& layer : layers_) {
        layer.warm_h = layer.h0;
        layer.warm_c = layer.c0;
    }
    reset();
    {
        // As process() runs it, so the prewarmed state is the one the audio
        // thread would have reached, to the bit.
        const DenormalGuard denormals;
        for (int t = 0; t < warm; ++t) step(0.0f);
    }
    for (Layer& layer : layers_) {
        const std::size_t in = static_cast<std::size_t>(layer.input_size);
        std::copy(layer.xh.begin() + static_cast<std::ptrdiff_t>(in), layer.xh.end(), layer.warm_h.begin());
        layer.warm_c = layer.c;
    }
    reset();
}

void Lstm::reset() noexcept SONORA_RT {
    for (Layer& layer : layers_) {
        const auto in = static_cast<std::ptrdiff_t>(layer.input_size);
        std::fill(layer.xh.begin(), layer.xh.begin() + in, 0.0f);
        std::copy(layer.warm_h.begin(), layer.warm_h.end(), layer.xh.begin() + in);
        std::copy(layer.warm_c.begin(), layer.warm_c.end(), layer.c.begin());
    }
}

void Lstm::process(const float* in, float* out, int frames) noexcept SONORA_RT {
    if (layers_.empty() || max_frames_ == 0) {
        for (int t = 0; t < frames; ++t) out[t] = 0.0f;
        return;
    }
    const DenormalGuard denormals;
    for (int t = 0; t < frames; ++t) {
        const float x = in[t];
        // x - x is 0 for every finite x and NaN otherwise (no -ffast-math).
        const float finite = (x - x == 0.0f) ? x : 0.0f;
        out[t] = step(std::min(std::max(finite, -kInputLimit), kInputLimit));
    }
}

float Lstm::step(float x) noexcept SONORA_RT {
    const detail::LstmMatvec matvec = matvec_for(kernels_);
    const auto tanh_rows = kernels_->tanh;
    const int hs = spec_.hidden_size;
    float* g = gates_.data();
    const float* below = &x;  // the layer's input: the sample, then the layer below's h
    for (Layer& layer : layers_) {
        float* xh = layer.xh.data();
        const int in = layer.input_size;
        for (int k = 0; k < in; ++k) xh[k] = below[k];
        matvec(layer.w.data(), 4 * hs, in + hs, xh, g);
        const float* b = layer.b.data();
        for (int r = 0; r < 4 * hs; ++r) g[r] += b[r];
        // Halve i and f (rows 0..2H) and o (3H..4H) for the sigmoid; g's
        // rows (2H..3H) take tanh as they are. Halving is exact.
        for (int r = 0; r < 2 * hs; ++r) g[r] *= 0.5f;
        for (int r = 3 * hs; r < 4 * hs; ++r) g[r] *= 0.5f;
        tanh_rows(g, 4 * hs);
        float* c = layer.c.data();
        float* gg = g + 2 * hs;
        for (int k = 0; k < hs; ++k) {
            const float si = 0.5f * (g[k] + 1.0f);
            const float sf = 0.5f * (g[hs + k] + 1.0f);
            c[k] = sf * c[k] + si * gg[k];
            gg[k] = c[k];  // g's row is spent: tanh(c) goes through it
        }
        tanh_rows(gg, hs);
        float* h = xh + in;
        const float* go = g + 3 * hs;
        for (int k = 0; k < hs; ++k) h[k] = 0.5f * (go[k] + 1.0f) * gg[k];
        below = h;
    }
    float y = 0.0f;
    matvec(head_w_.data(), 1, hs, below, &y);
    return y + head_b_;
}

}  // namespace sonora::dsp::neural
