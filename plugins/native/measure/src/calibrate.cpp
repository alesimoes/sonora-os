// SPDX-License-Identifier: GPL-3.0-or-later
//
// The reference kernel: a fixed workload whose cost per block calibrates a
// machine against the reference machine of ADR-0011 §3.5, so a CPU bar can
// gate on a shared CI runner of unknown speed (brain/research/
// native-tube-n2.md, "The gate"). The nightly runs a processor's CPU check
// with --cpu-calibrate and holds it to
//     class bar x (this kernel's p50 here / kReferenceKernelUs),
// which catches a regression of the processor and not the runner's speed.
//
// FROZEN. The kernel stands for the machine, so it must not change with
// the DSP: it uses nothing from common/dsp, and any edit to it, to the
// compiler flags it is built with, or to the way it is timed invalidates
// kReferenceKernelUs, which must then be measured again on the reference
// machine in the same commit.
//
// What it does, per q128 stereo block: what an eco (2x) tube stage does in
// kind, on each of two channels and 256 oversampled samples, one serial
// chain per sample: a one-pole input filter, three Newton passes on a
// diode clipper with a capacitor (one exp, one division each, a fixed
// count so the cost does not depend on the data), and a biquad. A chain of
// dependent operations with transcendental calls and divisions on it, the
// shape of work perf found in the triode solve, so the ratio between the
// two moves little from one microarchitecture to another.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "sonora/measure/measure.h"

namespace sonora::measure {

namespace {

// The reference kernel's p50 per block, per-block minimum of 5 runs, on
// the reference machine (Intel Core i7-11800H, Docker Desktop on Windows
// 11, the sonora-build:trixie image, GCC 14, Release), quiet host:
// 32.58-32.60 us over three runs (--cpu-reference-kernel), 2026-10-14.
constexpr double kReferenceKernelUs = 32.6;

struct KernelState {
    double lp = 0.0, v = 0.0, z1 = 0.0, z2 = 0.0;
};

// One block of one channel: n oversampled samples.
double kernel_channel(KernelState& s, const double* in, std::size_t n) {
    // A 2x rate at 48 kHz; a 1N4148-like clipper behind 2.2 kOhm and 10 nF.
    constexpr double fs = 96000.0, r = 2.2e3, c = 10e-9, is = 2.52e-9, vt = 0.02585 * 1.752;
    constexpr double gc = 2.0 * c * fs;  // trapezoidal capacitor
    constexpr double a = 0.25;           // the one-pole input filter
    // A 12 kHz biquad low-pass at 96 kHz (RBJ cookbook, Q = 0.707).
    constexpr double b0 = 0.0995, b1 = 0.1990, b2 = 0.0995, a1 = -0.9465, a2 = 0.3445;
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        s.lp += a * (in[i] - s.lp);
        const double hist = gc * s.v + (s.lp - s.v) / r;  // a crude history term
        double v = s.v;
        for (int pass = 0; pass < 3; ++pass) {
            const double e = std::exp(v / vt);
            const double f = (s.lp - v) / r - is * (e - 1.0) - gc * v + hist;
            const double df = -1.0 / r - is * e / vt - gc;
            // Bounded, so no pass can overflow exp whatever the state.
            v = std::min(1.0, std::max(-5.0, v - f / df));
        }
        s.v = v;
        const double y = b0 * v + s.z1;
        s.z1 = b1 * v - a1 * y + s.z2;
        s.z2 = b2 * v - a2 * y;
        acc += y;
    }
    return acc;
}

}  // namespace

double reference_kernel_reference_us() { return kReferenceKernelUs; }

double reference_kernel_us(double seconds) {
    constexpr std::size_t kSamples = 256;  // 128 frames at 2x
    const auto blocks = static_cast<std::size_t>(std::max(1.0, seconds) * 48000.0 / 128.0);
    // A 997 Hz tone at 2 V peak (into grid-current territory for the
    // diode), one period of samples cycled through: a fixed input.
    std::vector<double> tone(kSamples * 4);
    for (std::size_t i = 0; i < tone.size(); ++i) tone[i] = 2.0 * std::sin(2.0 * 3.14159265358979 * 997.0 * i / 96000.0);
    std::vector<double> best(blocks, 1e30);
    volatile double sink = 0.0;
    for (int run = 0; run < 5; ++run) {
        KernelState l, r;
        for (std::size_t b = 0; b < blocks; ++b) {
            const double* x = &tone[(b % 4) * kSamples];
            const auto t0 = std::chrono::steady_clock::now();
            const double y = kernel_channel(l, x, kSamples) + kernel_channel(r, x, kSamples);
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            sink = sink + y;
            best[b] = std::min(best[b], us);
        }
    }
    // The first second warms caches and branch predictors, as in
    // check_cpu_on.
    best.erase(best.begin(), best.begin() + static_cast<long>(std::min<std::size_t>(blocks / 2, 375)));
    std::sort(best.begin(), best.end());
    return best[best.size() / 2];
}

}  // namespace sonora::measure
