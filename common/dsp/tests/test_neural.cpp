// SPDX-License-Identifier: GPL-3.0-or-later
//
// The WaveNet kernel (neural/wavenet.h) on its own: its activations
// against libm in double, the whole network against a plain per-sample
// implementation of the format's semantics written here in double (every
// activation, gated and not, one to three layer arrays, kernel sizes 1 to
// 4), block-size independence to the bit, reset, non-finite input, the
// load-time refusals, and no allocation in process() or reset().
//
// The equivalence with the NAM reference implementation itself, on .nam
// files, is plugins/native/ampcab/tests/test_nam.cpp.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "../src/neural/kernels.h"
#include "check.h"
#include "sonora/dsp/neural/wavenet.h"

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

double act_ref(Activation a, double x) {
    switch (a) {
        case Activation::tanh: return std::tanh(x);
        case Activation::fast_tanh: {
            const double ax = std::fabs(x), x2 = x * x;
            return (x * (2.45550750702956 + 2.45550750702956 * ax + (0.893229853513558 + 0.821226666969744 * ax) * x2) /
                    (2.44506634652299 + (2.44506634652299 + x2) * std::fabs(x + 0.814642734961073 * x * ax)));
        }
        case Activation::hard_tanh: return x < -1.0 ? -1.0 : (x > 1.0 ? 1.0 : x);
        case Activation::relu: return x > 0.0 ? x : 0.0;
        case Activation::leaky_relu: return x > 0.0 ? x : 0.01 * x;
        case Activation::sigmoid: return 1.0 / (1.0 + std::exp(-x));
        case Activation::silu: return x / (1.0 + std::exp(-x));
        case Activation::hard_swish: {
            const double t = x + 3.0;
            return x * (t < 0.0 ? 0.0 : (t > 6.0 ? 6.0 : t)) / 6.0;
        }
        case Activation::softsign: return x / (1.0 + std::fabs(x));
    }
    return 0.0;
}

const Activation kAll[] = {Activation::tanh,       Activation::fast_tanh, Activation::hard_tanh,
                           Activation::relu,       Activation::leaky_relu, Activation::sigmoid,
                           Activation::silu,       Activation::hard_swish, Activation::softsign};

int test_activations() {
    std::vector<float> x;
    for (int i = -400000; i <= 400000; ++i) x.push_back(static_cast<float>(i) * 5e-5f);
    // And the edges: tiny, tanh_approx's clamp at 7.9, the sigmoid's at
    // 80, large.
    for (float v : {1e-30f, 1e-8f, 0.25f, 7.8999f, 7.9f, 7.9001f, 9.0f, 50.0f, 79.9f, 80.0f, 80.1f, 1e4f}) {
        x.push_back(v);
        x.push_back(-v);
    }
    for (Activation a : kAll) {
        std::vector<float> y = x;
        apply_activation(a, y.data(), static_cast<int>(y.size()));
        double worst = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            const double ref = act_ref(a, static_cast<double>(x[i]));
            const double err = std::fabs(static_cast<double>(y[i]) - ref) / std::max(1.0, std::fabs(ref));
            if (!(err <= worst)) worst = err;  // NaN-safe: a NaN error becomes the worst
        }
        std::printf("  activation %-10s max error %.3g\n", activation_name(a), worst);
        // Fasttanh is the format's own float formula (vec_math.h): its
        // distance from the same formula in double is that formula's float
        // rounding, which the reference shares. The others are ours.
        // tanh is a rational form (vec_math.h): its float rounding near 1
        // is about 5 ulps, 3.2e-7 measured; the network's own bar is 1e-5.
        const double bar = a == Activation::fast_tanh ? 1e-6 : (a == Activation::tanh ? 3.5e-7 : 2.5e-7);
        CHECK_LE(worst, bar, activation_name(a));
    }
    // Names round-trip; an unknown one is refused.
    for (Activation a : kAll) {
        Activation back = Activation::relu;
        CHECK(activation_from_name(activation_name(a), &back) && back == a);
    }
    Activation dummy;
    CHECK(!activation_from_name("PReLU", &dummy) && !activation_from_name("tanh", &dummy));
    return 0;
}

// Weights at about 1/sqrt(fan-in), so the activations sit in their
// working range rather than saturating.
std::vector<float> random_weights(const WaveNetSpec& spec, std::uint32_t seed) {
    Noise noise(seed);
    std::vector<float> w(wavenet_weight_count(spec));
    const double fan = spec.arrays.front().channels * spec.arrays.front().kernel_size;
    for (float& v : w) v = noise.next() * static_cast<float>(1.7 / std::sqrt(fan));
    w.back() = 0.25f;  // the head scale
    return w;
}

// The format's semantics, sample by sample, in double, history before the
// signal at zero (as the reference allocates it). Written from wavenet.h's
// description, independently of the kernel's loop structure.
std::vector<double> naive(const WaveNetSpec& spec, const std::vector<float>& wf, const std::vector<float>& xin) {
    std::size_t at = 0;
    auto w = [&]() { return static_cast<double>(wf[at++]); };
    const int T = static_cast<int>(xin.size());
    using Rows = std::vector<std::vector<double>>;
    Rows cond(1, std::vector<double>(xin.begin(), xin.end()));
    Rows prev_out = cond, prev_head;
    for (std::size_t ai = 0; ai < spec.arrays.size(); ++ai) {
        const LayerArraySpec& a = spec.arrays[ai];
        const int C = a.channels, G = a.gated ? 2 : 1, K = a.kernel_size, H = a.head_size;
        Rows cur(C, std::vector<double>(T, 0.0));
        for (int o = 0; o < C; ++o)
            for (int j = 0; j < a.input_size; ++j) {
                const double v = w();
                for (int t = 0; t < T; ++t) cur[o][t] += v * prev_out[j][t];
            }
        Rows head_sum = ai == 0 ? Rows(C, std::vector<double>(T, 0.0)) : prev_head;
        for (int d : a.dilations) {
            Rows z(G * C, std::vector<double>(T, 0.0));
            for (int o = 0; o < G * C; ++o)
                for (int j = 0; j < C; ++j)
                    for (int k = 0; k < K; ++k) {
                        const double v = w();
                        const int back = d * (K - 1 - k);
                        for (int t = back; t < T; ++t) z[o][t] += v * cur[j][t - back];
                    }
            for (int o = 0; o < G * C; ++o) {
                const double b = w();
                for (int t = 0; t < T; ++t) z[o][t] += b;
            }
            for (int o = 0; o < G * C; ++o) {
                const double m = w();
                for (int t = 0; t < T; ++t) z[o][t] += m * cond[0][t];
            }
            for (int o = 0; o < C; ++o)
                for (int t = 0; t < T; ++t) {
                    z[o][t] = act_ref(a.activation, z[o][t]);
                    if (G == 2) z[o][t] *= act_ref(Activation::sigmoid, z[o + C][t]);
                    head_sum[o][t] += z[o][t];
                }
            Rows next = cur;
            for (int o = 0; o < C; ++o)
                for (int j = 0; j < C; ++j) {
                    const double v = w();
                    for (int t = 0; t < T; ++t) next[o][t] += v * z[j][t];
                }
            for (int o = 0; o < C; ++o) {
                const double b = w();
                for (int t = 0; t < T; ++t) next[o][t] += b;
            }
            cur = next;
        }
        Rows head(H, std::vector<double>(T, 0.0));
        for (int o = 0; o < H; ++o)
            for (int j = 0; j < C; ++j) {
                const double v = w();
                for (int t = 0; t < T; ++t) head[o][t] += v * head_sum[j][t];
            }
        if (a.head_bias)
            for (int o = 0; o < H; ++o) {
                const double b = w();
                for (int t = 0; t < T; ++t) head[o][t] += b;
            }
        prev_out = cur;
        prev_head = head;
    }
    const double scale = w();
    std::vector<double> y(T);
    for (int t = 0; t < T; ++t) y[t] = scale * prev_head[0][t];
    return y;
}

LayerArraySpec array_spec(int in, int c, int h, int k, std::vector<int> d, Activation a, bool gated, bool hb) {
    LayerArraySpec s;
    s.input_size = in;
    s.channels = c;
    s.head_size = h;
    s.kernel_size = k;
    s.dilations = std::move(d);
    s.activation = a;
    s.gated = gated;
    s.head_bias = hb;
    return s;
}

std::vector<float> test_signal(int n, std::uint32_t seed) {
    Noise noise(seed);
    std::vector<float> x(static_cast<std::size_t>(n));
    for (int t = 0; t < n; ++t) {
        // Noise under a slow ramp in level, then a step: quiet and hot.
        const float level = t < n / 2 ? 0.05f + 0.9f * static_cast<float>(t) / static_cast<float>(n / 2) : 0.7f;
        x[static_cast<std::size_t>(t)] = level * noise.next();
    }
    return x;
}

int test_against_naive() {
    struct Case {
        const char* name;
        WaveNetSpec spec;
    };
    std::vector<Case> cases;
    for (Activation a : kAll) {
        WaveNetSpec s;
        s.arrays.push_back(array_spec(1, 4, 3, 3, {1, 2, 4}, a, false, false));
        s.arrays.push_back(array_spec(4, 3, 1, 2, {8, 1}, a, false, true));
        cases.push_back({activation_name(a), s});
    }
    {
        WaveNetSpec s;
        s.arrays.push_back(array_spec(1, 3, 2, 3, {1, 3, 9}, Activation::tanh, true, true));
        s.arrays.push_back(array_spec(3, 2, 2, 1, {1, 1}, Activation::relu, true, false));
        s.arrays.push_back(array_spec(2, 2, 1, 4, {5}, Activation::silu, false, true));
        cases.push_back({"gated, three arrays", s});
    }
    {
        WaveNetSpec s;
        s.arrays.push_back(array_spec(1, 5, 1, 3, {1, 2, 4, 8, 16, 32}, Activation::tanh, false, true));
        cases.push_back({"one array", s});
    }
    std::uint32_t seed = 11;
    for (const Case& c : cases) {
        CHECK(wavenet_check(c.spec).empty());
        const std::vector<float> wts = random_weights(c.spec, seed++);
        WaveNet net;
        std::string err;
        CHECK(net.load(c.spec, wts, &err));
        net.prepare(64);
        const int pre = net.receptive_field() + 1;
        const std::vector<float> x = test_signal(3000, seed++);
        std::vector<float> padded(static_cast<std::size_t>(pre), 0.0f);
        padded.insert(padded.end(), x.begin(), x.end());
        const std::vector<double> ref = naive(c.spec, wts, padded);
        std::vector<float> y(x.size());
        net.process(x.data(), y.data(), static_cast<int>(x.size()));
        double worst = 0.0, peak = 0.0;
        for (std::size_t t = 0; t < x.size(); ++t) {
            const double r = ref[t + static_cast<std::size_t>(pre)];
            const double e = std::fabs(static_cast<double>(y[t]) - r);
            if (!(e <= worst)) worst = e;
            peak = std::max(peak, std::fabs(r));
        }
        std::printf("  naive %-20s max |diff| %.3g (output peak %.3g)\n", c.name, worst, peak);
        CHECK(peak > 1e-3);  // the case is not trivially silent
        CHECK_LE(worst, 1e-5, c.name);
    }
    return 0;
}

WaveNetSpec standard_spec() {
    WaveNetSpec s;
    const std::vector<int> d = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512};
    s.arrays.push_back(array_spec(1, 16, 8, 3, d, Activation::tanh, false, false));
    s.arrays.push_back(array_spec(16, 8, 1, 3, d, Activation::tanh, false, true));
    return s;
}

int test_blocks_reset_nonfinite_alloc() {
    const WaveNetSpec spec = standard_spec();
    CHECK(wavenet_weight_count(spec) == 13802);  // the trainer's "standard" file
    CHECK(wavenet_receptive_field(spec) == 4092);
    const std::vector<float> wts = random_weights(spec, 99);
    const std::vector<float> x = test_signal(dsp_quick() ? 6000 : 20000, 5);
    const int max_frames = 128;

    WaveNet one;
    CHECK(one.load(spec, wts, nullptr));
    one.prepare(max_frames);
    std::vector<float> whole(x.size());
    // Longer than max_frames: process() runs it in pieces.
    one.process(x.data(), whole.data(), static_cast<int>(x.size()));

    // Any partition into blocks gives the same samples, to the bit.
    for (int block : {1, 7, 64, 128, 100}) {
        WaveNet net;
        CHECK(net.load(spec, wts, nullptr));
        net.prepare(max_frames);
        std::vector<float> y(x.size()), again(x.size());
        g_counting = true;
        for (std::size_t at = 0; at < x.size(); at += static_cast<std::size_t>(block)) {
            const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), x.size() - at));
            net.process(x.data() + at, y.data() + at, n);
        }
        // reset() then the same render: identical to a fresh network.
        net.reset();
        net.process(x.data(), again.data(), static_cast<int>(x.size()));
        g_counting = false;
        CHECK(g_allocations.load() == 0);
        for (std::size_t t = 0; t < x.size(); ++t) {
            CHECK(y[t] == whole[t]);
            CHECK(again[t] == whole[t]);
        }
    }

    // NaN and Inf in: finite out, and the same as with silence there.
    std::vector<float> bad = x, silent = x;
    const float poison[] = {NAN, INFINITY, -INFINITY};
    for (std::size_t t = 1000; t < 1300; ++t) {
        bad[t] = poison[t % 3];
        silent[t] = 0.0f;
    }
    // In place: in and out alias.
    WaveNet a, b;
    CHECK(a.load(spec, wts, nullptr) && b.load(spec, wts, nullptr));
    a.prepare(max_frames);
    b.prepare(max_frames);
    a.process(bad.data(), bad.data(), static_cast<int>(bad.size()));
    b.process(silent.data(), silent.data(), static_cast<int>(silent.size()));
    for (std::size_t t = 0; t < x.size(); ++t) {
        CHECK(std::isfinite(bad[t]));
        CHECK(bad[t] == silent[t]);
    }
    // A hot input is held, not passed through to overflow.
    std::vector<float> hot(512, 1e30f);
    a.process(hot.data(), hot.data(), 512);
    for (float v : hot) CHECK(std::isfinite(v));
    return 0;
}

int test_refusals() {
    WaveNet net;
    std::string err;
    WaveNetSpec spec = standard_spec();
    std::vector<float> wts = random_weights(spec, 3);
    wts.pop_back();
    CHECK(!net.load(spec, wts, &err) && !net.loaded());
    CHECK(err.find("13802") != std::string::npos);
    std::printf("  refused: %s\n", err.c_str());

    auto refused = [&](WaveNetSpec s, const char* what) {
        const std::string why = wavenet_check(s);
        std::printf("  refused (%s): %s\n", what, why.c_str());
        return !why.empty();
    };
    WaveNetSpec s = standard_spec();
    s.arrays[1].head_size = 2;
    CHECK(refused(s, "two outputs"));
    s = standard_spec();
    s.arrays[1].channels = 4;
    CHECK(refused(s, "channels != previous head_size"));
    s = standard_spec();
    s.arrays[0].condition_size = 2;
    CHECK(refused(s, "condition_size"));
    s = standard_spec();
    s.arrays[0].dilations.push_back(0);
    CHECK(refused(s, "dilation 0"));
    s = standard_spec();
    s.arrays[0].channels = 100000;
    CHECK(refused(s, "huge"));
    s.arrays.clear();
    CHECK(refused(s, "empty"));

    // Review of #318: every dimension within its own cap, the totals not.
    // One array, kernel 2, 32 layers at dilation 32768: a 52 KB file at 16
    // channels made prepare() run 19 s and take 132 MiB; at 128 channels
    // (3.1 MB) it ran for minutes towards 1 GiB. Refused at the check, and
    // load() returns at once without sizing anything.
    for (int ch : {16, 128}) {
        WaveNetSpec h;
        h.arrays.push_back(array_spec(1, ch, 1, 2, std::vector<int>(32, 32768), Activation::tanh, false, true));
        CHECK(refused(h, ch == 16 ? "hostile, 16 channels" : "hostile, 128 channels"));
        std::vector<float> zeros(wavenet_weight_count(h), 0.0f);
        const auto t0 = std::chrono::steady_clock::now();
        WaveNet hostile;
        CHECK(!hostile.load(h, zeros, &err));
        hostile.prepare(128);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
    }
    // Wide and deep without a long history: the rows, then the warm-up
    // (32 channels, 30 layers at dilation 1024: every other total in range,
    // 7.7e9 multiply-adds to prewarm).
    WaveNetSpec wide;
    wide.arrays.push_back(array_spec(1, 128, 1, 32, std::vector<int>(64, 1), Activation::tanh, false, true));
    CHECK(refused(wide, "layer rows"));
    WaveNetSpec warm;
    warm.arrays.push_back(array_spec(1, 32, 1, 3, std::vector<int>(30, 1024), Activation::tanh, false, true));
    CHECK(refused(warm, "warm-up work") && wavenet_check(warm).find("warm-up") != std::string::npos);
    // The trainer's presets stay well inside every total.
    CHECK(wavenet_check(standard_spec()).empty());
    // An unloaded kernel outputs silence rather than reading nothing.
    float buf[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    WaveNet empty;
    empty.prepare(4);
    empty.process(buf, buf, 4);
    CHECK(buf[0] == 0.0f && buf[3] == 0.0f);
    return 0;
}

bool same_bits(const float* a, const float* b, std::size_t n) {
    return std::memcmp(a, b, n * sizeof(float)) == 0;
}

// The SIMD kernel table (AVX2 on x86-64, kernels.h) gives the generic
// one's bits: products at every row count and frame count that exercise
// its 8-, 4- and 1-row tiles and its scalar tail, the activations over the
// sweep test_activations() uses plus NaN and Inf at every tail length, and
// whole networks through WaveNet::process at several block sizes. A
// capture then sounds the same on every machine, and the equivalence test
// against the reference, run on whichever table the runner picked, holds
// for both.
int test_simd_bits() {
    const detail::Kernels& gen = detail::generic_kernels();
    const detail::Kernels* simd = detail::simd_kernels();
    if (simd == nullptr) {
        std::printf("  simd: no SIMD kernel table on this processor; the generic one runs\n");
        WaveNet net;
        CHECK(std::strcmp(net.kernels_name(), "generic") == 0);
        return 0;
    }
    std::printf("  simd: checking the %s table against the generic one\n", simd->name);

    Noise noise(1234);
    const int max_nin = 48, max_n = 130;
    std::vector<float> rows_in(static_cast<std::size_t>(max_nin * max_n));
    for (float& v : rows_in) v = noise.next() * 3.0f;
    // Zeros, signed zeros and tiny and huge values among the inputs.
    rows_in[0] = 0.0f;
    rows_in[1] = -0.0f;
    rows_in[2] = 1e-38f;
    rows_in[3] = 3e38f;
    std::vector<const float*> in(static_cast<std::size_t>(max_nin));
    for (int i = 0; i < max_nin; ++i) in[static_cast<std::size_t>(i)] = rows_in.data() + i * max_n;
    long products = 0;
    for (int rows : {1, 3, 4, 5, 8, 9, 12, 16, 17, 32}) {
        for (int nin : {1, 3, 16, 48}) {
            std::vector<float> w(static_cast<std::size_t>(rows * nin));
            for (float& v : w) v = noise.next();
            for (int n : {1, 7, 8, 9, 37, 128, 130}) {
                const std::size_t stride = static_cast<std::size_t>(max_n) + 3;
                std::vector<float> a(stride * static_cast<std::size_t>(rows), 7.0f), b = a;
                gen.matmul_rows(w.data(), rows, nin, in.data(), n, a.data(), stride);
                simd->matmul_rows(w.data(), rows, nin, in.data(), n, b.data(), stride);
                if (!same_bits(a.data(), b.data(), a.size())) {
                    std::fprintf(stderr, "  matmul differs: rows %d nin %d n %d\n", rows, nin, n);
                    CHECK(false);
                }
                ++products;
            }
        }
    }
    std::printf("  simd: %ld products identical\n", products);

    std::vector<float> x;
    for (int i = -400000; i <= 400000; ++i) x.push_back(static_cast<float>(i) * 5e-5f);
    for (float v : {1e-30f, 1e-8f, 0.25f, 7.8999f, 7.9f, 7.9001f, 9.0f, 50.0f, 79.9f, 80.0f, 80.1f, 1e4f, 3e38f,
                    INFINITY}) {
        x.push_back(v);
        x.push_back(-v);
    }
    x.push_back(NAN);
    x.push_back(-0.0f);
    for (int which = 0; which < 2; ++which) {
        auto fg = which == 0 ? gen.tanh : gen.sigmoid;
        auto fs = which == 0 ? simd->tanh : simd->sigmoid;
        std::vector<float> a = x, b = x;
        fg(a.data(), static_cast<int>(a.size()));
        fs(b.data(), static_cast<int>(b.size()));
        CHECK(same_bits(a.data(), b.data(), a.size()));
        // Every tail length: the padded last vector.
        for (int n = 1; n <= 17; ++n) {
            std::vector<float> c(x.end() - 40, x.end()), d = c;
            fg(c.data(), n);
            fs(d.data(), n);
            CHECK(same_bits(c.data(), d.data(), c.size()));
        }
    }

    // Whole networks: the standard size, and a gated one with a sigmoid
    // array, so every dispatched kernel runs inside process().
    WaveNetSpec gated;
    gated.arrays.push_back(array_spec(1, 6, 5, 3, {1, 2, 4, 8}, Activation::tanh, true, false));
    gated.arrays.push_back(array_spec(6, 5, 1, 2, {16, 1, 3}, Activation::sigmoid, false, true));
    const WaveNetSpec specs[] = {standard_spec(), gated};
    const std::vector<float> sig = test_signal(dsp_quick() ? 3000 : 12000, 77);
    for (const WaveNetSpec& spec : specs) {
        const std::vector<float> wts = random_weights(spec, 4242);
        for (int block : {37, 128}) {
            WaveNet g, s;
            g.allow_simd(false);
            CHECK(g.load(spec, wts, nullptr) && s.load(spec, wts, nullptr));
            CHECK(std::strcmp(g.kernels_name(), "generic") == 0);
            CHECK(std::strcmp(s.kernels_name(), simd->name) == 0);
            g.prepare(128);
            s.prepare(128);
            std::vector<float> yg(sig.size()), ys(sig.size());
            for (std::size_t at = 0; at < sig.size(); at += static_cast<std::size_t>(block)) {
                const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), sig.size() - at));
                g.process(sig.data() + at, yg.data() + at, n);
                s.process(sig.data() + at, ys.data() + at, n);
            }
            CHECK(same_bits(yg.data(), ys.data(), sig.size()));
        }
    }
    std::printf("  simd: whole networks identical to the generic kernel\n");
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_activations();
    failed += test_against_naive();
    failed += test_blocks_reset_nonfinite_alloc();
    failed += test_refusals();
    failed += test_simd_bits();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp neural: all passed\n");
    return 0;
}
