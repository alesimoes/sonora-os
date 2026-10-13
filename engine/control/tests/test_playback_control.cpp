// SPDX-License-Identifier: GPL-3.0-or-later
//
// The plan builder and the publisher (ADR-0009 §4; timeline-model.md):
// a model snapshot becomes a plan with the right segments, fades,
// crossfades, chunk slots and graph, or is refused naming the limit; and
// the controller publishes only resident plans, so an undo while rolling
// switches what is heard at a block boundary with no missing chunk, a
// locate plays its target from the next block, and a superseded pending
// plan is dropped.
#include <sys/stat.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "sonora/model/history.h"
#include "sonora/model/operations.h"
#include "sonora/playback_controller.h"
#include "sonora/session.h"
#include "sonora/session_commands.h"
#include "sonora/wav_writer.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

#define CHECK_APPLY(history, op)                                              \
    do {                                                                      \
        std::string check_apply_error_;                                       \
        if (!(history).apply((op), check_apply_error_)) {                     \
            std::fprintf(stderr, "FAIL %s:%d: apply refused: %s\n", __FILE__, \
                         __LINE__, check_apply_error_.c_str());               \
            return 1;                                                         \
        }                                                                     \
    } while (0)

namespace {

using namespace sonora::model;
using sonora::PlaybackController;
using sonora::PlaybackPlan;
using sonora::Session;

constexpr uint32_t kChunk = 256;
constexpr uint32_t kBlock = 64;
const std::string kDir = "test_playback_control_session";

float value(Id source, int64_t f) {
    return static_cast<float>(source) + static_cast<float>(f % 65536) / 65536.0f;
}

bool write_source(Id id, int64_t frames) {
    const std::string path = kDir + "/Audio Files/src_" + std::to_string(id) + ".wav";
    std::remove(path.c_str());
    sonora::WavWriter w;
    if (!w.open(path, 48000, 1, sonora::WavWriter::Format::Float32, 0)) return false;
    std::vector<float> buf(static_cast<std::size_t>(frames));
    for (int64_t f = 0; f < frames; ++f) buf[static_cast<std::size_t>(f)] = value(id, f);
    return w.write(buf.data(), buf.size()) && w.close();
}

AddSource source_op(Id id, int64_t frames) {
    AddSource op;
    op.source.id = id;
    op.source.path = "Audio Files/src_" + std::to_string(id) + ".wav";
    op.source.length = frames;
    op.source.format = AudioFormat::Float32;
    op.source.kind = SourceKind::Imported;
    return op;
}

AddTrack track_op(Id track, Id playlist, const std::string& name) {
    AddTrack op;
    op.track = track;
    op.playlist = playlist;
    op.name = name;
    op.playlist_name = name + ".01";
    op.mix.input.push_back(0);
    op.mix.pan = -1.0f;  // hard left: the left output is the track, exactly
    return op;
}

Region region(Id id, Id source, Frames position, Frames length, Frames offset) {
    Region r;
    r.id = id;
    r.name = "r" + std::to_string(id);
    r.position = position;
    r.length = length;
    r.source_offset = offset;
    r.channels.push_back({source, 0});
    return r;
}

std::unique_ptr<Session> make_session(int tracks) {
    sonora::SessionConfig cfg;
    cfg.dir = kDir;
    cfg.track_count = tracks;
    cfg.ring_seconds = 0.1;
    cfg.playback.chunk_frames = kChunk;
    cfg.playback.window_seconds = 0.02;
    cfg.playback.pool_chunks = 128;
    cfg.playback.max_block_frames = 256;
    cfg.playback.max_segments = 64;
    auto s = std::make_unique<Session>(cfg);
    s->create_dirs();
    return s;
}

struct Out {
    std::vector<float> l = std::vector<float>(kBlock);
    std::vector<float> r = std::vector<float>(kBlock);
};

void run_block(Session& s, Out& o) {
    float* outs[2] = {o.l.data(), o.r.data()};
    sonora::backend::AudioBlock b{};
    b.outputs = outs;
    b.output_count = 2;
    b.nframes = kBlock;
    s.process(b);
}

// Track 1: region 210 at 1000 for 3000 frames of source 1 from 500, fade in
// 100 linear, fade out 200 equal power; region 211 butting it at 4000 for
// 2000 frames of source 2 from 1000, a centred 128-frame s-curve crossfade
// between them. Track 2: region 212, muted.
int test_build_from_model() {
    auto session = make_session(3);
    CHECK(write_source(1, 8000));
    CHECK(write_source(2, 8000));
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, source_op(1, 8000));
    CHECK_APPLY(h, source_op(2, 8000));
    CHECK_APPLY(h, track_op(100, 101, "A"));
    CHECK_APPLY(h, track_op(102, 103, "B"));
    Region a = region(210, 1, 1000, 3000, 500);
    a.fade_in = {100, FadeShape::Linear};
    a.fade_out = {200, FadeShape::EqualPower};
    a.gain_db = -6.0f;
    CHECK_APPLY(h, (PlaceRegion{101, a, 0}));
    CHECK_APPLY(h, (PlaceRegion{101, region(211, 2, 4000, 2000, 1000), 0}));
    CHECK_APPLY(h, (SetCrossfade{101, Crossfade{210, 211, 128, Align::Centre, FadeShape::SCurve}}));
    Region muted = region(212, 1, 0, 500, 0);
    muted.mute = true;
    CHECK_APPLY(h, (PlaceRegion{103, muted, 0}));

    PlaybackController ctl(*session, kDir);
    std::string error;
    CHECK(ctl.show(h.current(), error));
    CHECK(ctl.pending());
    const sonora::PlanReport& rep = ctl.last_report();
    CHECK(rep.segments == 2);
    CHECK(rep.peak_chunk_demand > 0);

    CHECK(ctl.settle(2.0));
    const PlaybackPlan& p = *session->plans().current();
    CHECK(p.track_count == 3);
    CHECK(p.segment_count == 2);
    CHECK(p.tracks[0].segment_count == 2);
    CHECK(p.tracks[1].segment_count == 0);  // its one region is muted
    const sonora::PlanSegment& l = p.segments[0];
    const sonora::PlanSegment& r = p.segments[1];
    CHECK(l.start == 1000);
    CHECK(l.end == 4000 + 64);              // extended into its handle
    CHECK(l.source_offset == 500);
    CHECK(l.fade_in_end == 1100);
    CHECK(l.fade_in_shape == sonora::FadeCurve::Linear);
    CHECK(l.fade_out_start == 4000 - 64);   // the crossfade replaces the fade out
    CHECK(l.fade_out_shape == sonora::FadeCurve::SCurve);
    CHECK(std::fabs(l.gain - std::pow(10.0f, -6.0f / 20.0f)) < 1e-6f);
    CHECK(r.start == 4000 - 64);
    CHECK(r.source_offset == 1000 - 64);
    CHECK(r.fade_in_end == 4000 + 64);
    CHECK(r.fade_in_shape == sonora::FadeCurve::SCurve);
    CHECK(r.end == 6000);
    CHECK(l.source_slot[0] != r.source_slot[0]);
    // Chunk slots: one per source chunk each segment spans, laid end to end.
    CHECK(l.first_chunk_index[0] == 500 / kChunk);
    CHECK(l.chunk_count[0] == (500 + 3064 - 1) / kChunk - 500 / kChunk + 1);
    CHECK(r.first_chunk_slot[0] == l.first_chunk_slot[0] + l.chunk_count[0]);
    CHECK(p.chunk_slot_count == l.chunk_count[0] + r.chunk_count[0]);
    // The graph: TrackOut x3 -> BusSum -> HardwareOut, delay fields at 0.
    CHECK(p.node_count == 5);
    CHECK(p.edge_count == 4);
    CHECK(p.nodes[3].kind == sonora::NodeKind::BusSum);
    CHECK(p.nodes[3].input_edge_count == 3);
    CHECK(p.nodes[4].kind == sonora::NodeKind::HardwareOut);
    for (uint32_t e = 0; e < p.edge_count; ++e) CHECK(p.edges[e].delay_frames == 0);
    CHECK(p.monitor_out[0] == 0 && p.monitor_out[1] == 1);
    // -3 dB at centre.
    CHECK(std::fabs(std::pow(0.5f, p.pan_exponent) - std::pow(10.0f, -3.0f / 20.0f)) < 1e-6f);
    CHECK(session->tracks()[0]->pan() == -1.0f);
    return 0;
}

int test_limits_are_named() {
    auto session = make_session(1);
    CHECK(write_source(1, 8000));
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, source_op(1, 8000));
    CHECK_APPLY(h, track_op(100, 101, "A"));
    CHECK_APPLY(h, track_op(102, 103, "B"));
    PlaybackController ctl(*session, kDir);
    std::string error;
    CHECK(!ctl.show(h.current(), error));
    CHECK(error.find("limit: tracks") != std::string::npos);
    CHECK(!ctl.pending());

    auto two = make_session(1);
    History g(new_session(SessionSettings{}));
    CHECK_APPLY(g, source_op(1, 8000));
    CHECK_APPLY(g, track_op(100, 101, "A"));
    for (Id i = 0; i < 65; ++i) {
        CHECK_APPLY(g, (PlaceRegion{101, region(200 + i, 1, i * 100, 50, 0), 0}));
    }
    PlaybackController c2(*two, kDir);
    CHECK(!c2.show(g.current(), error));
    CHECK(error.find("limit: max_segments") != std::string::npos);
    return 0;
}

// Record-and-undo's playback half, without a backend: rolling over a
// region, a punch-like overwrite is applied and shown, then undone, all
// while rolling. Every block is exactly one plan's audio or the other's,
// switching at a block boundary, and no chunk is ever missing.
int test_undo_while_rolling() {
    auto session = make_session(1);
    Session& s = *session;
    CHECK(write_source(1, 40 * kChunk));
    CHECK(write_source(2, 40 * kChunk));
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, source_op(1, 40 * kChunk));
    CHECK_APPLY(h, source_op(2, 40 * kChunk));
    CHECK_APPLY(h, track_op(100, 101, "A"));
    CHECK_APPLY(h, (PlaceRegion{101, region(210, 1, 0, 30 * kChunk, 0), 0}));
    PlaybackController ctl(s, kDir);
    std::string error;
    CHECK(ctl.show(h.current(), error));
    CHECK(ctl.settle(2.0));
    Out o;
    run_block(s, o);  // ramps settle
    ctl.play();
    CHECK(ctl.settle(2.0));
    CHECK(s.transport().state() == sonora::TransportState::Rolling);

    // Overwrite [2000, 5000) with source 2, then undo it, both while
    // rolling through it.
    const auto expect = [&](int64_t t, bool overwritten) {
        if (overwritten && t >= 2000 && t < 5000) return value(2, t);
        return value(1, t);
    };
    bool overwritten = false;
    int switches = 0;
    for (int blk = 0; blk < 100; ++blk) {
        if (blk == 10) {
            CHECK_APPLY(h, (PlaceRegion{101, region(211, 2, 2000, 3000, 2000), 220}));
            CHECK(ctl.show(h.current(), error));
        }
        if (blk == 50) {
            CHECK(h.undo());
            CHECK(ctl.show(h.current(), error));
        }
        s.reader().run_pass();
        ctl.pump();
        const uint64_t epoch_before = s.plans().current()->epoch;
        const int64_t at = static_cast<int64_t>(s.transport().playhead());
        run_block(s, o);
        // The plan the block played is the one current when it began:
        // epoch 2 the original, 3 the overwrite, 4 the undo.
        const bool now_overwritten = epoch_before == 3;
        if (now_overwritten != overwritten) {
            ++switches;
            overwritten = now_overwritten;
        }
        for (uint32_t i = 0; i < kBlock; ++i) {
            if (o.l[i] != expect(at + i, overwritten)) {
                std::fprintf(stderr, "  block %d frame %u: %f\n", blk, i, o.l[i]);
                return 1;
            }
        }
    }
    CHECK(switches == 2);
    CHECK(ctl.plans_published() == 3);
    CHECK(s.playback_misses() == 0);
    return 0;
}

int test_locate_and_cancel() {
    auto session = make_session(1);
    Session& s = *session;
    CHECK(write_source(1, 40 * kChunk));
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, source_op(1, 40 * kChunk));
    CHECK_APPLY(h, track_op(100, 101, "A"));
    CHECK_APPLY(h, (PlaceRegion{101, region(210, 1, 0, 40 * kChunk, 0), 0}));
    PlaybackController ctl(s, kDir);
    std::string error;
    // Two snapshots before the first is resident: only the newest waits.
    CHECK(ctl.show(h.current(), error));
    CHECK_APPLY(h, (SetFade{101, 210, FadeEnd::In, Fade{10, FadeShape::Linear}}));
    CHECK(ctl.show(h.current(), error));
    CHECK(ctl.settle(2.0));
    CHECK(ctl.plans_published() == 1);
    CHECK(s.plans().current()->segments[0].fade_in_end == 10);
    Out o;
    run_block(s, o);
    ctl.play();
    CHECK(ctl.settle(2.0));
    run_block(s, o);
    run_block(s, o);

    const uint64_t target = 33 * kChunk + 5;
    CHECK(ctl.locate(target, error));
    CHECK(ctl.locating());
    // The reader has not run: nothing is sent, the roll goes on.
    ctl.pump();
    CHECK(ctl.locating());
    run_block(s, o);
    CHECK(s.transport().playhead() == 3 * kBlock);
    s.reader().run_pass();
    ctl.pump();
    CHECK(!ctl.locating());
    run_block(s, o);
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(o.l[i] == value(1, static_cast<int64_t>(target + i)));
    CHECK(s.playback_misses() == 0);
    CHECK(ctl.max_locate_ms() >= 0.0);

    // Stopped, then a locate and play from there: the first block rolled is
    // the target's.
    ctl.stop();
    CHECK(ctl.locate(2 * kChunk, error));
    ctl.play();
    // The callback keeps running while stopped; that is where the locate
    // is applied, and only then does the roll start.
    for (int i = 0; i < 10 && s.transport().state() != sonora::TransportState::Rolling; ++i) {
        s.reader().run_pass();
        ctl.pump();
        if (s.transport().state() != sonora::TransportState::Rolling) run_block(s, o);
    }
    CHECK(s.transport().state() == sonora::TransportState::Rolling);
    const uint64_t at = s.transport().playhead();
    CHECK(at == 2 * kChunk);
    run_block(s, o);
    for (uint32_t i = 0; i < kBlock; ++i) CHECK(o.l[i] == value(1, static_cast<int64_t>(at + i)));
    return 0;
}

// The keys: toggle_play rolls once resident and stops a roll without
// touching the take counter; return_to_zero locates through the gate.
int test_transport_commands() {
    auto session = make_session(1);
    Session& s = *session;
    CHECK(write_source(1, 40 * kChunk));
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, source_op(1, 40 * kChunk));
    CHECK_APPLY(h, track_op(100, 101, "A"));
    CHECK_APPLY(h, (PlaceRegion{101, region(210, 1, 0, 40 * kChunk, 0), 0}));
    PlaybackController ctl(s, kDir);
    std::string error;
    CHECK(ctl.show(h.current(), error));
    sonora::CommandRegistry reg;
    int take = 7;
    bool quit = false;
    sonora::install_session_commands(reg, s, take, quit, nullptr, &ctl);
    CHECK(reg.implemented("transport.play"));
    CHECK(reg.implemented("transport.toggle_play"));
    CHECK(reg.implemented("transport.return_to_zero"));

    CHECK(reg.dispatch("transport.toggle_play") == sonora::DispatchResult::Ok);
    CHECK(ctl.play_requested());
    CHECK(ctl.settle(2.0));
    CHECK(s.transport().state() == sonora::TransportState::Rolling);
    Out o;
    for (int i = 0; i < 5; ++i) run_block(s, o);
    CHECK(reg.dispatch("transport.toggle_play") == sonora::DispatchResult::Ok);
    CHECK(s.transport().state() == sonora::TransportState::Stopped);
    CHECK(take == 7);
    CHECK(reg.dispatch("transport.play") == sonora::DispatchResult::Ok);
    CHECK(ctl.settle(2.0));
    CHECK(reg.dispatch("transport.stop") == sonora::DispatchResult::Ok);
    CHECK(s.transport().state() == sonora::TransportState::Stopped);
    CHECK(take == 7);

    CHECK(s.transport().playhead() == 5 * kBlock);
    CHECK(reg.dispatch("transport.return_to_zero") == sonora::DispatchResult::Ok);
    s.reader().run_pass();
    ctl.pump();
    run_block(s, o);
    CHECK(s.transport().playhead() == 0);
    return 0;
}

}  // namespace

int main() {
    ::mkdir(kDir.c_str(), 0775);
    ::mkdir((kDir + "/Audio Files").c_str(), 0775);
    if (test_build_from_model()) return 1;
    if (test_limits_are_named()) return 1;
    if (test_undo_while_rolling()) return 1;
    if (test_locate_and_cancel()) return 1;
    if (test_transport_commands()) return 1;
    std::remove((kDir + "/Audio Files/src_1.wav").c_str());
    std::remove((kDir + "/Audio Files/src_2.wav").c_str());
    std::printf("playback_control: ok\n");
    return 0;
}
