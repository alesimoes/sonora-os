// SPDX-License-Identifier: GPL-3.0-or-later
//
// What Session::process() does with one block: which track each input lands
// on, what is metered, what is captured, when an overrun is counted and when
// the playhead moves. ADR-0007 M2 changed how the backend reaches this
// function. Every check not marked "added with ProcessCallback" was written
// against the call as it stood before and passed on it, so the change could
// not alter any of that unnoticed.
#include <cstdio>
#include <vector>

#include "sonora/session.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::Session;
using sonora::TransportState;

sonora::SessionConfig config(int tracks, double ring_seconds = 1.0) {
    sonora::SessionConfig cfg;
    // process() never touches the folder; nothing is created there.
    cfg.dir = "test_session_process_unused";
    cfg.track_count = tracks;
    cfg.ring_seconds = ring_seconds;
    return cfg;
}

// The one place that knows how a block reaches the session. Called through
// the interface, as a backend calls it, with one input per track.
void feed(Session& s, const float* const* inputs, uint32_t nframes) {
    sonora::backend::AudioBlock block{};
    block.inputs = inputs;
    block.input_count = static_cast<uint32_t>(s.tracks().size());
    block.nframes = nframes;
    sonora::backend::ProcessCallback& cb = s;
    cb.process(block);
}

std::vector<float> drain(sonora::Track& t) {
    std::vector<float> out(t.capture_ring().read_space());
    t.capture_ring().read(out.data(), out.size());
    return out;
}

// Input i, frame f. Distinct per channel and per frame, so a crossed channel
// or a dropped or repeated frame shows up as a wrong value, not a near miss.
float sample(std::size_t channel, uint32_t frame) {
    return static_cast<float>(channel + 1) + static_cast<float>(frame) * 1e-3f;
}

int test_channel_identity() {
    constexpr int kTracks = 18;  // the UMC1820
    constexpr uint32_t kFrames = 64;
    Session s(config(kTracks));
    for (const auto& t : s.tracks()) t->set_rec_armed(true);
    s.transport().set_state(TransportState::Recording);

    std::vector<std::vector<float>> bufs(kTracks, std::vector<float>(kFrames));
    std::vector<const float*> inputs(kTracks);
    for (std::size_t c = 0; c < kTracks; ++c) {
        for (uint32_t f = 0; f < kFrames; ++f) bufs[c][f] = sample(c, f);
        inputs[c] = bufs[c].data();
    }
    feed(s, inputs.data(), kFrames);
    feed(s, inputs.data(), kFrames);

    for (std::size_t c = 0; c < kTracks; ++c) {
        sonora::Track& t = *s.tracks()[c];
        const std::vector<float> got = drain(t);
        CHECK(got.size() == 2 * kFrames);
        for (uint32_t f = 0; f < 2 * kFrames; ++f) {
            // Against what was fed, not sample() evaluated again: capture is a
            // copy, and the GCC 15 CI build failed here comparing with a
            // second evaluation. Hex, so a last-bit difference shows.
            if (got[f] != bufs[c][f % kFrames]) {
                std::fprintf(stderr, "FAIL track %zu frame %u: %a, fed %a\n", c + 1, f,
                             static_cast<double>(got[f]),
                             static_cast<double>(bufs[c][f % kFrames]));
                return 1;
            }
        }
        CHECK(t.take_peak() == bufs[c][kFrames - 1]);
        CHECK(t.overruns() == 0);
    }
    CHECK(s.transport().playhead() == 2 * kFrames);
    return 0;
}

// The operator sees signal before pressing record, so metering does not
// depend on arming or on the transport.
int test_meters_while_stopped() {
    Session s(config(2));
    const float a[4] = {0.1f, -0.7f, 0.3f, 0.0f};
    const float b[4] = {0.0f, 0.0f, 0.0f, 0.25f};
    const float* inputs[2] = {a, b};
    feed(s, inputs, 4);

    CHECK(s.tracks()[0]->take_peak() == 0.7f);
    CHECK(s.tracks()[1]->take_peak() == 0.25f);
    CHECK(s.tracks()[0]->capture_ring().read_space() == 0);
    CHECK(s.tracks()[1]->capture_ring().read_space() == 0);
    CHECK(s.transport().playhead() == 0);
    return 0;
}

int test_captures_only_armed_tracks_while_recording() {
    Session s(config(2));
    const float a[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    const float* inputs[2] = {a, a};

    s.tracks()[0]->set_rec_armed(true);
    feed(s, inputs, 8);  // armed, but the transport is stopped
    CHECK(s.tracks()[0]->capture_ring().read_space() == 0);

    s.transport().set_state(TransportState::Recording);
    feed(s, inputs, 8);
    CHECK(s.tracks()[0]->capture_ring().read_space() == 8);
    CHECK(s.tracks()[1]->capture_ring().read_space() == 0);
    CHECK(s.tracks()[1]->take_peak() == 0.5f);  // metered all the same
    return 0;
}

int test_unconnected_input_is_skipped() {
    Session s(config(3));
    for (const auto& t : s.tracks()) t->set_rec_armed(true);
    s.transport().set_state(TransportState::Recording);
    const float a[4] = {0.2f, 0.2f, 0.2f, 0.2f};
    const float* inputs[3] = {a, nullptr, a};
    feed(s, inputs, 4);

    CHECK(s.tracks()[0]->capture_ring().read_space() == 4);
    CHECK(s.tracks()[1]->capture_ring().read_space() == 0);
    CHECK(s.tracks()[1]->take_peak() == 0.0f);
    CHECK(s.tracks()[2]->capture_ring().read_space() == 4);
    CHECK(s.transport().playhead() == 4);
    return 0;
}

// A full ring is counted once per short block and never waited on.
int test_overrun_counts_short_blocks() {
    // 1 ms at 48 kHz is 48 frames, rounded up to a 64-slot ring that holds 63.
    Session s(config(1, 0.001));
    sonora::Track& t = *s.tracks()[0];
    t.set_rec_armed(true);
    s.transport().set_state(TransportState::Recording);
    std::vector<float> a(64, 0.1f);
    const float* inputs[1] = {a.data()};

    feed(s, inputs, 32);
    CHECK(t.overruns() == 0);
    feed(s, inputs, 64);
    CHECK(t.overruns() == 1);
    CHECK(t.capture_ring().read_space() == 63);
    feed(s, inputs, 64);
    CHECK(t.overruns() == 2);
    CHECK(s.disk().total_overruns() == 2);
    // The playhead keeps time even when the disk cannot keep up.
    CHECK(s.transport().playhead() == 32 + 64 + 64);
    return 0;
}

int test_playhead_moves_unless_stopped() {
    Session s(config(1));
    const float a[16] = {};
    const float* inputs[1] = {a};

    feed(s, inputs, 16);
    CHECK(s.transport().playhead() == 0);
    s.transport().set_state(TransportState::Rolling);
    feed(s, inputs, 16);
    CHECK(s.transport().playhead() == 16);
    s.transport().set_state(TransportState::Recording);
    feed(s, inputs, 16);
    CHECK(s.transport().playhead() == 32);
    return 0;
}

// Added with ProcessCallback: a backend may offer fewer inputs than the
// session has tracks, and the tracks past the end are unconnected.
int test_fewer_inputs_than_tracks() {
    Session s(config(4));
    for (const auto& t : s.tracks()) t->set_rec_armed(true);
    s.transport().set_state(TransportState::Recording);
    const float a[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    const float* inputs[2] = {a, a};
    sonora::backend::AudioBlock block{};
    block.inputs = inputs;
    block.input_count = 2;
    block.nframes = 8;
    s.process(block);

    CHECK(s.tracks()[1]->capture_ring().read_space() == 8);
    CHECK(s.tracks()[2]->capture_ring().read_space() == 0);
    CHECK(s.tracks()[3]->take_peak() == 0.0f);
    CHECK(s.transport().playhead() == 8);
    return 0;
}

// Added with ProcessCallback: nothing is played back yet, and a backend
// buffer holds whatever its last cycle left in it, so every output comes
// back silent.
int test_outputs_are_silenced() {
    Session s(config(1));
    float out_a[16], out_b[16];
    for (int i = 0; i < 16; ++i) out_a[i] = out_b[i] = 0.9f;
    float* outputs[2] = {out_a, out_b};
    const float in[16] = {};
    const float* inputs[1] = {in};
    sonora::backend::AudioBlock block{};
    block.inputs = inputs;
    block.input_count = 1;
    block.outputs = outputs;
    block.output_count = 2;
    block.nframes = 16;
    s.process(block);

    for (int i = 0; i < 16; ++i) {
        CHECK(out_a[i] == 0.0f);
        CHECK(out_b[i] == 0.0f);
    }
    return 0;
}

}  // namespace

int main() {
    if (test_channel_identity() != 0) return 1;
    if (test_meters_while_stopped() != 0) return 1;
    if (test_captures_only_armed_tracks_while_recording() != 0) return 1;
    if (test_unconnected_input_is_skipped() != 0) return 1;
    if (test_overrun_counts_short_blocks() != 0) return 1;
    if (test_playhead_moves_unless_stopped() != 0) return 1;
    if (test_fewer_inputs_than_tracks() != 0) return 1;
    if (test_outputs_are_silenced() != 0) return 1;
    std::printf("test_session_process: ok\n");
    return 0;
}
