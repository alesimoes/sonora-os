// SPDX-License-Identifier: GPL-3.0-or-later
//
// The zero-latency partitioned convolver (convolver.h) against a direct
// convolution in double, at every partition size the cabinet processor may
// use and at host block sizes that do and do not divide the partition.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <complex>
#include <cstdio>
#include <limits>
#include <vector>

#include "../src/convolver_kernels.h"
#include "check.h"
#include "sonora/dsp/convolver.h"
#include "sonora/dsp/noise.h"

namespace {

using namespace sonora::dsp;

std::vector<double> random_ir(std::uint32_t taps, std::uint64_t seed) {
    // A decaying noise burst, the shape of a cabinet or room response.
    GaussianNoise g(seed);
    std::vector<double> h(taps);
    for (std::uint32_t i = 0; i < taps; ++i) {
        h[i] = 0.3 * g.next() * std::exp(-4.0 * i / taps);
    }
    return h;
}

std::vector<float> random_input(std::uint32_t n, std::uint64_t seed) {
    GaussianNoise g(seed);
    std::vector<float> x(n);
    for (auto& v : x) v = static_cast<float>(0.25 * g.next());
    return x;
}

std::vector<double> direct(const std::vector<float>& x, const std::vector<double>& h) {
    std::vector<double> y(x.size(), 0.0);
    for (std::size_t n = 0; n < x.size(); ++n) {
        double acc = 0.0;
        const std::size_t m = std::min(h.size(), n + 1);
        for (std::size_t i = 0; i < m; ++i) acc += h[i] * static_cast<double>(x[n - i]);
        y[n] = acc;
    }
    return y;
}

// Runs x through the convolver in blocks cycling through `blocks`.
std::vector<float> run(Convolver& c, const ConvolutionKernel& k, const std::vector<float>& x,
                       const std::vector<std::uint32_t>& blocks) {
    std::vector<float> y(x.size());
    std::size_t at = 0, b = 0;
    while (at < x.size()) {
        const std::uint32_t n =
            static_cast<std::uint32_t>(std::min<std::size_t>(blocks[b++ % blocks.size()], x.size() - at));
        c.process(k, x.data() + at, y.data() + at, n);
        at += n;
    }
    return y;
}

// Worst |out - reference| over the reference's peak, in dB.
double error_db(const std::vector<float>& y, const std::vector<double>& ref, std::size_t from = 0) {
    double err = 0.0, peak = 0.0;
    for (std::size_t n = from; n < ref.size(); ++n) {
        err = std::max(err, std::fabs(static_cast<double>(y[n]) - ref[n]));
        peak = std::max(peak, std::fabs(ref[n]));
    }
    return 20.0 * std::log10(std::max(err, 1e-300) / peak);
}

// RealFft against the complex transform it replaces, and its inverse back
// to the input, at every size from 4 to 8192.
int test_real_fft() {
    double worst = 0.0;
    for (std::uint32_t n = 4; n <= 8192; n *= 2) {
        GaussianNoise g(n);
        std::vector<double> x(n), back(n);
        std::vector<std::complex<double>> ref(n), X(n / 2 + 1);
        for (std::uint32_t i = 0; i < n; ++i) {
            x[i] = g.next();
            ref[i] = {x[i], 0.0};
        }
        Fft f;
        RealFft r;
        CHECK(f.prepare(n) && r.prepare(n));
        f.forward(ref.data());
        r.forward(x.data(), X.data());
        for (std::uint32_t k = 0; k <= n / 2; ++k) worst = std::max(worst, std::abs(X[k] - ref[k]) / n);
        r.inverse(X.data(), back.data());
        for (std::uint32_t i = 0; i < n; ++i) worst = std::max(worst, std::fabs(back[i] - x[i]));
    }
    std::printf("  real fft: worst error %.2e (per point, against the complex transform)\n", worst);
    CHECK_LE(worst, 1e-13, "real transform error");
    RealFft bad;
    CHECK(!bad.prepare(2) && !bad.prepare(96));
    return 0;
}

// The architecture's kernels give the bits of the scalar expressions they
// stand for, so the x86-64 and the generic build convolve identically
// (this binary is built against each: test_dsp_convolver and
// test_dsp_convolver_generic).
int test_kernels_bit_exact() {
    const std::uint32_t bins = 65;
    GaussianNoise g(77);
    std::vector<double> v(10 * bins);
    for (auto& e : v) e = g.next();
    const double* x0r = &v[2 * bins];
    const double* x0i = &v[3 * bins];
    const double* h0r = &v[4 * bins];
    const double* h0i = &v[5 * bins];
    const double* x1r = &v[6 * bins];
    const double* x1i = &v[7 * bins];
    const double* h1r = &v[8 * bins];
    const double* h1i = &v[9 * bins];
    std::vector<double> ar(v.begin(), v.begin() + bins), ai(v.begin() + bins, v.begin() + 2 * bins);
    std::vector<double> er = ar, ei = ai;
    spectral_mac2(ar.data(), ai.data(), x0r, x0i, h0r, h0i, x1r, x1i, h1r, h1i, bins);
    spectral_mac(ar.data(), ai.data(), x1r, x1i, h0r, h0i, bins);
    for (std::uint32_t k = 0; k < bins; ++k) {
        er[k] = er[k] + (x0r[k] * h0r[k] - x0i[k] * h0i[k]);
        ei[k] = ei[k] + (x0r[k] * h0i[k] + x0i[k] * h0r[k]);
        er[k] = er[k] + (x1r[k] * h1r[k] - x1i[k] * h1i[k]);
        ei[k] = ei[k] + (x1r[k] * h1i[k] + x1i[k] * h1r[k]);
        er[k] = er[k] + (x1r[k] * h0r[k] - x1i[k] * h0i[k]);
        ei[k] = ei[k] + (x1r[k] * h0i[k] + x1i[k] * h0r[k]);
        CHECK(ar[k] == er[k] && ai[k] == ei[k]);
    }
    for (std::uint32_t n : {16u, 64u, 128u}) {
        double s[4] = {0.0, 0.0, 0.0, 0.0};
        for (std::uint32_t j = 0; j < n; ++j) s[j % 4] += v[j] * v[300 + j];
        CHECK(head_dot(v.data(), v.data() + 300, n) == (s[0] + s[1]) + (s[2] + s[3]));
    }
    return 0;
}

// Every partition size, response lengths on and around partition edges,
// block sizes of 1, odd, equal to and longer than the partition. The bar,
// -120 dB of the peak, is ADR-0011 §3.4's golden tolerance; what remains is
// the float output's own rounding (about -140 dB).
int test_matches_direct() {
    const std::uint32_t len = 6000;
    const auto x = random_input(len, 11);
    double worst = -400.0;
    for (std::uint32_t p : {16u, 64u, 128u, 512u}) {
        for (std::uint32_t taps : {1u, p - 1, p, p + 1, 3 * p + 5, 4000u}) {
            const auto h = random_ir(taps, 100 + taps);
            const auto ref = direct(x, h);
            ConvolutionKernel k;
            CHECK(k.prepare(p, 4000));
            CHECK(k.set(h.data(), taps));
            for (const auto& blocks : std::vector<std::vector<std::uint32_t>>{
                     {1}, {7, 300, 13}, {p}, {128}, {1000}}) {
                Convolver c;
                CHECK(c.prepare(p, 4000));
                CHECK(c.accepts(k));
                const auto y = run(c, k, x, blocks);
                const double e = error_db(y, ref);
                worst = std::max(worst, e);
                if (e > -120.0) std::fprintf(stderr, "  P %u taps %u block %u: %.1f dB\n", p, taps, blocks[0], e);
                CHECK_LE(e, -120.0, "error against direct convolution (dB of peak)");
            }
        }
    }
    std::printf("  against direct convolution: worst error %.1f dB of peak\n", worst);
    return 0;
}

// Zero latency, measured: the response to an impulse is the IR from the
// first sample, and a delayed impulse response peaks where it should, at
// every block size. latency() says 0, so PDC has nothing to compensate.
int test_latency() {
    CHECK(Convolver::latency() == 0);
    for (std::uint32_t p : {16u, 128u}) {
        for (std::uint32_t d : {0u, 1u, p - 1, p, 5 * p + 3}) {
            std::vector<double> h(d + 1, 0.0);
            h[d] = 1.0;
            ConvolutionKernel k;
            CHECK(k.prepare(p, 2048));
            CHECK(k.set(h.data(), d + 1));
            for (std::uint32_t block : {1u, 37u, 128u, 256u}) {
                Convolver c;
                CHECK(c.prepare(p, 2048));
                std::vector<float> x(4 * 2048, 0.0f);
                x[0] = 1.0f;
                const auto y = run(c, k, x, {block});
                std::size_t at = 0;
                for (std::size_t n = 0; n < y.size(); ++n) {
                    if (std::fabs(y[n]) > std::fabs(y[at])) at = n;
                }
                CHECK(at == d);
                CHECK_LE(std::fabs(y[d] - 1.0f), 1e-6, "impulse peak - 1");
            }
        }
    }
    std::printf("  latency: measured 0 at every block size and partition, reported 0\n");
    return 0;
}

// The state does not depend on the response: a kernel swapped mid-stream
// is exact from the next block boundary, at most P samples later.
int test_kernel_swap() {
    const std::uint32_t p = 64, len = 8000, swap_at = 3001;
    const auto x = random_input(len, 5);
    const auto ha = random_ir(1500, 1), hb = random_ir(2000, 2);
    ConvolutionKernel a, b;
    CHECK(a.prepare(p, 2000) && b.prepare(p, 2000));
    CHECK(a.set(ha.data(), 1500) && b.set(hb.data(), 2000));
    Convolver c;
    CHECK(c.prepare(p, 2000));
    std::vector<float> y(len);
    c.process(a, x.data(), y.data(), swap_at);
    c.process(b, x.data() + swap_at, y.data() + swap_at, len - swap_at);
    const auto ref_b = direct(x, hb);
    const double e = error_db(y, ref_b, swap_at + p);
    std::printf("  kernel swap: exact %u samples after the swap, error %.1f dB\n", p, e);
    CHECK_LE(e, -120.0, "error after a swap (dB of peak)");
    // A response that fits in the head, then a long one: the FDL kept the
    // history meanwhile.
    ConvolutionKernel shortk;
    CHECK(shortk.prepare(p, 2000));
    const double one = 0.5;
    CHECK(shortk.set(&one, 1) && shortk.tail_partitions() == 0);
    c.reset();
    c.process(shortk, x.data(), y.data(), swap_at);
    c.process(b, x.data() + swap_at, y.data() + swap_at, len - swap_at);
    CHECK_LE(error_db(y, ref_b, swap_at + p), -120.0, "error after a swap from a head-only kernel");
    return 0;
}

// NaN and Inf inputs are read as 0; a loud input through gain stays in the
// float range. The output is finite whatever comes in.
int test_non_finite() {
    const std::uint32_t p = 32, len = 2000;
    auto x = random_input(len, 9);
    const auto h = random_ir(500, 3);
    ConvolutionKernel k;
    CHECK(k.prepare(p, 500) && k.set(h.data(), 500));
    std::vector<float> clean = x;
    for (std::size_t n : {10u, 400u, 401u, 1500u}) {
        x[n] = n % 2 ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
        clean[n] = 0.0f;
    }
    x[700] = -std::numeric_limits<float>::infinity();
    clean[700] = 0.0f;
    Convolver c;
    CHECK(c.prepare(p, 500));
    const auto y = run(c, k, x, {128});
    for (float v : y) CHECK(std::isfinite(v));
    CHECK_LE(error_db(y, direct(clean, h)), -120.0, "non-finite read as zero");

    std::vector<double> loud(64, 1.0);
    ConvolutionKernel g;
    CHECK(g.prepare(p, 64) && g.set(loud.data(), 64));
    std::vector<float> big(256, std::numeric_limits<float>::max());
    c.reset();
    const auto yb = run(c, g, big, {128});
    for (float v : yb) CHECK(std::isfinite(v));
    std::printf("  non-finite input: output finite, NaN/Inf read as 0\n");
    return 0;
}

// set() refuses what it cannot hold and leaves the kernel as it was; a
// convolver given a kernel of another partition size is silent.
int test_refusals() {
    ConvolutionKernel k;
    CHECK(!k.prepare(24, 100));
    CHECK(!k.prepare(8, 100));
    CHECK(k.prepare(32, 100));
    CHECK(k.taps() == 1);  // a unit impulse after prepare
    const auto h = random_ir(100, 4);
    CHECK(k.set(h.data(), 100));
    std::vector<double> bad(101, 0.1);
    CHECK(!k.set(bad.data(), 101));
    CHECK(!k.set(bad.data(), 0));
    bad[50] = std::numeric_limits<double>::quiet_NaN();
    CHECK(!k.set(bad.data(), 60));
    CHECK(k.taps() == 100);

    Convolver other;
    CHECK(other.prepare(64, 100));
    CHECK(!other.accepts(k));
    std::vector<float> x(256, 0.5f), y(256, 1.0f);
    other.process(k, x.data(), y.data(), 256);
    for (float v : y) CHECK(v == 0.0f);
    Convolver small;
    CHECK(small.prepare(32, 40));
    CHECK(!small.accepts(k));
    return 0;
}

// After the input falls silent the output is exactly zero once the response
// has passed: no residue walks down through the subnormals.
int test_silence_is_exact() {
    const std::uint32_t p = 64, taps = 1000;
    const auto h = random_ir(taps, 6);
    ConvolutionKernel k;
    CHECK(k.prepare(p, taps) && k.set(h.data(), taps));
    Convolver c;
    CHECK(c.prepare(p, taps));
    auto x = random_input(3000, 8);
    std::fill(x.begin() + 1000, x.end(), 0.0f);
    const auto y = run(c, k, x, {128});
    // The tail block's transform sees the last input up to one block after
    // it, so the last nonzero output is at most taps + P samples on.
    std::size_t last = 0;
    for (std::size_t n = 0; n < y.size(); ++n) {
        if (y[n] != 0.0f) last = n;
    }
    std::printf("  silence: last nonzero output %zu samples after the input stopped (IR %u, P %u)\n",
                last - 999, taps, p);
    CHECK(last < 1000 + taps + p);
    return 0;
}

// A stereo cabinet: two convolvers, a 200 ms response at 48 kHz, 128-frame
// blocks, as ADR-0011 §3.5 measures, at three partition sizes. Reported,
// not held: the class-E figure (amp and cabinet together, 250 us p99.9) is
// the cabinet processor's, measured by sonora-dsp-measure's calibrated CPU
// gate on the reference machine; a wall-clock bar in a unit test on a
// shared runner is a flake, not a gate. The minimum is the load-free cost;
// with P equal to the quantum every call does the same work, and with P
// larger the transform falls on one call in P / 128.
int test_cpu() {
    const std::uint32_t rate = 48000, taps = rate / 5, q = 128;
    const double seconds = dsp_quick() ? 10.0 : 60.0;
    const auto hl = random_ir(taps, 21), hr = random_ir(taps, 22);
    for (std::uint32_t p : {64u, 128u, 256u}) {
        ConvolutionKernel kl, kr;
        CHECK(kl.prepare(p, taps) && kr.prepare(p, taps));
        CHECK(kl.set(hl.data(), taps) && kr.set(hr.data(), taps));
        Convolver cl, cr;
        CHECK(cl.prepare(p, taps) && cr.prepare(p, taps));
        const auto x = random_input(q * 64, 13);
        std::vector<float> yl(q), yr(q);
        const std::uint32_t blocks = static_cast<std::uint32_t>(seconds * rate / q);
        std::vector<double> us(blocks);
        for (std::uint32_t b = 0; b < blocks; ++b) {
            const float* in = x.data() + (b % 64) * q;
            const auto t0 = std::chrono::steady_clock::now();
            cl.process(kl, in, yl.data(), q);
            cr.process(kr, in, yr.data(), q);
            const auto t1 = std::chrono::steady_clock::now();
            us[b] = std::chrono::duration<double, std::micro>(t1 - t0).count();
        }
        std::sort(us.begin(), us.end());
        const double p50 = us[blocks / 2], p999 = us[static_cast<std::size_t>(blocks * 0.999)],
                     mx = us.back();
        std::printf("  cpu: stereo, IR 200 ms at 48 kHz, q128, P %u, %.0f s: min %.1f us, median %.1f us, "
                    "p99.9 %.1f us, max %.1f us\n",
                    p, seconds, us[0], p50, p999, mx);
    }
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_real_fft();
    failed += test_kernels_bit_exact();
    failed += test_matches_direct();
    failed += test_latency();
    failed += test_kernel_swap();
    failed += test_non_finite();
    failed += test_refusals();
    failed += test_silence_is_exact();
    failed += test_cpu();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp convolver: all passed\n");
    return 0;
}
