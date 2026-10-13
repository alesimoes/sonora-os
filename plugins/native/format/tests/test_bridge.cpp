// SPDX-License-Identifier: GPL-3.0-or-later
//
// The format-neutral adapter (Bridge) on a toy processor of its own: the
// first block's values are the initial state, later ones are smoothed,
// reconfiguring values wait for activate(), parameters set from another
// thread arrive, and a bridged render equals the Processor's own render
// sample for sample. The bundles themselves are tested through Sonora's
// LV2 and VST3 hosts (plugins/host/CMakeLists.txt, native_*).
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#include "sonora/native/format/bridge.h"
#include "sonora/native/processor_base.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

// Every operator new in this test counts while `g_counting` is set: the
// re-activation test below asserts that nothing allocates.
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

using namespace sonora::native;
using sonora::native::format::Bridge;

const ParamInfo kParams[] = {
    kBypassParam,
    // 10 ms of smoothing: a change after the first block must ramp.
    {"gain", "Gain", Unit::None, Taper::Linear, 0.0f, 2.0f, 1.0f, 10.0f, true, false, false, "%.2f",
     nullptr, 0},
    {"delay", "Delay", Unit::None, Taper::Stepped, 0.0f, 64.0f, 0.0f, 0.0f, false, true, false, "%.0f",
     nullptr, 0},
};
enum : std::uint32_t { kBypass, kGain, kDelay };
const Descriptor kDesc = {"org.sonora.test.bridge", "Bridge test", "Test", 1, kParams, 3, 1, 2, false};

// Output = input delayed by `delay` samples, times the smoothed gain.
class Toy : public ProcessorBase {
public:
    Toy() : ProcessorBase(kDesc) {}

protected:
    bool on_prepare() override {
        delay_ = static_cast<std::uint32_t>(value(kDelay));
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
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float g = static_cast<float>(smoother(kGain).next());
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

// Takes a key input (Descriptor::sidechain) and outputs whatever keys it:
// the external key when the host hands one over, its own input otherwise
// (ProcessorBase's rule), so a test sees which one reached it.
const ParamInfo kKeyParams[] = {kBypassParam};
const Descriptor kKeyDesc = {"org.sonora.test.key", "Key test", "Test", 1, kKeyParams, 1, 1, 2, true};

class KeyToy : public ProcessorBase {
public:
    KeyToy() : ProcessorBase(kKeyDesc) {}

protected:
    bool on_prepare() override { return true; }
    void on_reset() noexcept override {}
    void render(const float* const*, float* const* out, const float* const* key, std::uint32_t,
                std::uint32_t frames) noexcept override {
        for (std::uint32_t c = 0; c < channels(); ++c) {
            for (std::uint32_t i = 0; i < frames; ++i) out[c][i] = key[c][i];
        }
    }
};

using Chans = std::vector<std::vector<float>>;

Chans ramp_input(std::uint32_t frames) {
    Chans x(2, std::vector<float>(frames));
    for (std::uint32_t i = 0; i < frames; ++i) {
        x[0][i] = 0.5f * std::sin(0.01f * static_cast<float>(i)) + 0.1f;
        x[1][i] = -0.25f * std::cos(0.013f * static_cast<float>(i)) - 0.1f;
    }
    return x;
}

// Runs `in` through the bridge in blocks of `block`, calling between(k)
// before block k.
template <class F>
Chans run_bridge(Bridge& b, const Chans& in, std::uint32_t block, F&& between) {
    const std::size_t n = in[0].size();
    Chans out(2, std::vector<float>(n, 0.0f));
    for (std::size_t pos = 0, k = 0; pos < n; pos += block, ++k) {
        between(k);
        const std::uint32_t m = static_cast<std::uint32_t>(std::min<std::size_t>(block, n - pos));
        const float* ip[2] = {in[0].data() + pos, in[1].data() + pos};
        float* op[2] = {out[0].data() + pos, out[1].data() + pos};
        b.run(ip, op, m, Transport{});
    }
    return out;
}
Chans run_bridge(Bridge& b, const Chans& in, std::uint32_t block) {
    return run_bridge(b, in, block, [](std::size_t) {});
}

// Values the host hands over before the first block are where the instance
// starts: no ramp from the default (a session reloaded with a gain of 0.5
// must not fade in from 1.0).
int test_first_block_values_jump() {
    Bridge b(std::make_unique<Toy>(), 2);
    CHECK(b.activate(48000.0, 128));
    b.set_value(kGain, 0.5f);  // LV2 delivers control ports this way: inside the first run()
    const Chans in = ramp_input(256);
    const Chans out = run_bridge(b, in, 128);
    for (std::uint32_t i = 0; i < 256; ++i) CHECK(out[0][i] == 0.5f * in[0][i]);
    return 0;
}

// After the first block a change is automation, and ramps.
int test_later_changes_are_smoothed() {
    Bridge b(std::make_unique<Toy>(), 2);
    CHECK(b.activate(48000.0, 128));
    const Chans in = ramp_input(2048);
    const Chans out = run_bridge(b, in, 128, [&](std::size_t k) {
        if (k == 2) b.set_value(kGain, 0.0f);
    });
    // Untouched before the change, a ramp after it (480 samples at 48 kHz),
    // and silent once it is done.
    for (std::uint32_t i = 0; i < 256; ++i) CHECK(out[0][i] == in[0][i]);
    CHECK(std::fabs(out[0][256 + 1]) > 0.9f * std::fabs(in[0][256 + 1]));
    CHECK(std::fabs(out[0][256 + 240]) < 0.6f * std::fabs(in[0][256 + 240]));
    CHECK(std::fabs(out[0][256 + 240]) > 0.4f * std::fabs(in[0][256 + 240]));
    for (std::uint32_t i = 256 + 480; i < 2048; ++i) CHECK(out[0][i] == 0.0f);
    return 0;
}

// A reconfiguring value set while inactive takes effect at activate();
// set while running, it waits for the next activate(), and the reported
// latency keeps matching what the audio does in the meantime.
int test_reconfiguring_values_wait_for_activate() {
    Bridge b(std::make_unique<Toy>(), 2);
    b.set_value(kDelay, 16.0f);
    CHECK(b.activate(48000.0, 128));
    CHECK(b.latency() == 16);
    Chans imp(2, std::vector<float>(256, 0.0f));
    imp[0][0] = 1.0f;
    Chans out = run_bridge(b, imp, 64);
    CHECK(out[0][16] == 1.0f);

    // While running: not applied, reported latency unchanged.
    b.set_value(kDelay, 32.0f);
    out = run_bridge(b, imp, 64);
    CHECK(b.latency() == 16);
    CHECK(b.value(kDelay) == 32.0f);
    CHECK(b.activate(48000.0, 128));
    CHECK(b.latency() == 32);
    out = run_bridge(b, imp, 64);
    CHECK(out[0][32] == 1.0f && out[0][16] == 0.0f);
    return 0;
}

// Not activated: silence, whatever the buffers held. Zero frames: nothing.
int test_inactive_is_silent() {
    Bridge b(std::make_unique<Toy>(), 2);
    Chans in = ramp_input(64);
    Chans out(2, std::vector<float>(64, 7.0f));
    const float* ip[2] = {in[0].data(), in[1].data()};
    float* op[2] = {out[0].data(), out[1].data()};
    b.run(ip, op, 64, Transport{});
    for (std::uint32_t i = 0; i < 64; ++i) CHECK(out[0][i] == 0.0f && out[1][i] == 0.0f);
    CHECK(b.activate(48000.0, 64));
    out[0][0] = 7.0f;
    b.run(ip, op, 0, Transport{});
    CHECK(out[0][0] == 7.0f);
    return 0;
}

// Values are sanitised on the way in, so what a host reads back is what the
// processor uses.
int test_values_are_sanitised() {
    Bridge b(std::make_unique<Toy>(), 2);
    b.set_value(kGain, 9.0f);
    CHECK(b.value(kGain) == 2.0f);
    b.set_value(kGain, std::nanf(""));
    CHECK(b.value(kGain) == 1.0f);  // the default
    b.set_value(kDelay, 3.4f);
    CHECK(b.value(kDelay) == 3.0f);
    b.set_value(99, 1.0f);  // no such parameter: ignored
    CHECK(b.value(99) == 0.0f);
    return 0;
}

// A VST3 host may call setState() on its main thread while process() runs:
// the value must arrive, and nothing may race (the TSan job runs this).
int test_values_from_another_thread() {
    Bridge b(std::make_unique<Toy>(), 2);
    CHECK(b.activate(48000.0, 128));
    const Chans in = ramp_input(128);
    Chans out(2, std::vector<float>(128));
    const float* ip[2] = {in[0].data(), in[1].data()};
    float* op[2] = {out[0].data(), out[1].data()};
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (int k = 0; !stop.load(std::memory_order_relaxed); ++k)
            b.set_value(kGain, (k & 1) ? 0.25f : 0.75f);
        b.set_value(kGain, 0.0f);
    });
    for (int k = 0; k < 2000; ++k) b.run(ip, op, 128, Transport{});
    stop.store(true);
    writer.join();
    // The last value set reaches the audio: 10 ms of ramp, then silence.
    for (int k = 0; k < 8; ++k) b.run(ip, op, 128, Transport{});
    for (std::uint32_t i = 0; i < 128; ++i) CHECK(out[0][i] == 0.0f);
    return 0;
}

// Through the bridge, with the values handed over before the first block,
// the output is the Processor's own render with the same values set before
// prepare(): the property the host tests rely on to compare the bundles
// against the N0 reference renders.
int test_bridged_render_equals_direct_render() {
    const Chans in = ramp_input(4096);
    Toy direct;
    direct.set_param(kGain, 0.3f);
    direct.set_param(kDelay, 7.0f);
    CHECK(direct.prepare(44100.0, 128, 2));
    Chans want(2, std::vector<float>(in[0].size()));
    for (std::size_t pos = 0; pos < in[0].size(); pos += 128) {
        const float* ip[2] = {in[0].data() + pos, in[1].data() + pos};
        float* op[2] = {want[0].data() + pos, want[1].data() + pos};
        direct.process(AudioBuffers{ip, op, nullptr, 2, 0, 128}, nullptr, 0, Transport{});
    }

    Bridge b(std::make_unique<Toy>(), 2);
    b.set_value(kDelay, 7.0f);  // before activate, as a VST3 host restoring state
    CHECK(b.activate(44100.0, 128));
    const Chans got = run_bridge(b, in, 128, [&](std::size_t k) {
        if (k == 0) b.set_value(kGain, 0.3f);  // in the first block, as LV2
    });
    for (int c = 0; c < 2; ++c) {
        for (std::size_t i = 0; i < in[0].size(); ++i) CHECK(got[c][i] == want[c][i]);
    }
    return 0;
}

// Bypass through the bridge is the Processor's own: the input delayed by
// the reported latency, never undelayed (ADR-0011 §1.5).
int test_bypass_keeps_latency() {
    Bridge b(std::make_unique<Toy>(), 2);
    b.set_value(kDelay, 10.0f);
    b.set_value(kBypass, 1.0f);
    CHECK(b.activate(48000.0, 128));
    Chans imp(2, std::vector<float>(128, 0.0f));
    imp[1][0] = 1.0f;
    const Chans out = run_bridge(b, imp, 128);
    CHECK(out[1][10] == 1.0f);
    for (std::uint32_t i = 0; i < 128; ++i) {
        if (i != 10) CHECK(out[1][i] == 0.0f);
    }
    return 0;
}

// DPF activates an instance lazily from inside the host's process call when
// the host never did (DistrhoPluginInternal.hpp, PluginExporter::run;
// DistrhoPluginVST3.cpp, process()), so activate() can run on the audio
// thread. With nothing changed since the last prepare() it must not
// allocate: it applies pending values as jumps and clears the signal
// state, and the instance starts again as a first block (RT audit of
// PR #40, "lazy activate"). The bundles are held to the same through DPF
// by tests/linux/bundle_driver.cpp.
int test_unchanged_activate_does_not_allocate() {
    Bridge b(std::make_unique<Toy>(), 2);
    b.set_value(kDelay, 8.0f);
    CHECK(b.activate(48000.0, 128));
    // An impulse still inside the delay line when the host re-activates: a
    // reset clears it.
    Chans imp(2, std::vector<float>(4, 0.0f));
    imp[0][0] = 1.0f;
    run_bridge(b, imp, 4);
    b.set_value(kGain, 0.5f);

    const Chans in = ramp_input(256);
    Chans out(2, std::vector<float>(256, 7.0f));
    const float* ip[2] = {in[0].data(), in[1].data()};
    float* op[2] = {out[0].data(), out[1].data()};
    g_allocations.store(0);
    g_counting.store(true);
    const bool ok = b.activate(48000.0, 128);
    b.run(ip, op, 128, Transport{});
    const float* ip2[2] = {in[0].data() + 128, in[1].data() + 128};
    float* op2[2] = {out[0].data() + 128, out[1].data() + 128};
    b.run(ip2, op2, 128, Transport{});
    g_counting.store(false);
    CHECK(ok);
    CHECK(g_allocations.load() == 0);
    CHECK(b.latency() == 8);
    // Cleared (nothing of the impulse comes out), and the gain set while
    // inactive is where the instance starts, not a ramp from 1.0.
    for (std::uint32_t i = 0; i < 8; ++i) CHECK(out[0][i] == 0.0f);
    for (std::uint32_t i = 8; i < 256; ++i) CHECK(out[0][i] == 0.5f * in[0][i - 8]);

    // A host that never says how long its blocks are gets the same default
    // every time, so its lazy activation is free as well.
    CHECK(b.activate(48000.0, 0));
    g_allocations.store(0);
    g_counting.store(true);
    const bool again = b.activate(48000.0, 0);
    g_counting.store(false);
    CHECK(again);
    CHECK(g_allocations.load() == 0);
    return 0;
}

// Anything prepare() depends on still prepares again: the rate, the block
// size, and (test_reconfiguring_values_wait_for_activate) a reconfiguring
// value.
int test_changed_activate_prepares() {
    Bridge b(std::make_unique<Toy>(), 2);
    CHECK(b.activate(48000.0, 128));
    g_allocations.store(0);
    g_counting.store(true);
    const bool rate = b.activate(96000.0, 128);
    const long after_rate = g_allocations.load();
    const bool block = b.activate(96000.0, 256);
    const long after_block = g_allocations.load();
    g_counting.store(false);
    CHECK(rate && block);
    CHECK(after_rate > 0);
    CHECK(after_block > after_rate);
    return 0;
}

// The bundle's key input reaches the processor (ADR-0011 N1, the
// compressor's and gate's side-chain): stereo key on a stereo bundle, and a
// mono key on a stereo bundle feeds both channels' detectors.
int test_key_reaches_processor() {
    const Chans in = ramp_input(256);
    Chans key = ramp_input(256);
    for (auto& c : key) {
        for (float& v : c) v = 0.5f * v - 0.2f;
    }
    for (const std::uint32_t key_channels : {2u, 1u}) {
        Bridge b(std::make_unique<KeyToy>(), 2, key_channels);
        CHECK(b.sidechain_channels() == key_channels);
        CHECK(b.activate(48000.0, 128));
        Chans out(2, std::vector<float>(256, 7.0f));
        for (std::uint32_t pos = 0; pos < 256; pos += 128) {
            const float* ip[2] = {in[0].data() + pos, in[1].data() + pos};
            const float* kp[2] = {key[0].data() + pos, key[1].data() + pos};
            float* op[2] = {out[0].data() + pos, out[1].data() + pos};
            b.run(ip, op, kp, 128, Transport{});
        }
        for (std::uint32_t c = 0; c < 2; ++c) {
            const std::uint32_t kc = c < key_channels ? c : key_channels - 1;
            for (std::uint32_t i = 0; i < 256; ++i) CHECK(out[c][i] == key[kc][i]);
        }
    }
    return 0;
}

// A key port the host left unconnected (LV2 allows a host to, DPF hands the
// null through) is no key: the processor keys from its own input, and
// nothing reads through the null. The same for a host that passes no
// side-chain array at all.
int test_unconnected_key_is_no_key() {
    const Chans in = ramp_input(128);
    const Chans key(2, std::vector<float>(128, 0.75f));
    Bridge b(std::make_unique<KeyToy>(), 2, 2);
    CHECK(b.activate(48000.0, 128));
    Chans out(2, std::vector<float>(128, 7.0f));
    const float* ip[2] = {in[0].data(), in[1].data()};
    float* op[2] = {out[0].data(), out[1].data()};
    const float* half[2] = {key[0].data(), nullptr};
    b.run(ip, op, half, 128, Transport{});
    for (std::uint32_t c = 0; c < 2; ++c) {
        for (std::uint32_t i = 0; i < 128; ++i) CHECK(out[c][i] == in[c][i]);
    }
    b.run(ip, op, nullptr, 128, Transport{});
    for (std::uint32_t i = 0; i < 128; ++i) CHECK(out[1][i] == in[1][i]);
    return 0;
}

// A bundle cannot hand a key to a processor that declares none: the
// bundle's side-chain count is ignored for it, so a mistaken build flag
// changes nothing the processor sees.
int test_key_needs_descriptor() {
    Bridge b(std::make_unique<Toy>(), 2, 2);
    CHECK(b.sidechain_channels() == 0);
    return 0;
}

// The mono bundles: one channel in, one out, through the same bridge, equal
// to the Processor's own mono render. Three channels is not a layout any
// N0 processor takes: refused, and silent rather than garbage.
int test_mono_bridge_equals_direct_render() {
    const Chans in2 = ramp_input(1024);
    const Chans in(1, in2[0]);
    Toy direct;
    direct.set_param(kGain, 0.7f);
    direct.set_param(kDelay, 5.0f);
    CHECK(direct.prepare(48000.0, 128, 1));
    std::vector<float> want(1024), got(1024);
    for (std::size_t pos = 0; pos < 1024; pos += 128) {
        const float* ip[1] = {in[0].data() + pos};
        float* op[1] = {want.data() + pos};
        direct.process(AudioBuffers{ip, op, nullptr, 1, 0, 128}, nullptr, 0, Transport{});
    }
    Bridge b(std::make_unique<Toy>(), 1);
    b.set_value(kDelay, 5.0f);
    CHECK(b.activate(48000.0, 128));
    CHECK(b.latency() == 5);
    for (std::size_t pos = 0; pos < 1024; pos += 128) {
        if (pos == 0) b.set_value(kGain, 0.7f);
        const float* ip[1] = {in[0].data() + pos};
        float* op[1] = {got.data() + pos};
        b.run(ip, op, 128, Transport{});
    }
    for (std::size_t i = 0; i < 1024; ++i) CHECK(got[i] == want[i]);

    Bridge three(std::make_unique<Toy>(), 3);
    CHECK(!three.activate(48000.0, 128));
    Chans out(3, std::vector<float>(64, 7.0f));
    const float* ip[3] = {in[0].data(), in[0].data(), in[0].data()};
    float* op[3] = {out[0].data(), out[1].data(), out[2].data()};
    three.run(ip, op, 64, Transport{});
    for (const auto& c : out) {
        for (float v : c) CHECK(v == 0.0f);
    }
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += test_first_block_values_jump();
    failures += test_later_changes_are_smoothed();
    failures += test_reconfiguring_values_wait_for_activate();
    failures += test_inactive_is_silent();
    failures += test_values_are_sanitised();
    failures += test_values_from_another_thread();
    failures += test_bridged_render_equals_direct_render();
    failures += test_bypass_keeps_latency();
    failures += test_unchanged_activate_does_not_allocate();
    failures += test_changed_activate_prepares();
    failures += test_key_reaches_processor();
    failures += test_unconnected_key_is_no_key();
    failures += test_key_needs_descriptor();
    failures += test_mono_bridge_equals_direct_render();
    if (failures == 0) std::printf("native_format: all cases pass\n");
    return failures == 0 ? 0 : 1;
}
