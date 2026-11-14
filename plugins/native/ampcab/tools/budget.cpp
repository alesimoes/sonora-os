// SPDX-License-Identifier: GPL-3.0-or-later
//
// The class-E cost of NAM-class captures on this machine (ADR-0011 §3.5,
// Unverified 9; ADR-0111 for the LSTM): per-block time of common/dsp/
// neural's WaveNet at the trainer's four sizes and of its LSTM at the
// trainer's preset, the two test shapes and the largest the kernel takes,
// at q128 and 48 kHz, mono, as the amp processor will run one capture.
// p50, p99.9 and max per block, against class E's 250 us p99.9 and 500 us
// max. The timed block is the amp network followed by the cabinet: a
// 200 ms IR at 48 kHz (9600 taps) in common/dsp's partitioned Convolver,
// quantum-sized partitions (ADR-0011 §4.3, Unverified 9).
//
//   ampcab-budget [SECONDS]   (default 20 s of audio per size)
//
// Not a ctest: a wall-clock figure belongs to the machine it was taken on,
// and is reported with it (the PR, brain/research), measured on a quiet
// machine. The weights are random; the arithmetic per block does not
// depend on them (no branches on values, denormals flushed).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "sonora/dsp/convolver.h"
#include "sonora/dsp/neural/lstm.h"
#include "sonora/dsp/neural/wavenet.h"

namespace {

using namespace sonora::dsp::neural;
using sonora::dsp::ConvolutionKernel;
using sonora::dsp::Convolver;

constexpr int kQ = 128;
constexpr double kFs = 48000.0;
// A 200 ms cabinet IR at 48 kHz, the length class E has to hold.
constexpr std::uint32_t kIrTaps = 9600;

LayerArraySpec arr(int in, int c, int h, std::vector<int> d, bool hb) {
    LayerArraySpec s;
    s.input_size = in;
    s.channels = c;
    s.head_size = h;
    s.kernel_size = 3;
    s.dilations = std::move(d);
    s.activation = Activation::tanh;
    s.head_bias = hb;
    return s;
}

// The trainer's presets (neural-amp-modeler, nam/train/core.py,
// get_wavenet_config): standard 16/8 channels over 10 + 10 layers; lite,
// feather and nano 12/6, 8/4 and 4/2 over 7 + 13.
WaveNetSpec preset(int c) {
    const std::vector<int> s10 = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512};
    const std::vector<int> s7 = {1, 2, 4, 8, 16, 32, 64};
    const std::vector<int> l13 = {128, 256, 512, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512};
    WaveNetSpec w;
    if (c == 16) {
        w.arrays = {arr(1, 16, 8, s10, false), arr(16, 8, 1, s10, true)};
    } else {
        w.arrays = {arr(1, c, c / 2, s7, false), arr(c, c / 2, 1, l13, true)};
    }
    return w;
}

struct Noise {
    std::uint32_t s = 1;
    float operator()() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(static_cast<double>(s) / 2147483648.0 - 1.0);
    }
};

// A guitar-like input, made beforehand: noise at -12 dBFS under a 110 Hz
// tone at -6 dBFS.
std::vector<float> guitar(std::size_t total, Noise& noise) {
    std::vector<float> in(total);
    double phase = 0.0;
    for (float& v : in) {
        phase += 2.0 * 3.14159265358979 * 110.0 / kFs;
        v = static_cast<float>(0.5 * std::sin(phase)) + 0.25f * noise();
    }
    return in;
}

// A decaying noise IR, 60 dB down at its end: the convolver's arithmetic
// does not depend on the taps, but a real cabinet decays, so its tail is
// not all loud.
struct Cabinet {
    ConvolutionKernel kernel;
    Convolver conv;
    bool prepare() {
        std::vector<double> ir(kIrTaps);
        Noise ir_noise;
        ir_noise.s = 7;
        for (std::uint32_t i = 0; i < kIrTaps; ++i) {
            const double decay = std::exp(-6.9078 * static_cast<double>(i) / kIrTaps);
            ir[i] = 0.05 * decay * static_cast<double>(ir_noise());
        }
        const auto q = static_cast<std::uint32_t>(kQ);
        if (!kernel.prepare(q, kIrTaps) || !kernel.set(ir.data(), kIrTaps)) return false;
        return conv.prepare(q, kIrTaps) && conv.accepts(kernel);
    }
};

// Times `blocks` q128 blocks after 200 untimed ones and prints one row. The
// timed work is the network and then the cabinet's convolution.
template <class Net>
void time_blocks(Net& net, Cabinet& cab, const char* name, std::size_t weights, int blocks, Noise& noise) {
    const std::vector<float> in = guitar(static_cast<std::size_t>(blocks + 200) * kQ, noise);
    std::vector<float> mid(static_cast<std::size_t>(kQ));
    std::vector<float> out(static_cast<std::size_t>(kQ));
    cab.conv.reset();
    std::vector<double> us(static_cast<std::size_t>(blocks));
    std::clock_t cpu0 = 0;
    for (int b = -200; b < blocks; ++b) {
        if (b == 0) cpu0 = std::clock();
        const float* x = in.data() + static_cast<std::size_t>(b + 200) * static_cast<std::size_t>(kQ);
        const auto t0 = std::chrono::steady_clock::now();
        net.process(x, mid.data(), kQ);
        cab.conv.process(cab.kernel, mid.data(), out.data(), static_cast<std::uint32_t>(kQ));
        const auto t1 = std::chrono::steady_clock::now();
        if (b >= 0) us[static_cast<std::size_t>(b)] = std::chrono::duration<double, std::micro>(t1 - t0).count();
    }
    // Process CPU time per block: leaves out the time the thread was
    // preempted, so on a loaded machine it is the kernel's cost where the
    // wall-clock tail is the scheduler's.
    const double cpu_us = 1e6 * static_cast<double>(std::clock() - cpu0) / CLOCKS_PER_SEC / blocks;
    std::sort(us.begin(), us.end());
    const double p50 = us[us.size() / 2];
    const double p999 = us[std::min(us.size() - 1, static_cast<std::size_t>(us.size() * 0.999))];
    std::printf("%-9s %-8s %8zu %8.1f %10.1f %8.1f %8.1f%% %8.1f\n", name, net.kernels_name(), weights, p50, p999,
                us.back(), 100.0 * p50 / (kQ / kFs * 1e6), cpu_us);
}

void header(const char* what, int blocks) {
    std::printf("%s per block at q%d, %.0f Hz, mono; %d blocks (%.0f s) each\n", what, kQ, kFs, blocks,
                blocks * kQ / kFs);
    std::printf("%-9s %-8s %8s %8s %10s %8s %9s %8s\n", "size", "kernels", "weights", "p50 us", "p99.9 us", "max us",
                "RT load", "CPU us");
}

// Each size with the generic kernels, then with the SIMD table when this
// processor has one (allow_simd): the same bits, so the difference is the
// table's alone.
int wavenets(int blocks, Noise& noise) {
    struct Size {
        const char* name;
        int channels;
    };
    const Size sizes[] = {{"nano", 4}, {"feather", 8}, {"lite", 12}, {"standard", 16}};
    header("WaveNet", blocks);
    Cabinet cab;
    if (!cab.prepare()) {
        std::fprintf(stderr, "cabinet IR: kernel refused\n");
        return 1;
    }
    for (int pass = 0; pass < 2 * 4; ++pass) {
        const Size& sz = sizes[pass / 2];
        const bool simd = pass % 2 == 1;
        const WaveNetSpec spec = preset(sz.channels);
        std::vector<float> w(wavenet_weight_count(spec));
        const float scale = 1.5f / std::sqrt(static_cast<float>(sz.channels * 3));
        for (float& v : w) v = noise() * scale;
        w.back() = 0.02f;
        WaveNet net;
        net.allow_simd(simd);
        std::string err;
        if (!net.load(spec, w, &err)) {
            std::fprintf(stderr, "%s: %s\n", sz.name, err.c_str());
            return 1;
        }
        if (simd && std::string(net.kernels_name()) == "generic") continue;
        net.prepare(kQ);
        time_blocks(net, cab, sz.name, w.size(), blocks, noise);
    }
    return 0;
}

// The trainer's LSTM preset (nam/train/core.py: 1 x 24), the equivalence
// test's shapes, a deeper and a wider one, and the largest lstm_check
// takes at either cap (4 x 64, 116 545 weights; 1 x 128; 2 x 128 is over
// the weight cap).
int lstms(int blocks, Noise& noise) {
    struct Size {
        const char* name;
        int layers, hidden;
    };
    const Size sizes[] = {{"1x16", 1, 16},  {"2x8", 2, 8},   {"1x24", 1, 24},
                          {"2x32", 2, 32},  {"4x64", 4, 64}, {"1x128", 1, 128}};
    header("LSTM", blocks);
    Cabinet cab;
    if (!cab.prepare()) {
        std::fprintf(stderr, "cabinet IR: kernel refused\n");
        return 1;
    }
    for (int pass = 0; pass < 2 * 6; ++pass) {
        const Size& sz = sizes[pass / 2];
        const bool simd = pass % 2 == 1;
        LstmSpec spec;
        spec.num_layers = sz.layers;
        spec.hidden_size = sz.hidden;
        std::vector<float> w(lstm_weight_count(spec));
        const float scale = 1.5f / std::sqrt(static_cast<float>(2 * sz.hidden));
        for (float& v : w) v = noise() * scale;
        Lstm net;
        net.allow_simd(simd);
        std::string err;
        if (!net.load(spec, w, &err)) {
            std::fprintf(stderr, "%s: %s\n", sz.name, err.c_str());
            return 1;
        }
        if (simd && std::string(net.kernels_name()) == "generic") continue;
        net.prepare(kQ, kFs);
        time_blocks(net, cab, sz.name, w.size(), blocks, noise);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 20.0;
    const int blocks = std::max(100, static_cast<int>(seconds * kFs / kQ));
    Noise noise;
    if (wavenets(blocks, noise) != 0 || lstms(blocks, noise) != 0) return 1;
    std::printf("class E: p99.9 <= 250 us, max <= 500 us, with the cabinet IR included\n");
    return 0;
}
