// SPDX-License-Identifier: GPL-3.0-or-later
//
// ConvolutionKernel::set()'s tap bound (ADR-0180): a tap past +-kMaxTap
// (1e30), Inf or NaN is refused wherever it sits (head or tail partition),
// the bound itself is taken, and a refused set() leaves the kernel, and
// what the convolver makes with it, exactly as it was.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "check.h"
#include "sonora/dsp/convolver.h"

namespace {

using sonora::dsp::ConvolutionKernel;
using sonora::dsp::Convolver;

constexpr std::uint32_t kPartition = 32;
constexpr std::uint32_t kTaps = 200;  // a head and six tail partitions

std::vector<double> decaying_ir(std::uint32_t taps) {
    std::vector<double> h(taps);
    for (std::uint32_t i = 0; i < taps; ++i) h[i] = std::exp(-0.03 * i) * ((i % 3) == 0 ? 0.5 : -0.25);
    return h;
}

std::vector<float> render(const ConvolutionKernel& k) {
    Convolver c;
    if (!c.prepare(kPartition, kTaps)) return {};
    std::vector<float> x(512), y(512);
    for (std::size_t i = 0; i < x.size(); ++i) x[i] = static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
    c.process(k, x.data(), y.data(), static_cast<std::uint32_t>(x.size()));
    return y;
}

// Every value a tap must not hold, at a head index and at a tail index.
int test_bad_taps_refused() {
    ConvolutionKernel k;
    CHECK(k.prepare(kPartition, kTaps));
    const auto h = decaying_ir(kTaps);
    CHECK(k.set(h.data(), kTaps));
    const auto before = render(k);
    CHECK(!before.empty());
    const double inf = std::numeric_limits<double>::infinity();
    const double bad[] = {std::nextafter(ConvolutionKernel::kMaxTap, inf),
                          -std::nextafter(ConvolutionKernel::kMaxTap, inf),
                          2e30,
                          1e300,
                          std::numeric_limits<double>::max(),
                          inf,
                          -inf,
                          std::numeric_limits<double>::quiet_NaN()};
    for (std::uint32_t at : {0u, 5u, kPartition + 1, kTaps - 1}) {
        for (double v : bad) {
            auto g = h;
            g[at] = v;
            CHECK(!k.set(g.data(), kTaps));
            CHECK(k.taps() == kTaps && k.tail_partitions() == 6);
        }
    }
    const auto after = render(k);
    CHECK(after.size() == before.size());
    for (std::size_t i = 0; i < after.size(); ++i) CHECK(after[i] == before[i]);
    std::printf("  taps past +-1e30, Inf, NaN: refused in head and tail, kernel unchanged\n");
    return 0;
}

// The bound is |h| <= 1e30, both signs.
int test_bound_taken() {
    ConvolutionKernel k;
    CHECK(k.prepare(kPartition, kTaps));
    auto g = decaying_ir(kTaps);
    g[3] = ConvolutionKernel::kMaxTap;
    g[kPartition + 7] = -ConvolutionKernel::kMaxTap;
    CHECK(k.set(g.data(), 60) && k.taps() == 60);
    CHECK(!k.set(nullptr, 10));
    CHECK(k.taps() == 60);
    std::printf("  taps of exactly +-1e30: taken\n");
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_bad_taps_refused();
    failed += test_bound_taken();
    if (failed == 0) std::printf("dsp convolver refusals: all passed\n");
    return failed == 0 ? 0 : 1;
}
