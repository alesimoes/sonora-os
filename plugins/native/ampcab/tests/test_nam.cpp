// SPDX-License-Identifier: GPL-3.0-or-later
//
// The `.nam` reader and the WaveNet kernel against the NAM project's MIT
// reference implementation (ADR-0011 §3.2, "Neural kernel equivalence":
// maximum absolute difference <= 1e-5 on the same input).
//
// tests/data/ holds our own test networks (seeded weights in the format's
// architectures, from tiny to the trainer's "standard"), one input signal,
// and the reference implementation's output on each, written by
// tests/oracle/generate.sh. The kernel runs them at several block sizes,
// none of them the oracle's 64-frame block alone.
//
//   test_native_ampcab DATA_DIR
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "sonora/ampcab/nam_file.h"
#include "sonora/dsp/neural/wavenet.h"

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            return 1;                                                                      \
        }                                                                                  \
    } while (0)

namespace {

using namespace sonora;
using dsp::neural::Activation;

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

int test_reads_the_test_networks() {
    ampcab::NamModel m;
    std::string err;
    CHECK(ampcab::read_nam_file(g_data + "/standard.nam", &m, &err));
    CHECK(m.architecture == "WaveNet" && m.version == "0.5.4");
    CHECK(m.sample_rate_given && m.sample_rate == 48000.0);
    CHECK(m.has_loudness && m.loudness_db == -18.0);
    CHECK(m.wavenet.arrays.size() == 2);
    CHECK(m.wavenet.arrays[0].channels == 16 && m.wavenet.arrays[0].head_size == 8);
    CHECK(m.wavenet.arrays[1].channels == 8 && m.wavenet.arrays[1].head_size == 1);
    CHECK(m.wavenet.arrays[1].head_bias && !m.wavenet.arrays[0].head_bias);
    CHECK(m.wavenet.arrays[0].dilations.size() == 10 && m.wavenet.arrays[0].dilations[9] == 512);
    CHECK(m.weights.size() == 13802);
    CHECK(m.weights.back() == 0.02f);

    CHECK(ampcab::read_nam_file(g_data + "/gated.nam", &m, &err));
    CHECK(m.wavenet.arrays[0].gated && !m.wavenet.arrays[1].gated);
    CHECK(m.wavenet.arrays[1].activation == Activation::relu && m.wavenet.arrays[1].kernel_size == 2);
    CHECK(ampcab::read_nam_file(g_data + "/feather.nam", &m, &err));
    CHECK(m.wavenet.arrays[0].activation == Activation::fast_tanh);

    CHECK(!ampcab::read_nam_file(g_data + "/no-such-file.nam", &m, &err));
    std::printf("  refused: %s\n", err.c_str());
    return 0;
}

// A small valid file, then each way to break it.
std::string tiny_text(const std::string& layer0_extra = "", const std::string& top_extra = "",
                      const std::string& weights = "") {
    // 1 -> 2 channels, head 1: rechannel 2, layer 2*2*1 + 2 + 2 + 4 + 2 = 14,
    // head 2 + bias 1, head scale 1: 20 weights.
    std::string w = weights;
    if (w.empty()) {
        for (int i = 0; i < 20; ++i) w += (i ? "," : "") + std::string(i % 2 ? "0.5" : "-0.25");
    }
    return "{\"version\":\"0.5.4\",\"architecture\":\"WaveNet\",\"config\":{\"layers\":[{"
           "\"input_size\":1,\"condition_size\":1,\"head_size\":1,\"channels\":2,\"kernel_size\":1,"
           "\"dilations\":[1],\"activation\":\"Tanh\",\"gated\":false,\"head_bias\":true" +
           layer0_extra + "}],\"head\":null,\"head_scale\":0.02" + top_extra + "},\"weights\":[" + w + "]}";
}

int test_refusals() {
    ampcab::NamModel m;
    std::string err;
    CHECK(ampcab::parse_nam(tiny_text(), &m, &err));
    CHECK(!m.sample_rate_given && m.sample_rate == 48000.0 && !m.has_loudness);
    // The newer keys at their neutral values are the same network.
    CHECK(ampcab::parse_nam(tiny_text(",\"bottleneck\":2,\"groups_input\":1,\"gating_mode\":\"none\","
                                      "\"layer1x1\":{\"active\":true,\"groups\":1},\"conv_pre_film\":false,"
                                      "\"head1x1\":{\"active\":false,\"out_channels\":2,\"groups\":1}"),
                            &m, &err));

    struct Bad {
        const char* what;
        std::string text;
    };
    const Bad bad[] = {
        {"not JSON", "{\"version\":"},
        {"LSTM", "{\"version\":\"0.5.4\",\"architecture\":\"LSTM\",\"config\":{},\"weights\":[]}"},
        {"no version", "{\"architecture\":\"WaveNet\"}"},
        {"unknown layer key", tiny_text(",\"secret_sauce\":1")},
        {"unknown config key", tiny_text("", ",\"absorb\":true")},
        {"FiLM", tiny_text(",\"conv_pre_film\":{\"active\":true}")},
        {"bottleneck", tiny_text(",\"bottleneck\":1")},
        {"groups", tiny_text(",\"groups_input\":2")},
        {"blended gating", tiny_text(",\"gating_mode\":\"blended\"")},
        {"no layer1x1", tiny_text(",\"layer1x1\":{\"active\":false,\"groups\":1}")},
        {"head1x1", tiny_text(",\"head1x1\":{\"active\":true,\"out_channels\":2,\"groups\":1}")},
        {"post-stack head", "{\"version\":\"0.5.4\",\"architecture\":\"WaveNet\",\"config\":{\"layers\":[],"
                            "\"head\":{\"channels\":2},\"head_scale\":0.02},\"weights\":[]}"},
        {"too few weights", tiny_text("", "", "0.1,0.2")},
        {"a weight that is not a number", tiny_text("", "", "\"x\"")},
        {"a weight beyond float", tiny_text("", "", "1e39,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0")},
        {"unknown activation", tiny_text(",\"activation\":\"Wobble\"").replace(
                                   tiny_text().find("\"activation\":\"Tanh\","), 20, "")},
        {"two inputs", tiny_text("", ",\"in_channels\":2")},
        {"condition_size", tiny_text().replace(tiny_text().find("\"condition_size\":1"), 18, "\"condition_size\":3")},
    };
    for (const Bad& b : bad) {
        err.clear();
        const bool ok = ampcab::parse_nam(b.text, &m, &err);
        std::printf("  refused (%s): %s\n", b.what, err.c_str());
        CHECK(!ok && !err.empty());
    }
    return 0;
}

int test_equivalence() {
    const std::vector<float> x = read_f32(g_data + "/input.f32");
    CHECK(x.size() == 8192);
    const char* names[] = {"tiny", "gated", "mixed", "feather", "standard"};
    // The oracle ran at 64 frames. Ours: odd sizes, a size over the
    // prepared maximum (run in pieces), and one frame at a time.
    const std::vector<std::pair<int, int>> full = {{1, 128}, {37, 128}, {64, 64}, {128, 128}, {1000, 256}};
    const std::vector<std::pair<int, int>> fast = {{37, 128}, {128, 128}};
    for (const char* name : names) {
        ampcab::NamModel m;
        std::string err;
        CHECK(ampcab::read_nam_file(g_data + "/" + name + ".nam", &m, &err));
        const std::vector<float> ref = read_f32(g_data + "/" + name + ".ref.f32");
        CHECK(ref.size() == x.size());
        double peak = 0.0;
        for (float v : ref) peak = std::max(peak, static_cast<double>(std::fabs(v)));
        for (const auto& bm : quick() ? fast : full) {
            dsp::neural::WaveNet net;
            CHECK(net.load(m.wavenet, m.weights, &err));
            net.prepare(bm.second);
            std::vector<float> y(x.size());
            for (std::size_t at = 0; at < x.size(); at += static_cast<std::size_t>(bm.first)) {
                const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(bm.first), x.size() - at));
                net.process(x.data() + at, y.data() + at, n);
            }
            double worst = 0.0;
            for (std::size_t t = 0; t < x.size(); ++t) {
                const double e = std::fabs(static_cast<double>(y[t]) - static_cast<double>(ref[t]));
                if (!(e <= worst)) worst = e;
            }
            std::printf("  %-8s block %4d (max %3d): max |ours - reference| %.3g, reference peak %.3g\n", name,
                        bm.first, bm.second, worst, peak);
            if (!(worst <= 1e-5)) {
                std::fprintf(stderr, "FAIL %s: %.3g over the 1e-5 bar\n", name, worst);
                return 1;
            }
        }
        // A network whose output is near silence would prove nothing.
        CHECK(peak > 0.01);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_native_ampcab DATA_DIR\n");
        return 2;
    }
    g_data = argv[1];
    int failed = 0;
    failed += test_reads_the_test_networks();
    failed += test_refusals();
    failed += test_equivalence();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("native ampcab .nam: all passed\n");
    return 0;
}
