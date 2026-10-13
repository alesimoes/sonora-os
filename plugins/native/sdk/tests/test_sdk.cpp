// SPDX-License-Identifier: GPL-3.0-or-later
//
// The base class's contract, on a toy processor: sample-accurate events,
// latency-compensated bypass, declicked switches, non-finite recovery,
// in-place hosts, oversized blocks, and the state codec.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/native/processor_base.h"
#include "sonora/native/state.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using namespace sonora::native;

const char* const kOffOn[] = {"off", "on"};
const ParamInfo kParams[] = {
    kBypassParam,
    {"gain", "Gain", Unit::Decibel, Taper::Linear, -24.0f, 24.0f, 0.0f, 0.0f, true, false, false,
     "%.1f dB", nullptr, 0},
    {"invert", "Invert", Unit::Toggle, Taper::Stepped, 0.0f, 1.0f, 0.0f, 0.0f, false, false, true,
     "%.0f", kOffOn, 2},
    {"delay", "Delay", Unit::None, Taper::Stepped, 0.0f, 64.0f, 5.0f, 0.0f, false, true, false,
     "%.0f", nullptr, 0},
};
const Descriptor kDesc = {"org.sonora.test", "Test", "Test", 1, kParams, 4, 1, 2, false};

// Output = input delayed by `delay` samples, times gain, times +-1.
class Toy : public ProcessorBase {
public:
    Toy() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        delay_ = static_cast<std::uint32_t>(value(3));
        line_.assign(channels(), std::vector<float>(delay_ + 1, 0.0f));
        set_latency(delay_);
        return true;
    }
    void on_reset() noexcept override {
        for (auto& l : line_) {
            for (float& v : l) v = 0.0f;
        }
        pos_ = 0;
    }
    void render(const float* const* in, float* const* out, const float* const*, std::uint32_t,
                std::uint32_t frames) noexcept override {
        const float g = static_cast<float>(std::pow(10.0, value(1) / 20.0)) * (value(2) >= 0.5f ? -1.0f : 1.0f);
        for (std::uint32_t i = 0; i < frames; ++i) {
            const std::uint32_t rd = (pos_ + 1) % (delay_ + 1);
            for (std::uint32_t c = 0; c < channels(); ++c) {
                line_[c][pos_] = in[c][i];
                out[c][i] = g * line_[c][rd];
            }
            pos_ = rd;
        }
    }

private:
    std::uint32_t delay_ = 0;
    std::uint32_t pos_ = 0;
    std::vector<std::vector<float>> line_;
};

struct Run {
    std::vector<std::vector<float>> out;
};

std::vector<std::vector<float>> render(Toy& t, const std::vector<std::vector<float>>& in,
                                       std::uint32_t block, const std::vector<ParamEvent>& events_abs) {
    const std::size_t n = in[0].size();
    std::vector<std::vector<float>> out(in.size(), std::vector<float>(n));
    std::vector<const float*> ip(in.size());
    std::vector<float*> op(in.size());
    for (std::size_t pos = 0; pos < n; pos += block) {
        const auto len = static_cast<std::uint32_t>(std::min<std::size_t>(block, n - pos));
        std::vector<ParamEvent> ev;
        for (const ParamEvent& e : events_abs) {
            if (e.offset >= pos && e.offset < pos + len) ev.push_back({static_cast<std::uint32_t>(e.offset - pos), e.index, e.value});
        }
        for (std::size_t c = 0; c < in.size(); ++c) {
            ip[c] = &in[c][pos];
            op[c] = &out[c][pos];
        }
        AudioBuffers io{ip.data(), op.data(), nullptr, static_cast<std::uint32_t>(in.size()), 0, len};
        t.process(io, ev.data(), static_cast<std::uint32_t>(ev.size()), Transport{});
    }
    return out;
}

std::vector<std::vector<float>> ramp(std::size_t n, std::uint32_t ch) {
    std::vector<std::vector<float>> x(ch, std::vector<float>(n));
    for (std::size_t i = 0; i < n; ++i) {
        for (std::uint32_t c = 0; c < ch; ++c) x[c][i] = 0.001f * static_cast<float>(i % 997) + 0.1f * c;
    }
    return x;
}

int test_events_land_on_their_sample() {
    Toy t;
    CHECK(t.prepare(48000.0, 64, 1));
    const auto in = ramp(1000, 1);
    const auto out = render(t, in, 128, {{300, 1, 6.0f}});
    const float g = static_cast<float>(std::pow(10.0, 6.0 / 20.0));
    for (std::size_t i = 5; i < 1000; ++i) {
        const float want = (i < 300 ? 1.0f : g) * in[0][i - 5];
        CHECK(std::fabs(out[0][i] - want) < 1e-6f);
    }
    return 0;
}

int test_bypass_is_latency_compensated() {
    Toy t;
    t.set_param(1, 12.0f);
    CHECK(t.prepare(48000.0, 128, 2));
    CHECK(t.latency() == 5);
    const auto in = ramp(48000, 2);
    // Bypass on at 0.25 s and off at 0.5 s; the 10 ms fades are between a
    // gained and an ungained copy of the same, equally delayed signal.
    const auto out = render(t, in, 128, {{12000, 0, 1.0f}, {24000, 0, 0.0f}});
    for (std::size_t i = 12000 + 480 + 1; i < 24000; ++i) {
        if (out[1][i] != in[1][i - 5]) {
            std::fprintf(stderr, "  bypass mismatch at %zu: %.7f vs %.7f\n", i, out[1][i], in[1][i - 5]);
        }
        CHECK(out[1][i] == in[1][i - 5]);
    }
    // Never a sample from another time: during the fade the output stays
    // between the two equally delayed copies.
    const float g = static_cast<float>(std::pow(10.0, 12.0 / 20.0));
    for (std::size_t i = 12000; i < 12000 + 480; ++i) {
        const float lo = in[0][i - 5], hi = g * in[0][i - 5];
        CHECK(out[0][i] >= lo - 1e-6f && out[0][i] <= hi + 1e-6f);
    }
    return 0;
}

int test_declick_has_no_step() {
    Toy t;
    CHECK(t.prepare(48000.0, 128, 1));
    std::vector<std::vector<float>> in(1, std::vector<float>(9600, 0.5f));
    const auto out = render(t, in, 128, {{4800, 2, 1.0f}});
    double worst = 0.0;
    for (std::size_t i = 6; i < out[0].size(); ++i) worst = std::fmax(worst, std::fabs(out[0][i] - out[0][i - 1]));
    // A bare switch would step by 1.0; the 3 + 3 ms raised-cosine fade
    // moves 0.5 to 0 in 144 samples: steps of at most pi/2 * 0.5 / 144.
    std::printf("declicked inversion: largest step %.5f\n", worst);
    CHECK(worst < 0.006);
    CHECK(std::fabs(out[0][9000] + 0.5f) < 1e-6f);
    CHECK(t.param(2) == 1.0f);
    return 0;
}

int test_nonfinite_input_recovers() {
    Toy t;
    CHECK(t.prepare(48000.0, 128, 1));
    auto in = ramp(1024, 1);
    in[0][100] = std::nanf("");
    in[0][200] = INFINITY;
    // Past +120 dBFS is a fault, not a signal: silenced like NaN. At the
    // limit it is still a sample.
    in[0][300] = 1e30f;
    in[0][310] = -1.0e6f;
    const auto out = render(t, in, 128, {});
    for (float v : out[0]) CHECK(std::isfinite(v));
    CHECK(out[0][105] == 0.0f);
    CHECK(out[0][305] == 0.0f);
    CHECK(out[0][315] == -1.0e6f);
    CHECK(out[0][600] == in[0][595]);
    return 0;
}

int test_in_place_and_oversized_blocks() {
    Toy a, b;
    CHECK(a.prepare(48000.0, 64, 1));
    CHECK(b.prepare(48000.0, 64, 1));
    const auto in = ramp(4096, 1);
    const auto ref = render(a, in, 64, {});
    // One 1000-frame block through a processor prepared for 64, in place.
    std::vector<float> buf(in[0].begin(), in[0].begin() + 1000);
    const float* ip[1] = {buf.data()};
    float* op[1] = {buf.data()};
    AudioBuffers io{ip, op, nullptr, 1, 0, 1000};
    b.process(io, nullptr, 0, Transport{});
    for (std::size_t i = 0; i < 1000; ++i) CHECK(buf[i] == ref[0][i]);
    return 0;
}

int test_state_round_trip() {
    Toy t;
    t.set_param(1, -3.5f);
    t.set_param(2, 1.0f);
    const std::string s = save_state(t);
    Toy u;
    std::string err;
    CHECK(load_state(u, s, err));
    CHECK(u.param(1) == -3.5f && u.param(2) == 1.0f && u.param(3) == 5.0f);
    // A missing id takes its default.
    CHECK(load_state(u, "{\"processor\": \"org.sonora.test\", \"state_version\": 1, \"params\": {\"gain\": 2.0}}", err));
    CHECK(u.param(1) == 2.0f && u.param(2) == 0.0f);
    // An unknown id is refused, with a message, and changes nothing.
    CHECK(!load_state(u, "{\"processor\": \"org.sonora.test\", \"state_version\": 1, \"params\": {\"gian\": 2.0}}", err));
    CHECK(err.find("gian") != std::string::npos);
    CHECK(!load_state(u, "{\"processor\": \"org.sonora.eq\", \"state_version\": 1, \"params\": {}}", err));
    CHECK(!load_state(u, "{\"processor\": \"org.sonora.test\", \"state_version\": 2, \"params\": {}}", err));
    const std::string d = descriptor_json(kDesc);
    CHECK(d.find("\"id\": \"gain\"") != std::string::npos);
    CHECK(d.find("\"reconfigures\": true") != std::string::npos);
    CHECK(d.find("\"formats\"") == std::string::npos);
    // A format's own name for a parameter travels with it, and only with it.
    const FormatSymbol lv2_bypass{0, "lv2", "lv2_enabled", true};
    const std::string f = descriptor_json(kDesc, &lv2_bypass, 1);
    const std::size_t first = f.find("\"formats\"");
    CHECK(first != std::string::npos && f.find("\"formats\"", first + 1) == std::string::npos);
    CHECK(first < f.find("\"id\": \"gain\""));
    CHECK(f.find("\"symbol\": \"lv2_enabled\"") != std::string::npos);
    CHECK(f.find("\"inverted\": true") != std::string::npos);
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_events_land_on_their_sample();
    failed += test_bypass_is_latency_compensated();
    failed += test_declick_has_no_step();
    failed += test_nonfinite_input_recovers();
    failed += test_in_place_and_oversized_blocks();
    failed += test_state_round_trip();
    if (failed != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failed);
        return 1;
    }
    std::printf("native sdk: all passed\n");
    return 0;
}
