// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LSTM kernel against the NAM project's MIT reference implementation
// (ADR-0011 §3.2, ADR-0111: maximum absolute difference <= 1e-5 on the
// same input), through the `.nam` reader, on the generic product and on
// the SIMD one when this processor runs it.
//
// tests/data/lstm_1x16 and lstm_2x8 are our own seeded networks;
// lstm_*.ref.f32 is the reference's output on input.f32 after its own
// half-second prewarm, written by tests/oracle/generate.sh. The reference
// is never built or linked here.
//
//   test_native_ampcab_lstm_equiv DATA_DIR
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "sonora/ampcab/nam_file.h"
#include "sonora/dsp/neural/lstm.h"

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace {

using namespace sonora;

std::string g_data;

bool quick() {
    const char* q = std::getenv("SONORA_DSP_QUICK");
    return q != nullptr && std::strcmp(q, "1") == 0;
}

std::vector<float> read_f32(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<float> x;
    unsigned char b[4];
    // Little-endian on disk whatever the host (generate.sh writes it so).
    while (f.read(reinterpret_cast<char*>(b), 4)) {
        const std::uint32_t u = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
                                (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
        float v;
        std::memcpy(&v, &u, sizeof v);
        x.push_back(v);
    }
    return x;
}

int test_reads() {
    ampcab::NamModel m;
    std::string err;
    CHECK(ampcab::read_nam_file(g_data + "/lstm_1x16.nam", &m, &err));
    CHECK(m.architecture == "LSTM" && m.lstm.num_layers == 1 && m.lstm.hidden_size == 16);
    CHECK(m.weights.size() == 1201 && m.sample_rate == 48000.0);
    CHECK(ampcab::read_nam_file(g_data + "/lstm_2x8.nam", &m, &err));
    CHECK(m.lstm.num_layers == 2 && m.lstm.hidden_size == 8 && m.weights.size() == 905);
    return 0;
}

int test_equivalence() {
    const std::vector<float> x = read_f32(g_data + "/input.f32");
    CHECK(x.size() == 8192);
    // The oracle ran at 64 frames. Ours: one frame, odd sizes, a block over
    // the prepared maximum.
    const std::vector<std::pair<int, int>> full = {{1, 128}, {37, 128}, {64, 64}, {128, 128}, {1000, 256}};
    const std::vector<std::pair<int, int>> fast = {{37, 128}, {128, 128}};
    double worst_all = 0.0;
    for (const char* name : {"lstm_1x16", "lstm_2x8"}) {
        ampcab::NamModel m;
        std::string err;
        CHECK(ampcab::read_nam_file(g_data + "/" + name + ".nam", &m, &err));
        const std::vector<float> ref = read_f32(g_data + "/" + name + ".ref.f32");
        CHECK(ref.size() == x.size());
        double peak = 0.0;
        for (float v : ref) peak = std::max(peak, static_cast<double>(std::fabs(v)));
        for (const bool simd : {false, true}) {
            for (const auto& bm : quick() ? fast : full) {
                dsp::neural::Lstm net;
                net.allow_simd(simd);
                CHECK(net.load(m.lstm, m.weights, &err));
                if (simd && std::strcmp(net.kernels_name(), "generic") == 0) break;  // no SIMD here
                net.prepare(bm.second, m.sample_rate);
                std::vector<float> y(x.size());
                for (std::size_t at = 0; at < x.size(); at += static_cast<std::size_t>(bm.first)) {
                    const int n =
                        static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(bm.first), x.size() - at));
                    net.process(x.data() + at, y.data() + at, n);
                }
                double worst = 0.0;
                for (std::size_t t = 0; t < x.size(); ++t) {
                    const double e = std::fabs(static_cast<double>(y[t]) - static_cast<double>(ref[t]));
                    if (!(e <= worst)) worst = e;
                }
                worst_all = std::max(worst_all, worst);
                std::printf("  %-9s %-7s block %4d (max %3d): max |ours - reference| %.3g, reference peak %.3g\n",
                            name, net.kernels_name(), bm.first, bm.second, worst, peak);
                if (!(worst <= 1e-5)) {
                    std::fprintf(stderr, "FAIL %s: %.3g over the 1e-5 bar\n", name, worst);
                    return 1;
                }
            }
        }
        // A network whose output is near silence would prove nothing.
        CHECK(peak > 0.01);
    }
    std::printf("  max |ours - reference| over every case: %.3g (bar 1e-5)\n", worst_all);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_native_ampcab_lstm_equiv DATA_DIR\n");
        return 2;
    }
    g_data = argv[1];
    int failed = 0;
    failed += test_reads();
    failed += test_equivalence();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("native ampcab LSTM equivalence: all passed\n");
    return 0;
}
