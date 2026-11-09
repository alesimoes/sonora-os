// SPDX-License-Identifier: GPL-3.0-or-later
//
// The WaveNet kernel (wavenet.h). Layout and loop order, for speed:
// - Every signal is planar, one row of frames per channel, so the inner
//   loop of every product runs over the block's frames: contiguous and the
//   same length for every channel count. Every product, the dilated
//   convolution included, is one matmul_rows() over a list of input rows;
//   it and the tanh and sigmoid come from a kernel table (kernels.h), the
//   generic one or AVX2 chosen at run time, to the same bits.
// - Each layer keeps the history of its input in a linear buffer of
//   2 x lookback + max_frames frames per channel. A block is written at
//   `write`; when the next block would not fit, the last `lookback` frames
//   are copied to the front. Every tap then reads a contiguous run, and the
//   copy happens once every (lookback + max_frames) / frames blocks.
// - Sums are taken in the reference implementation's order where it is
//   cheap to (taps in order, then the bias, then the mixin; the residual
//   product, then its bias, then the input), so the equivalence test
//   measures the kernel and not a summation order.
#include "sonora/dsp/neural/wavenet.h"

#include <algorithm>
#include <cstring>

#include "kernels.h"
#include "sonora/dsp/denormals.h"
#include "vec_math.h"

namespace sonora::dsp::neural {

namespace {

struct ActivationName {
    Activation a;
    const char* name;
};
const ActivationName kActivationNames[] = {
    {Activation::tanh, "Tanh"},           {Activation::fast_tanh, "Fasttanh"},
    {Activation::hard_tanh, "Hardtanh"},  {Activation::relu, "ReLU"},
    {Activation::leaky_relu, "LeakyReLU"}, {Activation::sigmoid, "Sigmoid"},
    {Activation::silu, "SiLU"},           {Activation::hard_swish, "Hardswish"},
    {Activation::softsign, "Softsign"},
};

// Caps on what a file may ask for, so a hostile or corrupt one is refused
// with a message rather than sizing gigabytes in prepare(). The trainer's
// largest preset ("standard") has 16 channels, kernel 3, dilations to 512
// and 20 layers; these leave an order of magnitude above it.
constexpr int kMaxChannels = 128;
constexpr int kMaxKernel = 32;
constexpr int kMaxDilation = 1 << 15;
constexpr int kMaxLayers = 256;
constexpr int kMaxArrays = 16;
// Every dimension within its cap is not enough: the totals multiply (review
// of #318: one 52 KB file, 16 channels, 32 layers of dilation 32768, took
// 19 s and 132 MiB in prepare(); at 128 channels, minutes towards 1 GiB).
// So the totals are capped too. "standard" needs, against each:
//   receptive field     4 092 samples          cap 65 536
//   history             98 208 floats          cap 4 Mi floats (16 MiB)
//   weights             13 802                 cap 1 Mi
//   layer rows          240 (layers x channels) cap 4 096
//   warm-up work        5.6e7 multiply-adds    cap 2^32 (a few seconds here)
// The warm-up cap is weights x (receptive field + 1). A 64-channel gated net
// of the standard shape (kernel 3, 20 layers, dilations to 512) has 580 k
// weights x 4 093 = 2.4e9, which the earlier 2^30 refused; 2^32 (4.3e9) takes
// it and still refuses the 7.7e9 case of test_neural.
constexpr long kMaxReceptiveField = 1L << 16;
constexpr unsigned long long kMaxHistoryFloats = 1ULL << 22;
constexpr unsigned long long kMaxWeights = 1ULL << 20;
constexpr unsigned long long kMaxLayerRows = 4096;
constexpr unsigned long long kMaxWarmupWork = 1ULL << 32;
// prepare() sizes every row for at most this many frames; process() runs a
// longer block in pieces, so the cap costs nothing but bounds memory by
// kMaxLayerRows x kMaxBlock floats whatever the host asks for.
constexpr int kMaxBlock = 1024;

// The guitar sample is held to +-16 before it enters the network.
constexpr float kInputLimit = 16.0f;

int gate_factor(const LayerArraySpec& a) { return a.gated ? 2 : 1; }

}  // namespace

WaveNet::WaveNet() noexcept : kernels_(&detail::generic_kernels()) {}

void WaveNet::allow_simd(bool allowed) noexcept {
    allow_simd_ = allowed;
    const detail::Kernels* simd = allowed ? detail::simd_kernels() : nullptr;
    kernels_ = simd ? simd : &detail::generic_kernels();
}

const char* WaveNet::kernels_name() const noexcept { return kernels_->name; }

const char* activation_name(Activation a) noexcept {
    for (const auto& n : kActivationNames)
        if (n.a == a) return n.name;
    return "?";
}

bool activation_from_name(const std::string& name, Activation* out) noexcept {
    for (const auto& n : kActivationNames) {
        if (name == n.name) {
            *out = n.a;
            return true;
        }
    }
    return false;
}

void apply_activation(Activation a, float* x, int n) noexcept SONORA_RT {
    // One loop per case, the switch outside it, so each loop vectorises.
    switch (a) {
        case Activation::tanh:
            for (int i = 0; i < n; ++i) x[i] = detail::tanh_approx(x[i]);
            break;
        case Activation::fast_tanh:
            for (int i = 0; i < n; ++i) x[i] = detail::fast_tanh(x[i]);
            break;
        case Activation::hard_tanh:
            for (int i = 0; i < n; ++i) {
                const float t = x[i] < -1.0f ? -1.0f : x[i];
                x[i] = t > 1.0f ? 1.0f : t;
            }
            break;
        case Activation::relu:
            for (int i = 0; i < n; ++i) x[i] = x[i] > 0.0f ? x[i] : 0.0f;
            break;
        case Activation::leaky_relu:
            // The reference's default slope, a double 0.01 stored as float.
            for (int i = 0; i < n; ++i) x[i] = x[i] > 0.0f ? x[i] : 0.01f * x[i];
            break;
        case Activation::sigmoid:
            for (int i = 0; i < n; ++i) x[i] = detail::sigmoid_approx(x[i]);
            break;
        case Activation::silu:
            for (int i = 0; i < n; ++i) x[i] = x[i] * detail::sigmoid_approx(x[i]);
            break;
        case Activation::hard_swish:
            for (int i = 0; i < n; ++i) {
                const float t = x[i] + 3.0f;
                const float c = t < 0.0f ? 0.0f : (t > 6.0f ? 6.0f : t);
                x[i] = x[i] * c * (1.0f / 6.0f);
            }
            break;
        case Activation::softsign:
            for (int i = 0; i < n; ++i) x[i] = x[i] / (1.0f + std::fabs(x[i]));
            break;
    }
}

std::string wavenet_check(const WaveNetSpec& spec) {
    if (spec.arrays.empty()) return "a WaveNet needs at least one layer array";
    if (spec.arrays.size() > static_cast<std::size_t>(kMaxArrays))
        return "more than " + std::to_string(kMaxArrays) + " layer arrays";
    int layers = 0;
    for (std::size_t i = 0; i < spec.arrays.size(); ++i) {
        const LayerArraySpec& a = spec.arrays[i];
        const std::string where = "layer array " + std::to_string(i) + ": ";
        if (a.channels < 1 || a.channels > kMaxChannels)
            return where + "channels must be 1 to " + std::to_string(kMaxChannels);
        if (a.head_size < 1 || a.head_size > kMaxChannels)
            return where + "head_size must be 1 to " + std::to_string(kMaxChannels);
        if (a.kernel_size < 1 || a.kernel_size > kMaxKernel)
            return where + "kernel_size must be 1 to " + std::to_string(kMaxKernel);
        if (a.condition_size != 1)
            return where + "condition_size must be 1 (the condition is the input sample)";
        if (a.dilations.empty()) return where + "no layers (dilations is empty)";
        for (int d : a.dilations)
            if (d < 1 || d > kMaxDilation)
                return where + "dilations must be 1 to " + std::to_string(kMaxDilation);
        layers += static_cast<int>(a.dilations.size());
        if (layers > kMaxLayers) return "more than " + std::to_string(kMaxLayers) + " layers";
        if (i == 0 && a.input_size != 1)
            return where + "input_size must be 1 (one input channel)";
        if (i > 0) {
            const LayerArraySpec& p = spec.arrays[i - 1];
            if (a.input_size != p.channels)
                return where + "input_size must equal the previous array's channels";
            if (a.channels != p.head_size)
                return where + "channels must equal the previous array's head_size";
        }
    }
    if (spec.arrays.back().head_size != 1) return "the last layer array's head_size must be 1 (one output)";
    const long rf = wavenet_receptive_field(spec);
    if (rf > kMaxReceptiveField)
        return "the receptive field is over " + std::to_string(kMaxReceptiveField) + " samples";
    unsigned long long history = 0, rows = 0;
    for (const LayerArraySpec& a : spec.arrays)
        for (int d : a.dilations) {
            history += 2ULL * static_cast<unsigned long long>(a.channels) * static_cast<unsigned long long>(d) *
                       static_cast<unsigned long long>(a.kernel_size - 1);
            rows += static_cast<unsigned long long>(a.channels);
        }
    if (history > kMaxHistoryFloats)
        return "the history would be over " + std::to_string(kMaxHistoryFloats) + " samples";
    if (rows > kMaxLayerRows)
        return "layers x channels is over " + std::to_string(kMaxLayerRows);
    const unsigned long long weights = wavenet_weight_count(spec);
    if (weights > kMaxWeights) return "over " + std::to_string(kMaxWeights) + " weights";
    if (weights * static_cast<unsigned long long>(rf + 1) > kMaxWarmupWork)
        return "the warm-up (weights x receptive field) is over " + std::to_string(kMaxWarmupWork) +
               " multiply-adds";
    return {};
}

std::size_t wavenet_weight_count(const WaveNetSpec& spec) {
    std::size_t n = 0;
    for (const LayerArraySpec& a : spec.arrays) {
        const std::size_t c = static_cast<std::size_t>(a.channels);
        const std::size_t g = static_cast<std::size_t>(gate_factor(a));
        const std::size_t k = static_cast<std::size_t>(a.kernel_size);
        const std::size_t h = static_cast<std::size_t>(a.head_size);
        n += c * static_cast<std::size_t>(a.input_size);
        const std::size_t per_layer = g * c * c * k + g * c + g * c + c * c + c;
        n += per_layer * a.dilations.size();
        n += h * c + (a.head_bias ? h : 0);
    }
    return n + 1;  // the head scale
}

int wavenet_receptive_field(const WaveNetSpec& spec) {
    long rf = 0;
    for (const LayerArraySpec& a : spec.arrays)
        for (int d : a.dilations) rf += static_cast<long>(d) * (a.kernel_size - 1);
    return static_cast<int>(std::min(rf, kMaxReceptiveField + 1));
}

bool WaveNet::load(const WaveNetSpec& spec, const std::vector<float>& weights, std::string* error) {
    arrays_.clear();
    max_frames_ = 0;
    std::string why = wavenet_check(spec);
    if (why.empty() && weights.size() != wavenet_weight_count(spec)) {
        why = "the architecture needs " + std::to_string(wavenet_weight_count(spec)) +
              " weights, the file has " + std::to_string(weights.size());
    }
    if (!why.empty()) {
        if (error) *error = why;
        return false;
    }
    auto it = weights.begin();
    auto take = [&it](std::vector<float>& v, std::size_t n) {
        v.assign(it, it + static_cast<std::ptrdiff_t>(n));
        it += static_cast<std::ptrdiff_t>(n);
    };
    std::vector<Array> arrays(spec.arrays.size());
    for (std::size_t i = 0; i < spec.arrays.size(); ++i) {
        Array& a = arrays[i];
        a.spec = spec.arrays[i];
        const std::size_t c = static_cast<std::size_t>(a.spec.channels);
        const std::size_t gc = c * static_cast<std::size_t>(gate_factor(a.spec));
        const std::size_t k = static_cast<std::size_t>(a.spec.kernel_size);
        const std::size_t h = static_cast<std::size_t>(a.spec.head_size);
        take(a.rechannel_w, c * static_cast<std::size_t>(a.spec.input_size));
        a.layers.resize(a.spec.dilations.size());
        for (std::size_t l = 0; l < a.layers.size(); ++l) {
            Layer& layer = a.layers[l];
            layer.dilation = a.spec.dilations[l];
            layer.lookback = layer.dilation * (a.spec.kernel_size - 1);
            // The file's [o][j][k], kept as [o][k][j]: one row of products
            // per output channel over the K x C tap inputs, taps in order.
            std::vector<float> file_order;
            take(file_order, gc * c * k);
            layer.conv_w.resize(file_order.size());
            for (std::size_t o = 0; o < gc; ++o)
                for (std::size_t j = 0; j < c; ++j)
                    for (std::size_t kk = 0; kk < k; ++kk)
                        layer.conv_w[(o * k + kk) * c + j] = file_order[(o * c + j) * k + kk];
            take(layer.conv_b, gc);
            take(layer.mixin_w, gc);
            take(layer.res_w, c * c);
            take(layer.res_b, c);
        }
        take(a.head_w, h * c);
        if (a.spec.head_bias) take(a.head_b, h);
    }
    head_scale_ = *it;
    arrays_ = std::move(arrays);
    allow_simd(allow_simd_);  // reads CPUID once per load, off the audio thread
    receptive_field_ = wavenet_receptive_field(spec);
    return true;
}

void WaveNet::prepare(int max_frames) {
    if (arrays_.empty()) return;
    max_frames_ = std::min(std::max(1, max_frames), kMaxBlock);
    const auto mf = static_cast<std::size_t>(max_frames_);
    std::size_t c_max = 1;
    for (Array& a : arrays_) {
        const std::size_t c = static_cast<std::size_t>(a.spec.channels);
        c_max = std::max(c_max, c * static_cast<std::size_t>(gate_factor(a.spec)));
        for (Layer& layer : a.layers) {
            layer.capacity = 2 * layer.lookback + max_frames_;
            layer.history.assign(c * static_cast<std::size_t>(layer.capacity), 0.0f);
            layer.steady.assign(c, 0.0f);
            layer.write = layer.lookback;
        }
        a.head_sum.assign(c * mf, 0.0f);
        a.head_out.assign(static_cast<std::size_t>(a.spec.head_size) * mf, 0.0f);
        a.layer_out.assign(c * mf, 0.0f);
    }
    std::size_t inputs = 1;
    for (const Array& a : arrays_)
        inputs = std::max(inputs, static_cast<std::size_t>(std::max(a.spec.channels * a.spec.kernel_size,
                                                                    a.spec.input_size)));
    condition_.assign(mf, 0.0f);
    z_.assign(c_max * mf, 0.0f);
    acc_.assign(c_max * mf, 0.0f);
    inputs_.assign(inputs, nullptr);

    // The reference prewarms on silence for its receptive field plus one
    // sample. After that every history window holds one value per channel
    // (the network's response to silence), so the state is captured as that
    // value and reset() rebuilds it without running the network again.
    std::vector<float> silence(mf, 0.0f);
    std::vector<float> discard(mf, 0.0f);
    for (int left = receptive_field_ + 1; left > 0; left -= max_frames_)
        run(silence.data(), discard.data(), std::min(left, max_frames_));
    for (Array& a : arrays_) {
        const std::size_t c = static_cast<std::size_t>(a.spec.channels);
        for (Layer& layer : a.layers)
            for (std::size_t ch = 0; ch < c; ++ch)
                layer.steady[ch] = layer.history[ch * static_cast<std::size_t>(layer.capacity) +
                                                 static_cast<std::size_t>(layer.write - 1)];
    }
    reset();
}

void WaveNet::reset() noexcept SONORA_RT {
    for (Array& a : arrays_) {
        const int c = a.spec.channels;
        for (Layer& layer : a.layers) {
            for (int ch = 0; ch < c; ++ch) {
                float* row = layer.history.data() + static_cast<std::size_t>(ch) * static_cast<std::size_t>(layer.capacity);
                std::fill(row, row + layer.lookback, layer.steady[static_cast<std::size_t>(ch)]);
            }
            layer.write = layer.lookback;
        }
    }
}

void WaveNet::make_room(Layer& layer, int channels, int frames) noexcept SONORA_RT {
    if (layer.write + frames <= layer.capacity) return;
    // capacity = 2 lookback + max_frames and frames <= max_frames, so
    // write > 2 lookback here: source and destination never overlap.
    for (int ch = 0; ch < channels; ++ch) {
        float* row = layer.history.data() + static_cast<std::size_t>(ch) * static_cast<std::size_t>(layer.capacity);
        std::memcpy(row, row + (layer.write - layer.lookback), static_cast<std::size_t>(layer.lookback) * sizeof(float));
    }
    layer.write = layer.lookback;
}

void WaveNet::process(const float* in, float* out, int frames) noexcept SONORA_RT {
    if (arrays_.empty() || max_frames_ == 0) {
        for (int i = 0; i < frames; ++i) out[i] = 0.0f;
        return;
    }
    const DenormalGuard denormals;
    while (frames > 0) {
        const int n = std::min(frames, max_frames_);
        run(in, out, n);
        in += n;
        out += n;
        frames -= n;
    }
}

void WaveNet::run(const float* in, float* out, int n) noexcept SONORA_RT {
    const std::size_t mf = static_cast<std::size_t>(max_frames_);
    float* cond = condition_.data();
    for (int t = 0; t < n; ++t) {
        const float x = in[t];
        // x - x is 0 for every finite x and NaN otherwise (no -ffast-math).
        const float finite = (x - x == 0.0f) ? x : 0.0f;
        cond[t] = std::min(std::max(finite, -kInputLimit), kInputLimit);
    }
    const float** inputs = inputs_.data();
    const auto matmul_rows = kernels_->matmul_rows;

    for (std::size_t ai = 0; ai < arrays_.size(); ++ai) {
        Array& a = arrays_[ai];
        const int c = a.spec.channels;
        const int g = gate_factor(a.spec);
        const int gc = g * c;
        const int k_taps = a.spec.kernel_size;
        for (Layer& layer : a.layers) make_room(layer, c, n);

        // Re-channel into the first layer's history.
        {
            Layer& first = a.layers.front();
            const int in_ch = a.spec.input_size;
            for (int j = 0; j < in_ch; ++j)
                inputs[j] = ai == 0 ? cond : arrays_[ai - 1].layer_out.data() + static_cast<std::size_t>(j) * mf;
            matmul_rows(a.rechannel_w.data(), c, in_ch, inputs, n,
                        first.history.data() + static_cast<std::size_t>(first.write),
                        static_cast<std::size_t>(first.capacity));
        }

        // The head sum starts from the previous array's head output.
        for (int ch = 0; ch < c; ++ch) {
            float* hs = a.head_sum.data() + static_cast<std::size_t>(ch) * mf;
            if (ai == 0) {
                for (int t = 0; t < n; ++t) hs[t] = 0.0f;
            } else {
                const float* prev = arrays_[ai - 1].head_out.data() + static_cast<std::size_t>(ch) * mf;
                for (int t = 0; t < n; ++t) hs[t] = prev[t];
            }
        }

        for (std::size_t li = 0; li < a.layers.size(); ++li) {
            Layer& layer = a.layers[li];
            const std::size_t cap = static_cast<std::size_t>(layer.capacity);
            const float* hist = layer.history.data() + static_cast<std::size_t>(layer.write);

            // Dilated convolution: one product over the K x C tap inputs,
            // tap k reading dilation x (K - 1 - k) frames back; then the
            // bias, then the input mixin.
            for (int k = 0; k < k_taps; ++k) {
                const std::size_t back =
                    static_cast<std::size_t>(layer.dilation) * static_cast<std::size_t>(k_taps - 1 - k);
                for (int j = 0; j < c; ++j) inputs[k * c + j] = hist + static_cast<std::size_t>(j) * cap - back;
            }
            matmul_rows(layer.conv_w.data(), gc, k_taps * c, inputs, n, z_.data(), mf);
            for (int o = 0; o < gc; ++o) {
                float* zr = z_.data() + static_cast<std::size_t>(o) * mf;
                const float b = layer.conv_b[static_cast<std::size_t>(o)];
                const float m = layer.mixin_w[static_cast<std::size_t>(o)];
                for (int t = 0; t < n; ++t) zr[t] = (zr[t] + b) + m * cond[t];
            }

            // Activation; gated: activation(top) x sigmoid(bottom).
            for (int o = 0; o < c; ++o) {
                float* zr = z_.data() + static_cast<std::size_t>(o) * mf;
                if (a.spec.activation == Activation::tanh)
                    kernels_->tanh(zr, n);
                else if (a.spec.activation == Activation::sigmoid)
                    kernels_->sigmoid(zr, n);
                else
                    apply_activation(a.spec.activation, zr, n);
            }
            if (g == 2) {
                for (int o = 0; o < c; ++o) {
                    float* top = z_.data() + static_cast<std::size_t>(o) * mf;
                    float* bottom = z_.data() + static_cast<std::size_t>(o + c) * mf;
                    kernels_->sigmoid(bottom, n);
                    for (int t = 0; t < n; ++t) top[t] *= bottom[t];
                }
            }

            // Into the head sum.
            for (int o = 0; o < c; ++o) {
                float* hs = a.head_sum.data() + static_cast<std::size_t>(o) * mf;
                const float* zr = z_.data() + static_cast<std::size_t>(o) * mf;
                for (int t = 0; t < n; ++t) hs[t] += zr[t];
            }

            // Residual 1x1, its bias, plus the layer's input: into the next
            // layer's history or the array's output.
            for (int j = 0; j < c; ++j) inputs[j] = z_.data() + static_cast<std::size_t>(j) * mf;
            matmul_rows(layer.res_w.data(), c, c, inputs, n, acc_.data(), mf);
            const bool last = li + 1 == a.layers.size();
            Layer* next = last ? nullptr : &a.layers[li + 1];
            for (int o = 0; o < c; ++o) {
                const float* acc = acc_.data() + static_cast<std::size_t>(o) * mf;
                const float b = layer.res_b[static_cast<std::size_t>(o)];
                const float* x = hist + static_cast<std::size_t>(o) * cap;
                float* dst = last ? a.layer_out.data() + static_cast<std::size_t>(o) * mf
                                  : next->history.data() +
                                        static_cast<std::size_t>(o) * static_cast<std::size_t>(next->capacity) +
                                        static_cast<std::size_t>(next->write);
                for (int t = 0; t < n; ++t) dst[t] = x[t] + (acc[t] + b);
            }
        }
        for (Layer& layer : a.layers) layer.write += n;

        // The array's head.
        const int h = a.spec.head_size;
        for (int j = 0; j < c; ++j) inputs[j] = a.head_sum.data() + static_cast<std::size_t>(j) * mf;
        matmul_rows(a.head_w.data(), h, c, inputs, n, a.head_out.data(), mf);
        if (!a.head_b.empty()) {
            for (int o = 0; o < h; ++o) {
                float* dst = a.head_out.data() + static_cast<std::size_t>(o) * mf;
                const float b = a.head_b[static_cast<std::size_t>(o)];
                for (int t = 0; t < n; ++t) dst[t] += b;
            }
        }
    }

    const float* y = arrays_.back().head_out.data();
    for (int t = 0; t < n; ++t) {
        const float v = head_scale_ * y[t];
        out[t] = (v - v == 0.0f) ? v : 0.0f;
    }
}

}  // namespace sonora::dsp::neural
