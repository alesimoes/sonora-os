// SPDX-License-Identifier: GPL-3.0-or-later
//
// Record, then play back, end to end against drivers/fake (ADR-0007 §7,
// ADR-0009 §4.6): track 1 records the fake's channel-0 tone for a second;
// the take joins the document where it was recorded; a locate to its start
// and a play go through the residency gate; and the fake's captured
// outputs must be the recorded tone at the -3 dB centre gain on both sides
// of the monitor bus, from the first frame rolled, with every other block
// written and no chunk missing.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "sonora/fake_backend.h"
#include "sonora/playback_controller.h"
#include "sonora/session.h"
#include "sonora/take_document.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

namespace fs = std::filesystem;
using sonora::FakeBackend;

constexpr uint32_t kRate = 48000;
constexpr uint32_t kQuantum = 128;

std::size_t drain(FakeBackend& audio, uint32_t index, std::vector<float>* into = nullptr) {
    float buf[4096];
    std::size_t total = 0;
    for (;;) {
        const std::size_t n = audio.read_output(index, buf, 4096);
        if (into) into->insert(into->end(), buf, buf + n);
        total += n;
        if (n < 4096) return total;
    }
}

int test_record_then_play_back() {
    const fs::path dir = fs::temp_directory_path() / "sonora_test_playback_fake";
    fs::remove_all(dir);
    sonora::SessionConfig cfg;
    cfg.dir = dir.string();
    cfg.track_count = 2;
    cfg.sample_rate = kRate;
    cfg.format = sonora::WavWriter::Format::Float32;
    cfg.ring_seconds = 2.0;
    cfg.playback.chunk_frames = 4096;
    cfg.playback.window_seconds = 0.1;
    cfg.playback.max_block_frames = 256;
    sonora::Session s(cfg);
    CHECK(s.create_dirs());

    FakeBackend::Config fc;
    fc.quantum = kQuantum;
    fc.output_capture_frames = 4 * kRate;
    FakeBackend audio(fc);
    std::string error;
    CHECK(audio.open({"sonora", 2, 2, kRate, ""}, s, error));
    CHECK(audio.activate(error));
    CHECK(audio.connect_inputs_in_order() == 2);

    // One second of channel 0's tone on track 1, from server frame 0.
    s.disk().start();
    s.tracks()[0]->set_rec_armed(true);
    CHECK(s.start_recording(1, &error));
    constexpr uint32_t kBlocks = kRate / kQuantum;
    CHECK(audio.run_blocks(kBlocks) == kBlocks);
    CHECK(s.stop(&error));
    CHECK(s.tracks()[0]->writer().frames_written() == uint64_t{kBlocks} * kQuantum);

    sonora::TakeDocument doc(s);
    CHECK(doc.add_closed_take(s, error));
    s.tracks()[0]->set_rec_armed(false);
    sonora::PlaybackController playback(s, cfg.dir);
    CHECK(playback.show(doc.current(), error));
    CHECK(playback.locate(0, error));
    playback.play();

    // The callback keeps running while the gate waits; once the roll
    // starts, the next block is the first rolled.
    for (int i = 0; i < 100 && s.transport().state() != sonora::TransportState::Rolling; ++i) {
        s.reader().run_pass();
        playback.pump();
        if (s.transport().state() == sonora::TransportState::Rolling) break;
        CHECK(audio.run_blocks(1) == 1);
    }
    CHECK(s.transport().state() == sonora::TransportState::Rolling);
    CHECK(s.transport().playhead() == 0);
    drain(audio, 0);
    drain(audio, 1);

    // The whole take and a little past its end, the reader running between
    // blocks as its thread would.
    constexpr uint32_t kPlayBlocks = kBlocks + 20;
    for (uint32_t b = 0; b < kPlayBlocks; ++b) {
        s.reader().run_pass();
        playback.pump();
        CHECK(audio.run_blocks(1) == 1);
    }
    std::vector<float> left, right;
    drain(audio, 0, &left);
    drain(audio, 1, &right);
    CHECK(left.size() == uint64_t{kPlayBlocks} * kQuantum);
    CHECK(right.size() == left.size());

    const float gain = std::pow(10.0f, -3.0f / 20.0f);
    const uint64_t take = uint64_t{kBlocks} * kQuantum;
    double worst = 0.0;
    for (uint64_t t = 0; t < left.size(); ++t) {
        const float want = t < take ? FakeBackend::tone(0, t, kRate) * gain : 0.0f;
        worst = std::max(worst, static_cast<double>(std::fabs(left[t] - want)));
        worst = std::max(worst, static_cast<double>(std::fabs(right[t] - want)));
    }
    std::printf("playback through drivers/fake: %llu frames, worst residual %.3g\n",
                static_cast<unsigned long long>(left.size()), worst);
    CHECK(worst < 1e-6);
    CHECK(s.playback_misses() == 0);
    CHECK(audio.unwritten_output_periods() == 0);
    CHECK(audio.output_dropped() == 0);
    CHECK(audio.xruns() == 0);

    audio.deactivate();
    playback.after_deactivate();
    fs::remove_all(dir);
    return 0;
}

}  // namespace

int main() {
    if (test_record_then_play_back()) return 1;
    std::printf("playback_fake: ok\n");
    return 0;
}
