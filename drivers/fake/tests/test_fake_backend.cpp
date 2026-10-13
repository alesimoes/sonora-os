// SPDX-License-Identifier: GPL-3.0-or-later
//
// What drivers/fake does beyond the contract, which the conformance suite
// covers: the shapes it presents, the signal on each port, and each
// injection doing what the failure it stands for does.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "sonora/fake_backend.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::FakeBackend;
using sonora::backend::AudioBlock;
using sonora::backend::PortInfo;

// Keeps every input sample it is given, per input, and the frame_time of
// each block. Test only: it allocates in process(), which a real callback
// must never do, but the fake runs it on this thread.
class Capture final : public sonora::backend::ProcessCallback {
public:
    explicit Capture(uint32_t inputs) : samples(inputs), was_null(inputs, false) {}

    void process(const AudioBlock& b) noexcept override {
        times.push_back(b.frame_time);
        for (uint32_t i = 0; i < b.input_count; ++i) {
            if (b.inputs[i] == nullptr) {
                was_null[i] = true;
                samples[i].insert(samples[i].end(), b.nframes, 0.0f);
            } else {
                samples[i].insert(samples[i].end(), b.inputs[i], b.inputs[i] + b.nframes);
            }
        }
        for (uint32_t o = 0; o < b.output_count; ++o) {
            for (uint32_t f = 0; f < b.nframes; ++f) {
                b.outputs[o][f] = write_outputs ? static_cast<float>(next_out++) : 0.0f;
            }
            if (skip_output_writes) break;
        }
    }

    std::vector<std::vector<float>> samples;
    std::vector<bool> was_null;
    std::vector<uint64_t> times;
    bool write_outputs = false;
    bool skip_output_writes = false;
    uint64_t next_out = 0;
};

bool start(FakeBackend& f, Capture& cap, uint32_t inputs, uint32_t outputs,
           const std::string& device = "") {
    std::string error;
    return f.open({"test", inputs, outputs, 48000, device}, cap, error) && f.activate(error);
}

// Input i carries channel `channels[i]`'s tone, frame by frame from the
// block's frame_time.
bool carries_tones(const Capture& cap, const std::vector<uint32_t>& channels,
                   uint64_t first_frame) {
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const std::vector<float>& s = cap.samples[i];
        if (s.empty()) return false;
        for (std::size_t n = 0; n < s.size(); ++n) {
            if (s[n] != FakeBackend::tone(channels[i], first_frame + n, 48000)) return false;
        }
    }
    return true;
}

int test_umc1820_shape() {
    FakeBackend f;
    const std::vector<PortInfo> in = f.physical_capture_ports();
    const std::vector<PortInfo> out = f.physical_playback_ports();
    CHECK(in.size() == 18);
    CHECK(out.size() == 20);
    for (uint32_t c = 0; c < 18; ++c) {
        CHECK(in[c].device == "UMC1820 Pro");
        CHECK(in[c].name == "UMC1820 Pro:capture_AUX" + std::to_string(c));
        CHECK(in[c].channel == c);
        CHECK(in[c].usb);
    }
    CHECK(out[19].name == "UMC1820 Pro:playback_AUX19");
    return 0;
}

int test_onboard_shape() {
    FakeBackend f(FakeBackend::onboard_config());
    const std::vector<PortInfo> in = f.physical_capture_ports();
    CHECK(in.size() == 2);
    CHECK(in[0].device == "Built-in Audio Pro");
    CHECK(!in[0].usb);
    return 0;
}

// The tones are the ones tools/verify-recording.py looks for, so a take
// recorded through the fake verifies there too.
int test_tones_match_the_verifier() {
    CHECK(FakeBackend::tone_hz(0) == 250.0);
    CHECK(FakeBackend::tone_hz(17) == 250.0 + 317.0 * 17);
    // 250 Hz at 48 kHz: a quarter period is 48 frames.
    CHECK(std::fabs(FakeBackend::tone(0, 48, 48000) - 0.5f) < 1e-6f);
    CHECK(FakeBackend::tone(0, 0, 48000) == 0.0f);
    return 0;
}

int test_each_input_carries_its_channel() {
    FakeBackend f;
    Capture cap(18);
    CHECK(start(f, cap, 18, 0));
    CHECK(f.connect_inputs_in_order() == 18);
    CHECK(f.run_blocks(10) == 10);
    std::vector<uint32_t> channels(18);
    for (uint32_t c = 0; c < 18; ++c) channels[c] = c;
    CHECK(carries_tones(cap, channels, 0));
    CHECK(cap.times.front() == 0);
    CHECK(cap.times.back() == 9 * 128);
    return 0;
}

// Pro Audio's AUX names and the numbered ones both sort into channel order,
// however the server lists them and whatever ids it gives them. The
// identity check is on the signal: port capture_10 carries channel 9's tone.
int test_numbered_lexicographic_scrambled_ids() {
    FakeBackend::Config cfg;
    cfg.naming = FakeBackend::Naming::Numbered;
    cfg.list_order = FakeBackend::ListOrder::Lexicographic;
    cfg.ids_out_of_order = true;
    FakeBackend f(cfg);
    const std::vector<PortInfo> in = f.physical_capture_ports();
    CHECK(in.size() == 18);
    for (uint32_t c = 0; c < 18; ++c) {
        CHECK(in[c].name == "UMC1820 Pro:capture_" + std::to_string(c + 1));
        CHECK(in[c].channel == c);
    }
    // The ids really are out of channel order.
    bool ascending = true;
    for (uint32_t c = 1; c < 18; ++c) {
        if (f.port_id(in[c].name) < f.port_id(in[c - 1].name)) ascending = false;
    }
    CHECK(!ascending);

    Capture cap(18);
    CHECK(start(f, cap, 18, 0));
    CHECK(f.connect_inputs_in_order() == 18);
    CHECK(f.run_blocks(4) == 4);
    std::vector<uint32_t> channels(18);
    for (uint32_t c = 0; c < 18; ++c) channels[c] = c;
    CHECK(carries_tones(cap, channels, 0));
    return 0;
}

int test_reversed_listing() {
    FakeBackend::Config cfg;
    cfg.list_order = FakeBackend::ListOrder::Reversed;
    FakeBackend f(cfg);
    const std::vector<PortInfo> in = f.physical_capture_ports();
    CHECK(in.front().name == "UMC1820 Pro:capture_AUX0");
    CHECK(in.back().name == "UMC1820 Pro:capture_AUX17");
    return 0;
}

int test_silence_and_impulses() {
    {
        FakeBackend::Config cfg;
        cfg.signal = FakeBackend::Signal::Silence;
        FakeBackend f(cfg);
        Capture cap(18);
        CHECK(start(f, cap, 18, 0));
        CHECK(f.connect_inputs_in_order() == 18);
        f.run_blocks(4);
        for (const auto& s : cap.samples) {
            CHECK(std::all_of(s.begin(), s.end(), [](float x) { return x == 0.0f; }));
        }
    }
    FakeBackend::Config cfg;
    cfg.signal = FakeBackend::Signal::Impulses;
    cfg.impulse_interval = 300;
    FakeBackend f(cfg);
    Capture cap(18);
    CHECK(start(f, cap, 18, 0));
    CHECK(f.connect_inputs_in_order() == 18);
    f.run_blocks(10);
    // Every channel's impulses at the same frames: aligned tracks.
    for (const auto& s : cap.samples) {
        CHECK(s.size() == 1280);
        for (std::size_t n = 0; n < s.size(); ++n) {
            CHECK(s[n] == (n % 300 == 0 ? 1.0f : 0.0f));
        }
    }
    return 0;
}

int test_unconnected_inputs_are_null_by_default() {
    FakeBackend f;
    Capture cap(20);
    CHECK(start(f, cap, 20, 0));
    CHECK(f.connect_inputs_in_order() == 18);
    CHECK(f.last_warning().find("18 capture channels for 20 tracks") == 0);
    f.run_blocks(1);
    CHECK(!cap.was_null[17]);
    CHECK(cap.was_null[18]);
    CHECK(cap.was_null[19]);
    return 0;
}

int test_outputs_are_captured() {
    FakeBackend::Config cfg;
    cfg.output_capture_frames = 4096;
    FakeBackend f(cfg);
    Capture cap(0);
    cap.write_outputs = true;
    CHECK(start(f, cap, 0, 2));
    f.run_blocks(2);
    std::vector<float> got(512, -1.0f);
    // Output 0 got 0..127 then 256..383; output 1 the frames between.
    CHECK(f.read_output(0, got.data(), got.size()) == 256);
    CHECK(got[0] == 0.0f);
    CHECK(got[127] == 127.0f);
    CHECK(got[128] == 256.0f);
    CHECK(f.read_output(1, got.data(), got.size()) == 256);
    CHECK(got[0] == 128.0f);
    CHECK(f.unwritten_output_periods() == 0);
    CHECK(f.output_dropped() == 0);
    return 0;
}

// The contract has the callback write every output; the fake notices one
// that does not.
int test_unwritten_outputs_are_counted() {
    FakeBackend f;
    Capture cap(0);
    cap.skip_output_writes = true;
    CHECK(start(f, cap, 0, 3));
    f.run_blocks(5);
    CHECK(f.unwritten_output_periods() == 5);
    return 0;
}

// A USB interface enumerating after boot: absent at first, nothing chosen
// while waiting, then connected in channel order once it is there.
int test_late_enumeration() {
    FakeBackend::Config cfg;
    cfg.devices.front().appears_after_ns = 3000000000ull;
    FakeBackend f(cfg);
    CHECK(f.physical_capture_ports().empty());
    Capture cap(18);
    CHECK(start(f, cap, 18, 0));
    CHECK(f.connect_inputs_in_order() == 0);
    CHECK(f.capture_device().empty());
    f.run_blocks(375 * 4);  // 4 s at 128 frames, 48 kHz
    CHECK(f.physical_capture_ports().size() == 18);
    CHECK(f.connect_inputs_in_order() == 18);
    CHECK(f.capture_device() == "UMC1820 Pro");
    CHECK(!f.fatal());
    return 0;
}

int test_patch_by_hand() {
    FakeBackend f;
    Capture cap(2);
    CHECK(start(f, cap, 2, 0));
    CHECK(f.patch(0, "UMC1820 Pro:capture_AUX5"));
    CHECK(!f.patch(1, "UMC1820 Pro:capture_AUX99"));
    CHECK(!f.patch(5, "UMC1820 Pro:capture_AUX0"));
    CHECK(f.connected_input_count() == 1);
    f.run_blocks(2);
    for (std::size_t n = 0; n < cap.samples[0].size(); ++n) {
        CHECK(cap.samples[0][n] == FakeBackend::tone(5, n, 48000));
    }
    f.unpatch(0);
    CHECK(f.connected_input_count() == 0);
    return 0;
}

// With the ids scrambled, the port that goes is still found by id and name,
// and the reason names it.
int test_unplug_names_the_device() {
    FakeBackend::Config cfg;
    cfg.ids_out_of_order = true;
    FakeBackend f(cfg);
    Capture cap(18);
    CHECK(start(f, cap, 18, 0));
    CHECK(f.connect_inputs_in_order() == 18);
    f.run_blocks(4);
    CHECK(f.unplug("UMC1820 Pro"));
    CHECK(f.fatal());
    CHECK(f.fatal_reason().find("capture device UMC1820 Pro went away (UMC1820 Pro:capture_AUX") ==
          0);
    CHECK(f.connected_input_count() == 0);
    CHECK(f.physical_capture_ports().empty());
    const uint64_t calls = f.process_calls();
    f.run_blocks(4);
    CHECK(f.process_calls() == calls);
    return 0;
}

// Unpatched more than a second before it goes, an interface going is not
// the take's device going (ADR-0007 M2 note, item 9).
int test_unplug_after_unpatch_is_not_fatal() {
    FakeBackend f;
    Capture cap(2);
    CHECK(start(f, cap, 2, 0, "UMC1820 Pro"));
    CHECK(f.connect_inputs_in_order() == 2);
    f.unpatch(0);
    f.unpatch(1);
    f.run_blocks(500);  // over a second
    CHECK(f.unplug("UMC1820 Pro"));
    f.run_blocks(1);
    CHECK(!f.fatal());
    return 0;
}

// A device no input is fed from is not the take's device either.
int test_unplug_of_another_device_is_not_fatal() {
    FakeBackend::Config cfg;
    cfg.devices = {FakeBackend::umc1820(), FakeBackend::onboard()};
    FakeBackend f(cfg);
    Capture cap(18);
    CHECK(start(f, cap, 18, 0));
    CHECK(f.connect_inputs_in_order() == 18);
    CHECK(f.unplug("Built-in Audio Pro"));
    CHECK(f.run_blocks(2) == 2);
    CHECK(!f.fatal());
    CHECK(f.physical_capture_ports().size() == 18);
    return 0;
}

// The server's counter wraps at 2^32; frame_time carries on past it.
int test_frame_time_past_the_wrap() {
    FakeBackend::Config cfg;
    cfg.first_frame = 0xFFFFFFFFu - 200;
    FakeBackend f(cfg);
    Capture cap(1);
    CHECK(start(f, cap, 1, 0));
    CHECK(f.connect_inputs_in_order() == 1);
    CHECK(f.run_blocks(4) == 4);
    CHECK(cap.times[0] == 0xFFFFFFFFull - 200);
    CHECK(cap.times[3] == 0xFFFFFFFFull - 200 + 3 * 128);
    CHECK(f.clock_discontinuities() == 0);
    // The signal follows the frame, not the block count.
    CHECK(carries_tones(cap, {0}, 0xFFFFFFFFull - 200));
    return 0;
}

int test_xrun_moves_frame_time() {
    FakeBackend f;
    Capture cap(1);
    CHECK(start(f, cap, 1, 0));
    CHECK(f.connect_inputs_in_order() == 1);
    f.run_blocks(2);
    f.inject_xrun(1000);
    f.run_blocks(1);
    CHECK(f.xruns() == 1);
    CHECK(cap.times[2] == 2 * 128 + 1000);
    // What the server lost is not in the input either.
    CHECK(cap.samples[0][256] == FakeBackend::tone(0, 256 + 1000, 48000));
    CHECK(!f.fatal());
    return 0;
}

int test_clock_step_back_holds_frame_time() {
    FakeBackend f;
    Capture cap(1);
    CHECK(start(f, cap, 1, 0));
    f.run_blocks(3);
    f.inject_clock_step_back(1000);
    f.run_blocks(2);
    CHECK(f.clock_discontinuities() == 1);
    CHECK(cap.times[3] == cap.times[2]);
    CHECK(cap.times[4] == cap.times[2] + 128);
    return 0;
}

// A period the server skips and does not report, as PipeWire's dummy driver
// does when woken late: counted as a clock step, not an xrun, and the gap
// stays in frame_time.
int test_unreported_skip_is_a_clock_step() {
    FakeBackend f;
    Capture cap(1);
    CHECK(start(f, cap, 1, 0));
    f.run_blocks(2);
    f.inject_unreported_skip(128);
    f.run_blocks(2);
    CHECK(f.xruns() == 0);
    CHECK(f.clock_discontinuities() == 1);
    CHECK(cap.times[2] == 3 * 128);
    CHECK(cap.times[3] == 4 * 128);
    return 0;
}

// The server's notification of a new rate, as JACK's callback gives it,
// is fatal before any period at that rate runs.
int test_rate_change_notified() {
    FakeBackend f;
    Capture cap(1);
    CHECK(start(f, cap, 1, 0));
    f.run_blocks(2);
    f.inject_rate_change(44100, true);
    CHECK(f.fatal());
    CHECK(f.fatal_reason() ==
          "the audio server changed its sample rate mid-session (48000 -> 44100 Hz)");
    CHECK(f.run_blocks(2) == 0);
    return 0;
}

int test_server_loss_takes_the_ports() {
    FakeBackend f;
    Capture cap(1);
    CHECK(start(f, cap, 1, 0));
    f.inject_server_loss();
    CHECK(f.fatal());
    CHECK(f.fatal_reason() == "the audio server went away");
    CHECK(f.physical_capture_ports().empty());
    CHECK(f.run_blocks(3) == 0);
    CHECK(f.periods() == 0);
    FakeBackend g;
    g.inject_server_loss();
    std::string error;
    CHECK(!g.open({"test", 1, 0, 48000, ""}, cap, error));
    return 0;
}

int test_stall_is_seen_after_a_second() {
    FakeBackend f;
    Capture cap(1);
    CHECK(start(f, cap, 1, 0));
    f.run_blocks(2);
    f.set_stalled(true);
    f.run_blocks(200);  // about half a second, no period
    CHECK(!f.fatal());
    f.run_blocks(200);
    CHECK(!f.fatal() || f.fatal_reason() == "the audio server ran no period for over a second");
    f.run_blocks(400);
    CHECK(f.fatal());
    CHECK(f.fatal_reason() == "the audio server ran no period for over a second");
    return 0;
}

// The thread drive: periods arrive on their own, at the period's rate, and
// deactivate() stops them.
int test_thread_mode() {
    FakeBackend::Config cfg;
    cfg.mode = FakeBackend::Mode::Thread;
    FakeBackend f(cfg);
    Capture cap(18);
    CHECK(start(f, cap, 18, 2));
    CHECK(f.connect_inputs_in_order() == 18);
    CHECK(f.run_blocks(5) == 0);  // not this mode's drive
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (f.process_calls() < 20 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(f.process_calls() >= 20);
    f.deactivate();
    const uint64_t calls = f.process_calls();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(f.process_calls() == calls);
    CHECK(!f.fatal());
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_umc1820_shape();
    failed += test_onboard_shape();
    failed += test_tones_match_the_verifier();
    failed += test_each_input_carries_its_channel();
    failed += test_numbered_lexicographic_scrambled_ids();
    failed += test_reversed_listing();
    failed += test_silence_and_impulses();
    failed += test_unconnected_inputs_are_null_by_default();
    failed += test_outputs_are_captured();
    failed += test_unwritten_outputs_are_counted();
    failed += test_late_enumeration();
    failed += test_patch_by_hand();
    failed += test_unplug_names_the_device();
    failed += test_unplug_after_unpatch_is_not_fatal();
    failed += test_unplug_of_another_device_is_not_fatal();
    failed += test_frame_time_past_the_wrap();
    failed += test_xrun_moves_frame_time();
    failed += test_clock_step_back_holds_frame_time();
    failed += test_unreported_skip_is_a_clock_step();
    failed += test_rate_change_notified();
    failed += test_server_loss_takes_the_ports();
    failed += test_stall_is_seen_after_a_second();
    failed += test_thread_mode();
    if (failed == 0) std::printf("fake_backend: all tests passed\n");
    return failed == 0 ? 0 : 1;
}
