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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

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
    // And the edges: tiny, the 0.25 and 9 joins of tanh_approx, large.
    for (float v : {1e-30f, 1e-8f, 0.2499999f, 0.25f, 0.2500001f, 8.999f, 9.0f, 9.001f, 50.0f, 1e4f}) {
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
        CHECK_LE(worst, a == Activation::fast_tanh ? 1e-6 : 2.5e-7, activation_name(a));
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
    // An unloaded kernel outputs silence rather than reading nothing.
    float buf[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    WaveNet empty;
    empty.prepare(4);
    empty.process(buf, buf, 4);
    CHECK(buf[0] == 0.0f && buf[3] == 0.0f);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_activations();
    failed += test_against_naive();
    failed += test_blocks_reset_nonfinite_alloc();
    failed += test_refusals();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("dsp neural: all passed\n");
    return 0;
}
