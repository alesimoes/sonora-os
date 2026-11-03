// SPDX-License-Identifier: GPL-3.0-or-later
//
// The class-E cost of NAM-class captures on this machine (ADR-0011 §3.5,
// Unverified 9): per-block time of common/dsp/neural's WaveNet at the
// trainer's four sizes, at q128 and 48 kHz, mono, as the amp processor
// will run one capture. p50, p99.9 and max per block, against class E's
// 250 us p99.9 and 500 us max, which also has to hold the cabinet IR.
//
//   ampcab-budget [SECONDS]   (default 20 s of audio per size)
//
// Not a ctest: a wall-clock figure belongs to the machine it was taken on,
// and is reported with it (the PR, brain/research). The weights are
// random; the arithmetic per block does not depend on them (no branches
// on values, denormals flushed).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

#include "sonora/dsp/neural/wavenet.h"

namespace {

using namespace sonora::dsp::neural;

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

}  // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 20.0;
    const int q = 128;
    const double fs = 48000.0;
    const int blocks = std::max(100, static_cast<int>(seconds * fs / q));
    struct Size {
        const char* name;
        int channels;
    };
    const Size sizes[] = {{"nano", 4}, {"feather", 8}, {"lite", 12}, {"standard", 16}};
    std::printf("WaveNet per block at q%d, %.0f Hz, mono; %d blocks (%.0f s) each\n", q, fs, blocks,
                blocks * q / fs);
    std::printf("%-9s %8s %8s %10s %8s %9s %8s\n", "size", "weights", "p50 us", "p99.9 us", "max us", "RT load",
                "CPU us");
    std::uint32_t s = 1;
    auto noise = [&s]() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(static_cast<double>(s) / 2147483648.0 - 1.0);
    };
    for (const Size& sz : sizes) {
        const WaveNetSpec spec = preset(sz.channels);
        std::vector<float> w(wavenet_weight_count(spec));
        const float scale = 1.5f / std::sqrt(static_cast<float>(sz.channels * 3));
        for (float& v : w) v = noise() * scale;
        w.back() = 0.02f;
        WaveNet net;
        std::string err;
        if (!net.load(spec, w, &err)) {
            std::fprintf(stderr, "%s: %s\n", sz.name, err.c_str());
            return 1;
        }
        net.prepare(q);
        // A guitar-like input, made beforehand: noise at -12 dBFS under a
        // 110 Hz tone at -6 dBFS.
        const std::size_t total = static_cast<std::size_t>(blocks + 200) * static_cast<std::size_t>(q);
        std::vector<float> in(total), out(static_cast<std::size_t>(q));
        double phase = 0.0;
        for (float& v : in) {
            phase += 2.0 * 3.14159265358979 * 110.0 / fs;
            v = static_cast<float>(0.5 * std::sin(phase)) + 0.25f * noise();
        }
        std::vector<double> us(static_cast<std::size_t>(blocks));
        std::clock_t cpu0 = 0;
        for (int b = -200; b < blocks; ++b) {
            if (b == 0) cpu0 = std::clock();
            const float* x = in.data() + static_cast<std::size_t>(b + 200) * static_cast<std::size_t>(q);
            const auto t0 = std::chrono::steady_clock::now();
            net.process(x, out.data(), q);
            const auto t1 = std::chrono::steady_clock::now();
            if (b >= 0) us[static_cast<std::size_t>(b)] = std::chrono::duration<double, std::micro>(t1 - t0).count();
        }
        // Process CPU time per block: leaves out the time the thread was
        // preempted, so on a loaded machine it is the kernel's cost where
        // the wall-clock tail is the scheduler's.
        const double cpu_us = 1e6 * static_cast<double>(std::clock() - cpu0) / CLOCKS_PER_SEC / blocks;
        std::sort(us.begin(), us.end());
        const double p50 = us[us.size() / 2];
        const double p999 = us[std::min(us.size() - 1, static_cast<std::size_t>(us.size() * 0.999))];
        const double mx = us.back();
        std::printf("%-9s %8zu %8.1f %10.1f %8.1f %8.1f%% %8.1f\n", sz.name, w.size(), p50, p999, mx,
                    100.0 * p50 / (q / fs * 1e6), cpu_us);
    }
    std::printf("class E: p99.9 <= 250 us, max <= 500 us, with the cabinet IR included\n");
    return 0;
}
