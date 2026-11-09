// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LSTM kernel's behaviour (ADR-0111): against a plain double-precision
// LSTM written from the equations, reset determinism, block-size
// independence, silence, non-finite and huge input, no allocation in
// process(), and the generic and AVX2 products to the same bits. The
// equivalence with the NAM reference's own output is
// plugins/native/ampcab/tests/test_lstm_equiv.cpp.
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "../src/neural/kernels.h"
#include "../src/neural/lstm_kernels.h"
#include "check.h"
#include "sonora/dsp/neural/lstm.h"

namespace {
std::atomic<bool> g_counting{false};
std::atomic<long> g_allocations{0};
}  // namespace

void* operator new(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed)) g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace sonora::dsp::neural;

// xorshift32 in [-1, 1).
struct Noise {
    std::uint32_t s;
    explicit Noise(std::uint32_t seed) : s(seed) {}
    float next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return static_cast<float>(static_cast<double>(s) / 2147483648.0 - 1.0);
    }
};

LstmSpec spec(int layers, int hidden) {
    LstmSpec s;
    s.num_layers = layers;
    s.hidden_size = hidden;
    return s;
}

// Weights in the format's order at the scales a trained net has: W about
// 1.5 / sqrt(fan-in), so the gates move through their whole range.
std::vector<float> weights_for(const LstmSpec& s, std::uint32_t seed) {
    Noise n(seed);
    std::vector<float> w;
    const int h = s.hidden_size;
    for (int l = 0; l < s.num_layers; ++l) {
        const int nin = (l == 0 ? 1 : h) + h;
        const float sw = 1.5f / std::sqrt(static_cast<float>(nin));
        for (int i = 0; i < 4 * h * nin; ++i) w.push_back(sw * n.next());
        for (int i = 0; i < 4 * h; ++i) w.push_back(0.4f * n.next());
        for (int i = 0; i < 2 * h; ++i) w.push_back(0.3f * n.next());  // h0, c0
    }
    for (int i = 0; i < h; ++i) w.push_back(n.next() / std::sqrt(static_cast<float>(h)));
    w.push_back(0.05f);
    return w;
}

// Silence, a quiet then a hot tone, noise, a step: every gate regime.
std::vector<float> signal(std::size_t n) {
    Noise noise(99);
    std::vector<float> x(n);
    double phase = 0.0;
    for (std::size_t t = 0; t < n; ++t) {
        phase += 2.0 * 3.14159265358979 * (110.0 + 2.0 * static_cast<double>(t % 4000)) / 48000.0;
        if (t < 200)
            x[t] = 0.0f;
        else if (t < n / 3)
            x[t] = static_cast<float>(0.02 * std::sin(phase));
        else if (t < 2 * n / 3)
            x[t] = static_cast<float>(1.5 * std::sin(phase));
        else if (t < 5 * n / 6)
            x[t] = 0.5f * noise.next();
        else
            x[t] = 0.8f;
    }
    return x;
}

// The equations of lstm.h in double, with std::tanh and the exact sigmoid,
// and the reference's prewarm: an oracle independent of our activations
// and summation order.
struct NaiveLstm {
    int layers, h;
    std::vector<std::vector<double>> w, b, hs, cs;
    std::vector<double> head;
    double head_b;

    NaiveLstm(const LstmSpec& s, const std::vector<float>& wf, int warm) : layers(s.num_layers), h(s.hidden_size) {
        std::size_t at = 0;
        auto take = [&](std::size_t n) {
            std::vector<double> v(wf.begin() + static_cast<std::ptrdiff_t>(at),
                                  wf.begin() + static_cast<std::ptrdiff_t>(at + n));
            at += n;
            return v;
        };
        const std::size_t hh = static_cast<std::size_t>(h);
        for (int l = 0; l < layers; ++l) {
            const std::size_t nin = (l == 0 ? 1 : hh) + hh;
            w.push_back(take(4 * hh * nin));
            b.push_back(take(4 * hh));
            hs.push_back(take(hh));
            cs.push_back(take(hh));
        }
        head = take(hh);
        head_b = wf[at];
        for (int t = 0; t < warm; ++t) step(0.0);
    }

    static double sig(double v) { return 1.0 / (1.0 + std::exp(-v)); }

    double step(double x) {
        std::vector<double> in(1, x);
        for (int l = 0; l < layers; ++l) {
            std::vector<double> xh = in;
            xh.insert(xh.end(), hs[static_cast<std::size_t>(l)].begin(), hs[static_cast<std::size_t>(l)].end());
            const std::size_t nin = xh.size();
            std::vector<double> z(static_cast<std::size_t>(4 * h));
            for (std::size_t r = 0; r < z.size(); ++r) {
                double s = b[static_cast<std::size_t>(l)][r];
                for (std::size_t i = 0; i < nin; ++i) s += w[static_cast<std::size_t>(l)][r * nin + i] * xh[i];
                z[r] = s;
            }
            auto& c = cs[static_cast<std::size_t>(l)];
            auto& hv = hs[static_cast<std::size_t>(l)];
            for (int k = 0; k < h; ++k) {
                const std::size_t kk = static_cast<std::size_t>(k), hh = static_cast<std::size_t>(h);
                c[kk] = sig(z[hh + kk]) * c[kk] + sig(z[kk]) * std::tanh(z[2 * hh + kk]);
                hv[kk] = sig(z[3 * hh + kk]) * std::tanh(c[kk]);
            }
            in = hv;
        }
        double y = head_b;
        for (int k = 0; k < h; ++k) y += head[static_cast<std::size_t>(k)] * in[static_cast<std::size_t>(k)];
        return y;
    }
};

bool same_bits(const float* a, const float* b, std::size_t n) { return std::memcmp(a, b, n * sizeof(float)) == 0; }

std::vector<float> run(Lstm& net, const std::vector<float>& x, int block) {
    std::vector<float> y(x.size());
    for (std::size_t at = 0; at < x.size(); at += static_cast<std::size_t>(block)) {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - at));
        net.process(x.data() + at, y.data() + at, n);
    }
    return y;
}

bool make(Lstm& net, const LstmSpec& s, std::uint32_t seed, int max_frames, double rate = 48000.0) {
    std::string err;
    if (!net.load(s, weights_for(s, seed), &err)) {
        std::fprintf(stderr, "load: %s\n", err.c_str());
        return false;
    }
    net.prepare(max_frames, rate);
    return true;
}

int test_against_naive() {
    const std::vector<float> x = signal(dsp_quick() ? 6000 : 12000);
    const LstmSpec specs[] = {spec(1, 16), spec(2, 8), spec(1, 3), spec(3, 12)};
    for (const LstmSpec& s : specs) {
        // A short prewarm keeps the double oracle quick; the kernel's is the
        // same int(0.5 x rate), here 0.5 x 2000.
        Lstm net;
        CHECK(make(net, s, 11u + static_cast<std::uint32_t>(s.hidden_size), 128, 2000.0));
        NaiveLstm ref(s, weights_for(s, 11u + static_cast<std::uint32_t>(s.hidden_size)), 1000);
        const std::vector<float> y = run(net, x, 128);
        double worst = 0.0, peak = 0.0;
        for (std::size_t t = 0; t < x.size(); ++t) {
            const double r = ref.step(static_cast<double>(x[t]));
            peak = std::max(peak, std::fabs(r));
            worst = std::max(worst, std::fabs(static_cast<double>(y[t]) - r));
        }
        std::printf("  %dx%-3d (%s): max |kernel - double LSTM| %.3g, peak %.3g\n", s.num_layers, s.hidden_size,
                    net.kernels_name(), worst, peak);
        CHECK_LE(worst, 1e-5, "max abs difference from the double LSTM");
        CHECK(peak > 0.01);  // a network that says nothing proves nothing
    }
    return 0;
}

int test_blocks_and_reset() {
    const std::vector<float> x = signal(9000);
    const LstmSpec s = spec(2, 8);
    Lstm a;
    CHECK(make(a, s, 5, 256));
    const std::vector<float> ref = run(a, x, 1);
    // Block-size independence: one sample, odd sizes, the maximum, and a
    // block over it (process() takes any frame count).
    for (const int block : {37, 128, 256, 4000}) {
        Lstm b;
        CHECK(make(b, s, 5, 256));
        CHECK(same_bits(ref.data(), run(b, x, block).data(), x.size()));
    }
    // Reset restores the prewarmed state exactly, in the middle of a run.
    a.reset();
    CHECK(same_bits(ref.data(), run(a, x, 64).data(), x.size()));
    // prepare() again (a new block size) lands on the same state.
    a.prepare(64, 48000.0);
    CHECK(same_bits(ref.data(), run(a, x, 64).data(), x.size()));
    // in and out may alias.
    a.reset();
    std::vector<float> inplace = x;
    for (std::size_t at = 0; at < inplace.size(); at += 100)
        a.process(inplace.data() + at, inplace.data() + at, static_cast<int>(std::min<std::size_t>(100, x.size() - at)));
    CHECK(same_bits(ref.data(), inplace.data(), x.size()));
    std::printf("  blocks 1/37/128/256/4000, reset, re-prepare, in place: identical bits\n");
    return 0;
}

int test_silence() {
    // Prewarmed: silence in, the network's settled response out, from the
    // first sample (that is what the half second of warm-up is for).
    Lstm net;
    CHECK(make(net, spec(1, 16), 3, 128));
    const std::vector<float> zeros(4800, 0.0f);
    const std::vector<float> y = run(net, zeros, 128);
    float lo = y[0], hi = y[0];
    for (float v : y) {
        CHECK(std::isfinite(v));
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    std::printf("  silence: output %.6g, spread %.3g over 0.1 s\n", static_cast<double>(y[0]),
                static_cast<double>(hi - lo));
    CHECK_LE(hi - lo, 1e-6, "output spread on silence after the prewarm");

    // Not loaded, or loaded and not prepared: silence out.
    Lstm empty;
    std::vector<float> out(64, 1.0f);
    empty.process(zeros.data(), out.data(), 64);
    for (float v : out) CHECK(v == 0.0f);
    Lstm unprepared;
    std::string err;
    CHECK(unprepared.load(spec(1, 4), weights_for(spec(1, 4), 1), &err));
    std::fill(out.begin(), out.end(), 1.0f);
    unprepared.process(zeros.data(), out.data(), 64);
    for (float v : out) CHECK(v == 0.0f);
    return 0;
}

int test_hostile_input() {
    const LstmSpec s = spec(2, 16);
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> x = signal(4096);
    const float hostile[] = {inf, -inf, nan, FLT_MAX, -FLT_MAX, 1e30f, -1e30f, 1e-40f, 16.0f, -16.0f};
    for (std::size_t t = 1000; t < 3000; ++t) x[t] = hostile[t % 10];
    Lstm net;
    CHECK(make(net, s, 8, 128));
    const std::vector<float> y = run(net, x, 128);
    for (float v : y) CHECK(std::isfinite(v));

    // NaN and infinities read as silence, so they give silence's bits;
    // huge finite values are held to +-16.
    std::vector<float> clean = x;
    for (float& v : clean) {
        if (!std::isfinite(v)) v = 0.0f;
        v = std::min(std::max(v, -16.0f), 16.0f);
    }
    net.reset();
    CHECK(same_bits(y.data(), run(net, clean, 128).data(), x.size()));

    // Minutes of a full-scale square: the cell state's growth stays harmless.
    std::vector<float> square(48000);
    for (std::size_t t = 0; t < square.size(); ++t) square[t] = (t / 50) % 2 ? 16.0f : -16.0f;
    for (int rep = 0; rep < (dsp_quick() ? 5 : 60); ++rep) {
        const std::vector<float> ys = run(net, square, 128);
        for (float v : ys) CHECK(std::isfinite(v));
    }
    std::printf("  inf, NaN, FLT_MAX, 1e30, denormals, a long +-16 square: finite throughout\n");
    return 0;
}

int test_no_allocation() {
    Lstm net;
    CHECK(make(net, spec(2, 24), 4, 128));
    const std::vector<float> x = signal(2048);
    std::vector<float> y(x.size());
    g_allocations = 0;
    g_counting = true;
    net.process(x.data(), y.data(), 128);
    net.reset();
    net.process(x.data(), y.data(), 1);
    net.process(x.data(), y.data(), 2048);
    g_counting = false;
    CHECK(g_allocations.load() == 0);
    return 0;
}

int test_refusals() {
    Lstm net;
    std::string err;
    CHECK(!net.load(spec(1, 16), std::vector<float>(10, 0.0f), &err));
    CHECK(err.find("1201") != std::string::npos);
    CHECK(!net.loaded());
    std::vector<float> w = weights_for(spec(1, 16), 1);
    w[100] = std::numeric_limits<float>::quiet_NaN();
    err.clear();
    CHECK(!net.load(spec(1, 16), w, &err));
    CHECK(err.find("finite") != std::string::npos);
    CHECK(!net.load(spec(5, 16), std::vector<float>(10, 0.0f), &err));
    CHECK(!net.load(spec(1, 129), std::vector<float>(10, 0.0f), &err));
    // A refused load unloads a kernel that held a network.
    CHECK(net.load(spec(1, 4), weights_for(spec(1, 4), 2), &err));
    CHECK(net.loaded());
    CHECK(!net.load(spec(1, 4), std::vector<float>(3, 0.0f), &err));
    CHECK(!net.loaded());
    return 0;
}

// The product, over every tail length and row grouping, and whole networks.
int test_simd_bits() {
    using detail::lstm_matvec_generic;
    const detail::LstmMatvec simd = detail::simd_kernels() != nullptr ? detail::lstm_simd_matvec() : nullptr;
    if (simd == nullptr) {
        std::printf("  simd: none on this processor; the generic product only\n");
        return 0;
    }
    Noise n(17);
    for (int nin = 1; nin <= 41; ++nin) {
        for (int rows = 1; rows <= 9; ++rows) {
            std::vector<float> w(static_cast<std::size_t>(rows * nin)), x(static_cast<std::size_t>(nin));
            for (float& v : w) v = n.next();
            for (float& v : x) v = n.next();
            std::vector<float> a(static_cast<std::size_t>(rows)), b(static_cast<std::size_t>(rows));
            lstm_matvec_generic(w.data(), rows, nin, x.data(), a.data());
            simd(w.data(), rows, nin, x.data(), b.data());
            CHECK(same_bits(a.data(), b.data(), a.size()));
        }
    }
    const std::vector<float> x = signal(6000);
    for (const LstmSpec& s : {spec(1, 16), spec(2, 8), spec(1, 5), spec(4, 32)}) {
        Lstm g, v;
        g.allow_simd(false);
        CHECK(make(g, s, 21, 128));
        CHECK(make(v, s, 21, 128));
        CHECK(std::strcmp(g.kernels_name(), "generic") == 0);
        CHECK(std::strcmp(v.kernels_name(), "generic") != 0);
        CHECK(same_bits(run(g, x, 128).data(), run(v, x, 37).data(), x.size()));
    }
    std::printf("  simd (%s): product and whole networks identical to the generic kernel\n",
                detail::simd_kernels()->name);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_against_naive();
    failed += test_blocks_and_reset();
    failed += test_silence();
    failed += test_hostile_input();
    failed += test_no_allocation();
    failed += test_refusals();
    failed += test_simd_bits();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp lstm: all passed\n");
    return 0;
}
