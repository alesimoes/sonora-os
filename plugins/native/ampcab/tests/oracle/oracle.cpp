// SPDX-License-Identifier: GPL-3.0-or-later
//
// The equivalence test's oracle (ADR-0011 §3.2, §4.3): runs a .nam file
// through the NAM project's MIT reference implementation,
// NeuralAmpModelerCore, and writes what it outputs. Built and run only by
// generate.sh, against a pinned download of NeuralAmpModelerCore and Eigen
// (MPL-2.0); neither is vendored, linked into any Sonora target, or
// shipped. The committed outputs in ../data/ are the oracle's, and
// test_nam.cpp (WaveNet) and test_lstm_equiv.cpp (LSTM) hold our kernels
// to them. Nothing here is per-architecture: get_dsp() reads either, and
// ResetAndPrewarm() runs the LSTM's own warm-up, int(0.5 x the file's rate)
// samples of silence in BLOCK-frame pieces (24 000 at 48 kHz, a whole
// number of 64-frame blocks, which is what Lstm::prepare() runs).
//
//   oracle MODEL.nam INPUT.f32 BLOCK OUTPUT.f32
//   oracle --bench MODEL.nam SECONDS
//
// .f32: raw little-endian IEEE-754 binary32 samples, mono.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

// --bench: the reference's per-block cost at q128 on a tone plus noise,
// the same measurement as ampcab-budget, for Unverified 9's comparison.
// Default build of the reference (Eigen, std::tanh, no A2 fast path).
static int bench(const char* path, double seconds) {
    const int q = 128;
    std::unique_ptr<nam::DSP> model = nam::get_dsp(std::filesystem::path(path));
    model->ResetAndPrewarm(48000.0, q);
    const int blocks = std::max(100, static_cast<int>(seconds * 48000.0 / q));
    std::vector<NAM_SAMPLE> ib(q), ob(q);
    std::vector<double> us(static_cast<std::size_t>(blocks));
    std::uint32_t s = 1;
    double phase = 0.0;
    std::vector<NAM_SAMPLE> sig(static_cast<std::size_t>(blocks + 200) * q);
    for (auto& v : sig) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        phase += 2.0 * 3.14159265358979 * 110.0 / 48000.0;
        v = 0.5 * std::sin(phase) + 0.25 * (static_cast<double>(s) / 2147483648.0 - 1.0);
    }
    std::clock_t cpu0 = 0;
    for (int b = -200; b < blocks; ++b) {
        if (b == 0) cpu0 = std::clock();
        std::copy(sig.begin() + static_cast<std::ptrdiff_t>(b + 200) * q,
                  sig.begin() + static_cast<std::ptrdiff_t>(b + 201) * q, ib.begin());
        NAM_SAMPLE* ip = ib.data();
        NAM_SAMPLE* op = ob.data();
        const auto t0 = std::chrono::steady_clock::now();
        model->process(&ip, &op, q);
        const auto t1 = std::chrono::steady_clock::now();
        if (b >= 0) us[static_cast<std::size_t>(b)] = std::chrono::duration<double, std::micro>(t1 - t0).count();
    }
    const double cpu_us = 1e6 * static_cast<double>(std::clock() - cpu0) / CLOCKS_PER_SEC / blocks;
    std::sort(us.begin(), us.end());
    std::printf("reference %-14s p50 %7.1f us  p99.9 %7.1f us  max %7.1f us  CPU %7.1f us\n",
                std::filesystem::path(path).filename().c_str(), us[us.size() / 2],
                us[std::min(us.size() - 1, static_cast<std::size_t>(us.size() * 0.999))], us.back(), cpu_us);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--bench") return bench(argv[2], std::atof(argv[3]));
    if (argc != 5) {
        std::cerr << "usage: oracle MODEL.nam INPUT.f32 BLOCK OUTPUT.f32\n"
                     "       oracle --bench MODEL.nam SECONDS\n";
        return 2;
    }
    const int block = std::atoi(argv[3]);
    std::ifstream in(argv[2], std::ios::binary);
    std::vector<float> x;
    float v;
    while (in.read(reinterpret_cast<char*>(&v), sizeof v)) x.push_back(v);
    if (x.empty() || block < 1) {
        std::cerr << "oracle: no input or bad block\n";
        return 2;
    }

    std::unique_ptr<nam::DSP> model = nam::get_dsp(std::filesystem::path(argv[1]));
    // The reference's own prewarm: its receptive field of silence.
    model->ResetAndPrewarm(48000.0, block);

    std::vector<NAM_SAMPLE> ib(static_cast<std::size_t>(block)), ob(static_cast<std::size_t>(block));
    std::vector<float> y;
    for (std::size_t at = 0; at < x.size(); at += static_cast<std::size_t>(block)) {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - at));
        for (int i = 0; i < n; ++i) ib[static_cast<std::size_t>(i)] = static_cast<NAM_SAMPLE>(x[at + static_cast<std::size_t>(i)]);
        NAM_SAMPLE* ip = ib.data();
        NAM_SAMPLE* op = ob.data();
        model->process(&ip, &op, n);
        for (int i = 0; i < n; ++i) y.push_back(static_cast<float>(ob[static_cast<std::size_t>(i)]));
    }
    std::ofstream out(argv[4], std::ios::binary);
    out.write(reinterpret_cast<const char*>(y.data()), static_cast<std::streamsize>(y.size() * sizeof(float)));
    return out ? 0 : 1;
}
