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
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "sonora/closed_take.h"
#include "sonora/model/history.h"
#include "sonora/model/operations.h"
#include "sonora/playback_controller.h"
#include "sonora/session.h"
#include "sonora/session_commands.h"
#include "sonora/wav_reader.h"
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
using sonora::DispatchResult;
using sonora::Session;
using sonora::TransportState;

constexpr uint32_t kChunk = 256;
constexpr uint32_t kBlock = 64;
const std::string kDir = "test_playback_control_session";

// As EngineControlServer does at start and after an update that changes the
// mix: the plan is shown, and the mix applied on its own.
bool show(PlaybackController& ctl, const Snapshot& snapshot, std::string& error) {
    ctl.apply_mix(snapshot);
    return ctl.show(snapshot, error);
}

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

// A block with `in` on every track's input.
void run_block_in(Session& s, Out& o, const std::vector<float>& in) {
    const std::vector<const float*> ins(s.tracks().size(), in.data());
    float* outs[2] = {o.l.data(), o.r.data()};
    sonora::backend::AudioBlock b{};
    b.inputs = ins.data();
    b.input_count = static_cast<uint32_t>(ins.size());
    b.outputs = outs;
    b.output_count = 2;
    b.nframes = kBlock;
    s.process(b);
}

bool has_nan(const Out& o) {
    for (uint32_t i = 0; i < kBlock; ++i) {
        if (std::isnan(o.l[i]) || std::isnan(o.r[i])) return true;
    }
    return false;
}

// The BWF TimeReference and length of `track`'s last take, or false.
bool take_on_disk(Session& s, std::size_t track, uint64_t& origin, int64_t& frames) {
    sonora::WavReader r;
    std::string error;
    if (!r.open(s.tracks()[track]->writer().path(), error)) return false;
    origin = r.time_reference();
    frames = r.frames();
    return true;
}

// Track A plays source 1 from 0 for 40 chunks; track B is empty.
bool two_tracks(History& h) {
    std::string e;
    return h.apply(source_op(1, 40 * kChunk), e) && h.apply(track_op(100, 101, "A"), e) &&
           h.apply(track_op(102, 103, "B"), e) &&
           h.apply(PlaceRegion{101, region(210, 1, 0, 40 * kChunk, 0), 0}, e);
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
    CHECK(show(ctl, h.current(), error));
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

// The audio thread reads a segment's source from source_offset with no
// check (session.cpp, render_segment): a negative one reads before the
// chunk buffer. The model refuses a crossfade longer than the right
// region's handle, but the builder shifts the offset itself, so it refuses
// such a plan on its own rather than trust every path into it.
int test_crossfade_past_the_handle_is_refused() {
    auto session = make_session(1);
    CHECK(write_source(1, 8000));
    CHECK(write_source(2, 8000));
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, source_op(1, 8000));
    CHECK_APPLY(h, source_op(2, 8000));
    CHECK_APPLY(h, track_op(100, 101, "A"));
    CHECK_APPLY(h, (PlaceRegion{101, region(210, 1, 1000, 3000, 500), 0}));
    CHECK_APPLY(h, (PlaceRegion{101, region(211, 2, 4000, 2000, 1000), 0}));
    CHECK_APPLY(h, (SetCrossfade{101, Crossfade{210, 211, 128, Align::Centre, FadeShape::SCurve}}));

    // What only a bypassed check could produce: 64 frames of crossfade
    // before the cut with 10 frames of handle.
    Snapshot bad = h.current();
    auto edit = std::make_shared<EditState>(*bad.edit);
    auto track = std::make_shared<TrackEdit>(*edit->tracks[0]);
    auto list = std::make_shared<Playlist>(*track->playlists[0]);
    list->regions[1].source_offset = 10;
    track->playlists[0] = list;
    edit->tracks[0] = track;
    bad.edit = edit;

    PlaybackController ctl(*session, kDir);
    std::string error;
    CHECK(!ctl.show(bad, error));
    CHECK(error.find("before its source") != std::string::npos);
    CHECK(!ctl.pending());
    CHECK(ctl.show(h.current(), error));
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
    CHECK(show(ctl, h.current(), error));
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
            CHECK(show(ctl, h.current(), error));
        }
        if (blk == 50) {
            CHECK(h.undo());
            CHECK(show(ctl, h.current(), error));
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
    CHECK(show(ctl, h.current(), error));
    CHECK_APPLY(h, (SetFade{101, 210, FadeEnd::In, Fade{10, FadeShape::Linear}}));
    CHECK(show(ctl, h.current(), error));
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
    CHECK(show(ctl, h.current(), error));
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


// The take's origin is the timeline frame its first sample was recorded at,
// as the audio thread saw it, not the playhead when record was pressed:
// here a locate already queued is applied by the take's first block.
int test_take_origin_is_first_recorded_frame() {
    auto session = make_session(1);
    Session& s = *session;
    s.tracks()[0]->set_rec_armed(true);
    s.disk().start();
    const std::vector<float> in(kBlock, 0.5f);
    Out o;
    s.play();
    for (int i = 0; i < 3; ++i) run_block_in(s, o, in);
    CHECK(s.transport().playhead() == 3 * kBlock);
    CHECK(s.post_locate(1000));
    std::string error;
    CHECK(s.start_recording(s.next_free_take(), &error));
    for (int i = 0; i < 4; ++i) run_block_in(s, o, in);
    CHECK(s.stop(&error));
    uint64_t origin = 0;
    int64_t frames = 0;
    CHECK(take_on_disk(s, 0, origin, frames));
    CHECK(frames == 4 * kBlock);
    CHECK(origin == 1000);
    return 0;
}

// Takes closed while rolling join the document from the disk thread's
// report: punched out, and started over another, each at its own origin and
// length on disk, while the transport never stops. writer() names the take
// recording now, so the report is the only thing that can name these.
int test_takes_closed_while_rolling_join_the_document() {
    auto session = make_session(1);
    Session& s = *session;
    s.tracks()[0]->set_rec_armed(true);
    sonora::model::Document doc(sonora::engine_document(s));
    const std::vector<float> in(kBlock, 0.5f);
    Out o;
    std::string error;
    s.play();
    run_block_in(s, o, in);
    const int first = s.next_free_take();
    CHECK(s.start_recording(first, &error));
    for (int i = 0; i < 3; ++i) run_block_in(s, o, in);
    CHECK(s.start_recording(first + 1, &error));
    for (int i = 0; i < 2; ++i) run_block_in(s, o, in);
    CHECK(s.punch_out());
    run_block_in(s, o, in);
    CHECK(s.transport().state() == sonora::TransportState::Rolling);

    const std::vector<Session::ClosedTake> closed = s.closed_takes();
    CHECK(closed.size() == 2);
    for (const Session::ClosedTake& take : closed) {
        const sonora::TakeAdded added = sonora::add_closed_take(doc, s, take);
        CHECK(added.track_notices.empty() && added.cut_short.empty() && added.refused.empty());
        CHECK(added.updates.size() == 1);
    }
    const auto& track = *doc.current().edit->tracks[0];
    const sonora::model::Playlist* list = nullptr;
    for (const auto& p : track.playlists) {
        if (p->id == track.active_playlist) list = p.get();
    }
    CHECK(list != nullptr);
    CHECK(list->regions.size() == 2);
    CHECK(list->regions[0].position == kBlock);
    CHECK(list->regions[0].length == 3 * kBlock);
    CHECK(list->regions[1].position == 4 * kBlock);
    CHECK(list->regions[1].length == 2 * kBlock);
    CHECK(s.stop(&error));
    return 0;
}

// Code review 3 on PR #60: a take that failed (full disk, 4 GB limit) and
// was then punched out. punch_out() forgot it, so take_failure() went
// quiet, the next stop() said all was well, and the closed-take report
// placed the truncated file as a good take. Now the failure is still said
// until stop(), stop() reports it, and the failed file is placed up to its
// last whole frame, as main did: the operator keeps what was recorded.
int test_failure_in_a_take_punched_out_is_reported() {
    constexpr uint32_t kEmptyRiffSize = 4 + (8 + 602) + (8 + 16) + 8;
    auto session = make_session(2);
    Session& s = *session;
    s.tracks()[0]->set_rec_armed(true);
    s.tracks()[1]->set_rec_armed(true);
    // Only track 2 runs out of room: 10 mono 24-bit frames, in either slot.
    for (int slot = 0; slot < sonora::Track::kWriterSlots; ++slot) {
        s.tracks()[1]->writer(slot).set_riff_size_limit(kEmptyRiffSize + 10 * 3);
    }
    Document doc(sonora::engine_document(s));
    const std::vector<float> in(kBlock, 0.5f);
    Out o;
    std::string error;
    s.play();
    const int take = s.next_free_take();
    CHECK(s.start_recording(take, &error));
    for (int i = 0; i < 3; ++i) run_block_in(s, o, in);
    CHECK(s.closed_takes().empty());  // drains: track 2's writer fails here
    const std::string why = s.take_failure();
    CHECK(why.find("Audio 2_") != std::string::npos);
    CHECK(why.find("4 GB") != std::string::npos);
    CHECK(why.find("Audio 1_") == std::string::npos);

    CHECK(s.punch_out());
    CHECK(s.take_failure() == why);
    run_block_in(s, o, in);
    CHECK(s.transport().state() == TransportState::Rolling);

    const std::vector<Session::ClosedTake> closed = s.closed_takes();
    CHECK(closed.size() == 1);
    const sonora::TakeAdded added = sonora::add_closed_take(doc, s, closed[0]);
    CHECK(added.track_notices.empty() && added.refused.empty());
    const std::string& placed = added.cut_short;
    CHECK(placed.find("was cut short") != std::string::npos);
    CHECK(placed.find("Audio 2_") != std::string::npos);
    CHECK(placed.find("4 GB") != std::string::npos);
    CHECK(placed.find("placed up to the failure") != std::string::npos);
    CHECK(placed.find("Audio 1_") == std::string::npos);
    const auto regions = [&](std::size_t t) -> const std::vector<sonora::model::Region>* {
        const auto& track = *doc.current().edit->tracks[t];
        for (const auto& p : track.playlists) {
            if (p->id == track.active_playlist) return &p->regions;
        }
        return nullptr;
    };
    CHECK(regions(0) && regions(0)->size() == 1);
    CHECK((*regions(0))[0].length == 3 * kBlock);
    // Exactly the 10 frames that fit under the limit, at the take's start.
    CHECK(regions(1) && regions(1)->size() == 1);
    CHECK((*regions(1))[0].length == 10);
    CHECK((*regions(1))[0].position == (*regions(0))[0].position);
    CHECK(s.take_failure() == why);

    std::string at_stop;
    CHECK(!s.stop(&at_stop));
    CHECK(at_stop == why);
    CHECK(s.take_failure().empty());
    return 0;
}

// PR #60 review 4: take K failed and was punched out, and take K + 1 then
// started while rolling and recorded cleanly. failed_files_ lasted until
// stop(), so take_failure() kept naming K's file through K + 1 (the server
// posts that as "take K + 1 failed" and the health flag never clears), and
// stop() failed K + 1 with K's reason. K's failure was already said as a
// Notice when its close was collected; the next clean take clears it.
int test_failure_does_not_follow_into_the_next_take() {
    constexpr uint32_t kEmptyRiffSize = 4 + (8 + 602) + (8 + 16) + 8;
    auto session = make_session(2);
    Session& s = *session;
    s.tracks()[0]->set_rec_armed(true);
    s.tracks()[1]->set_rec_armed(true);
    for (int slot = 0; slot < sonora::Track::kWriterSlots; ++slot) {
        s.tracks()[1]->writer(slot).set_riff_size_limit(kEmptyRiffSize + 10 * 3);
    }
    Document doc(sonora::engine_document(s));
    const std::vector<float> in(kBlock, 0.5f);
    Out o;
    std::string error;
    s.play();
    const int take = s.next_free_take();
    CHECK(s.start_recording(take, &error));
    for (int i = 0; i < 3; ++i) run_block_in(s, o, in);
    CHECK(s.closed_takes().empty());
    const std::string why = s.take_failure();
    CHECK(why.find("Audio 2_") != std::string::npos);

    CHECK(s.punch_out());
    run_block_in(s, o, in);
    const std::vector<Session::ClosedTake> closed = s.closed_takes();
    CHECK(closed.size() == 1);
    CHECK(!sonora::add_closed_take(doc, s, closed[0]).cut_short.empty());
    CHECK(s.take_failure() == why);

    // Track 2 has no room in either slot, so the next take leaves it out.
    s.tracks()[1]->set_rec_armed(false);
    CHECK(s.start_recording(take + 1, &error));
    for (int i = 0; i < 3; ++i) run_block_in(s, o, in);
    CHECK(s.closed_takes().empty());
    if (!s.take_failure().empty()) {
        std::fprintf(stderr, "  take %d reported: %s\n", take + 1, s.take_failure().c_str());
        return 1;
    }
    std::string at_stop;
    CHECK(s.stop(&at_stop));
    CHECK(at_stop.empty());
    return 0;
}

// PR #60 re-review: a take whose first track was cut short (and placed up
// to the failure) and whose second track cannot be placed at all. The
// second reason replaced the first, so the Notice lost the truncated
// track, and it named no track for the one left out.
int test_unplaceable_track_keeps_the_earlier_failure() {
    constexpr uint32_t kEmptyRiffSize = 4 + (8 + 602) + (8 + 16) + 8;
    auto session = make_session(2);
    Session& s = *session;
    s.tracks()[0]->set_rec_armed(true);
    s.tracks()[1]->set_rec_armed(true);
    for (int slot = 0; slot < sonora::Track::kWriterSlots; ++slot) {
        s.tracks()[0]->writer(slot).set_riff_size_limit(kEmptyRiffSize + 10 * 3);
    }
    // A document with one track: the take's second file has nowhere to go.
    auto one = make_session(1);
    Document doc(sonora::engine_document(*one));
    const std::vector<float> in(kBlock, 0.5f);
    Out o;
    std::string error;
    s.play();
    CHECK(s.start_recording(s.next_free_take(), &error));
    for (int i = 0; i < 3; ++i) run_block_in(s, o, in);
    std::string at_stop;
    CHECK(!s.stop(&at_stop));

    const std::vector<Session::ClosedTake> closed = s.closed_takes();
    CHECK(closed.size() == 1);
    const sonora::TakeAdded added = sonora::add_closed_take(doc, s, closed[0]);
    CHECK(added.refused.empty());
    // The track left out has its own notice, naming it and its file.
    CHECK(added.track_notices.size() == 1);
    const std::string& left_out = added.track_notices[0];
    CHECK(left_out.find("is on disk but not in the session") != std::string::npos);
    CHECK(left_out.find("track 2") != std::string::npos);
    CHECK(left_out.find("Audio 2_") != std::string::npos);
    // The earlier track's failure is still said, in its own.
    const std::string& placed = added.cut_short;
    CHECK(placed.find("Audio 1_") != std::string::npos);
    CHECK(placed.find("4 GB") != std::string::npos);
    CHECK(placed.find("placed up to the failure") != std::string::npos);
    const auto& track = *doc.current().edit->tracks[0];
    for (const auto& p : track.playlists) {
        if (p->id != track.active_playlist) continue;
        CHECK(p->regions.size() == 1);
        CHECK(p->regions[0].length == 10);
    }
    return 0;
}

// Return to zero, then record, in one pass of the terminal loop: the take
// starts at zero once the locate is applied, and the playhead never jumps
// while it runs.
int test_return_to_zero_then_record() {
    auto session = make_session(2);
    Session& s = *session;
    CHECK(write_source(1, 40 * kChunk));
    History h(new_session(SessionSettings{}));
    CHECK(two_tracks(h));
    PlaybackController ctl(s, kDir);
    std::string error;
    CHECK(show(ctl, h.current(), error));
    CHECK(ctl.settle(2.0));
    sonora::CommandRegistry reg;
    int take = 1;
    bool quit = false;
    sonora::install_session_commands(reg, s, take, quit, nullptr, &ctl);
    s.tracks()[1]->set_rec_armed(true);
    s.disk().start();
    const std::vector<float> in(kBlock, 0.5f);
    Out o;
    ctl.play();
    CHECK(ctl.settle(2.0));
    for (int i = 0; i < 20; ++i) run_block_in(s, o, in);
    ctl.stop();
    CHECK(s.transport().playhead() == 20 * kBlock);

    CHECK(reg.dispatch("transport.return_to_zero") == DispatchResult::Ok);
    CHECK(reg.dispatch("transport.record") == DispatchResult::Ok);
    int recorded = 0;
    for (int i = 0; i < 40; ++i) {
        s.reader().run_pass();
        ctl.pump();
        const bool rec = s.transport().state() == TransportState::Recording;
        const uint64_t at = s.transport().playhead();
        run_block_in(s, o, in);
        if (rec) {
            ++recorded;
            CHECK(s.transport().playhead() == at + kBlock);
        }
    }
    CHECK(recorded > 30);
    CHECK(reg.dispatch("transport.stop") == DispatchResult::Ok);
    uint64_t origin = 1;
    int64_t frames = 0;
    CHECK(take_on_disk(s, 1, origin, frames));
    CHECK(origin == 0);
    CHECK(frames == recorded * static_cast<int64_t>(kBlock));
    CHECK(s.playback_misses() == 0);
    return 0;
}

// A locate still waiting for its window when a take starts is dropped: a
// take is one contiguous span of the timeline.
int test_pending_locate_dropped_by_a_take() {
    auto session = make_session(2);
    Session& s = *session;
    CHECK(write_source(1, 40 * kChunk));
    History h(new_session(SessionSettings{}));
    CHECK(two_tracks(h));
    PlaybackController ctl(s, kDir);
    std::string error;
    CHECK(show(ctl, h.current(), error));
    CHECK(ctl.settle(2.0));
    s.tracks()[1]->set_rec_armed(true);
    s.disk().start();
    const std::vector<float> in(kBlock, 0.5f);
    Out o;
    s.transport().set_playhead(10 * kBlock);
    CHECK(ctl.locate(0, error));
    CHECK(ctl.locating());
    // Headless runs start a take straight on the session.
    CHECK(s.start_recording(s.next_free_take(), &error));
    for (int i = 0; i < 10; ++i) {
        s.reader().run_pass();
        ctl.pump();
        const uint64_t at = s.transport().playhead();
        run_block_in(s, o, in);
        CHECK(s.transport().playhead() == at + kBlock);
    }
    CHECK(!ctl.locating());
    CHECK(s.stop(&error));
    uint64_t origin = 0;
    int64_t frames = 0;
    CHECK(take_on_disk(s, 1, origin, frames));
    CHECK(origin == 10 * kBlock);
    // Stopped again, a locate goes through as before.
    CHECK(ctl.locate(0, error));
    for (int i = 0; i < 4 && s.transport().playhead() != 0; ++i) {
        s.reader().run_pass();
        ctl.pump();
        run_block(s, o);
    }
    CHECK(s.transport().playhead() == 0);
    return 0;
}

// Record right after the document changed (a take just closed, or --place):
// the transport starts only once the new plan's window is resident, so the
// playback under the take has no hole.
int test_record_waits_for_residency() {
    auto session = make_session(2);
    Session& s = *session;
    CHECK(write_source(1, 40 * kChunk));
    CHECK(write_source(300, 40 * kChunk));
    History h(new_session(SessionSettings{}));
    CHECK(two_tracks(h));
    PlaybackController ctl(s, kDir);
    std::string error;
    CHECK(show(ctl, h.current(), error));
    CHECK(ctl.settle(2.0));
    sonora::CommandRegistry reg;
    int take = 1;
    bool quit = false;
    sonora::install_session_commands(reg, s, take, quit, nullptr, &ctl);
    s.tracks()[1]->set_rec_armed(true);
    s.disk().start();
    const std::vector<float> in(kBlock, 0.5f);
    Out o;
    run_block_in(s, o, in);

    CHECK_APPLY(h, source_op(300, 40 * kChunk));
    CHECK_APPLY(h, (PlaceRegion{101, region(301, 300, 0, 40 * kChunk, 0), 0}));
    CHECK(show(ctl, h.current(), error));
    CHECK(reg.dispatch("transport.record") == DispatchResult::Ok);
    // The reader has not run: nothing the new plan plays is resident.
    ctl.pump();
    run_block_in(s, o, in);
    CHECK(s.transport().state() != TransportState::Recording);
    for (int i = 0; i < 20; ++i) {
        s.reader().run_pass();
        ctl.pump();
        run_block_in(s, o, in);
    }
    CHECK(s.transport().state() == TransportState::Recording);
    CHECK(s.playback_misses() == 0);
    // A stop cancels a record still waiting for its window.
    CHECK(reg.dispatch("transport.stop") == DispatchResult::Ok);
    CHECK(ctl.locate(5 * kChunk, error));
    CHECK(reg.dispatch("transport.record") == DispatchResult::Ok);
    CHECK(ctl.record_requested());
    CHECK(reg.dispatch("transport.stop") == DispatchResult::Ok);
    CHECK(!ctl.record_requested());
    CHECK(ctl.settle(2.0));
    run_block_in(s, o, in);
    CHECK(s.transport().state() == TransportState::Stopped);
    // Record pressed twice while rolling, before the take could start: the
    // request is taken back and the roll goes on.
    ctl.play();
    CHECK(ctl.settle(2.0));
    CHECK(reg.dispatch("transport.toggle_record") == DispatchResult::Ok);
    CHECK(ctl.record_requested());
    CHECK(reg.dispatch("transport.toggle_record") == DispatchResult::Ok);
    CHECK(!ctl.record_requested());
    CHECK(s.transport().state() == TransportState::Rolling);
    return 0;
}

// --no-input-monitoring holds for the whole run: every mix applied after a
// take sets each track's monitor mode again, and must not put an armed
// track back on Auto; arming under it turns the track's input off at once.
int test_no_input_monitoring_persists() {
    auto session = make_session(2);
    Session& s = *session;
    CHECK(write_source(1, 40 * kChunk));
    History h(new_session(SessionSettings{}));
    CHECK(two_tracks(h));
    PlaybackController ctl(s, kDir);
    ctl.set_input_monitoring(false);
    s.tracks()[1]->set_rec_armed(true);
    std::string error;
    CHECK(show(ctl, h.current(), error));
    CHECK(s.tracks()[0]->monitor() == sonora::Track::Monitor::Auto);
    CHECK(s.tracks()[1]->monitor() == sonora::Track::Monitor::Off);
    ctl.apply_mix(h.current());
    CHECK(s.tracks()[1]->monitor() == sonora::Track::Monitor::Off);
    s.tracks()[1]->set_rec_armed(false);
    ctl.apply_monitoring(h.current());
    CHECK(s.tracks()[1]->monitor() == sonora::Track::Monitor::Auto);
    s.tracks()[0]->set_rec_armed(true);
    ctl.apply_monitoring(h.current());
    CHECK(s.tracks()[0]->monitor() == sonora::Track::Monitor::Off);
    return 0;
}

// A fader moved through a ParameterChange is a Track atomic the document's
// mix does not hold. An edit shows a new plan, and must leave that fader
// where the operator put it rather than snap it back to the document's.
int test_show_leaves_the_faders_alone() {
    auto session = make_session(2);
    Session& s = *session;
    CHECK(write_source(1, 40 * kChunk));
    History h(new_session(SessionSettings{}));
    CHECK(two_tracks(h));
    PlaybackController ctl(s, kDir);
    std::string error;
    CHECK(show(ctl, h.current(), error));
    s.tracks()[0]->set_gain(0.25f);
    CHECK_APPLY(h, (MoveRegion{101, 210, 4 * kChunk, 0}));
    CHECK(ctl.show(h.current(), error));
    CHECK(s.tracks()[0]->gain() == 0.25f);
    ctl.apply_mix(h.current());
    CHECK(s.tracks()[0]->gain() == 1.0f);
    return 0;
}

// Each engine track knows the document track it records for, which its
// takes' bext Description names, after a reorder and an undo as before;
// one past the document's end knows none.
int test_tracks_know_their_document_ids() {
    auto session = make_session(3);
    Session& s = *session;
    CHECK(write_source(1, 40 * kChunk));
    History h(new_session(SessionSettings{}));
    CHECK(two_tracks(h));
    PlaybackController ctl(s, kDir);
    std::string error;
    s.tracks()[2]->set_document_id(999);
    CHECK(show(ctl, h.current(), error));
    CHECK(s.tracks()[0]->document_id() == 100);
    CHECK(s.tracks()[1]->document_id() == 102);
    CHECK(s.tracks()[2]->document_id() == 0);
    CHECK_APPLY(h, (ReorderTracks{{102, 100}}));
    CHECK(ctl.show(h.current(), error));
    CHECK(s.tracks()[0]->document_id() == 102);
    CHECK(s.tracks()[1]->document_id() == 100);
    CHECK(h.undo());
    CHECK(ctl.show(h.current(), error));
    CHECK(s.tracks()[0]->document_id() == 100);
    return 0;
}

// The audio thread counts the solo of every engine track, document or not.
// An undone AddTrack of a soloed track leaves its engine track holding that
// solo, which would silence every other track while the interface shows no
// solo at all. Only the solo goes: until the plan swap the old plan still
// renders that track, so its fader and mute stay as they were.
int test_undone_track_leaves_no_solo() {
    auto session = make_session(3);
    Session& s = *session;
    CHECK(write_source(1, 40 * kChunk));
    History h(new_session(SessionSettings{}));
    CHECK(two_tracks(h));
    AddTrack c = track_op(300, 301, "C");
    c.mix.solo = true;
    c.mix.mute = true;
    c.mix.gain_db = -12.0;
    CHECK_APPLY(h, c);
    PlaybackController ctl(s, kDir);
    std::string error;
    CHECK(show(ctl, h.current(), error));
    CHECK(s.tracks()[2]->soloed() && s.tracks()[2]->muted());
    CHECK(h.undo());
    CHECK(show(ctl, h.current(), error));
    CHECK(!s.tracks()[2]->soloed());
    CHECK(s.tracks()[2]->muted());
    CHECK(s.tracks()[2]->gain() == sonora::gain_of_db(-12.0f));
    CHECK(s.tracks()[2]->pan() == -1.0f);
    CHECK(h.redo());
    CHECK(show(ctl, h.current(), error));
    CHECK(s.tracks()[2]->soloed());
    return 0;
}

// A SetMix's solo safe reaches the engine track, on and off, and nothing else
// changes with it.
int test_solo_safe_reaches_the_track() {
    auto session = make_session(2);
    Session& s = *session;
    CHECK(write_source(1, 40 * kChunk));
    History h(new_session(SessionSettings{}));
    CHECK(two_tracks(h));
    PlaybackController ctl(s, kDir);
    std::string error;
    CHECK(show(ctl, h.current(), error));
    CHECK(!s.tracks()[0]->solo_safe());
    TrackMix m = *h.current().mix.find(100);
    m.solo_safe = true;
    CHECK_APPLY(h, SetMix{m});
    ctl.apply_mix(h.current());
    CHECK(s.tracks()[0]->solo_safe());
    CHECK(!s.tracks()[0]->soloed() && !s.tracks()[0]->muted());
    CHECK(!s.tracks()[1]->solo_safe());
    m.solo_safe = false;
    CHECK_APPLY(h, SetMix{m});
    ctl.apply_mix(h.current());
    CHECK(!s.tracks()[0]->solo_safe());
    return 0;
}

// The same, rolling. A document without track C (a session opened, an
// AddTrack undone) is shown while the old plan still renders C on engine
// track 2, and goes on rendering it until the reader has the new plan's
// window and pump() publishes it. C is muted: not a sample of it may be
// heard in that window (review of PR #71: it played at 0 dB until the
// swap when the controller unmuted tracks past the document's end).
int test_removed_track_stays_muted_until_the_swap() {
    auto session = make_session(3);
    Session& s = *session;
    CHECK(write_source(1, 40 * kChunk));
    History without(new_session(SessionSettings{}));
    CHECK(two_tracks(without));
    History with = without;
    AddTrack c = track_op(300, 301, "C");
    c.mix.mute = true;
    CHECK_APPLY(with, c);
    CHECK_APPLY(with, (PlaceRegion{301, region(310, 1, 0, 40 * kChunk, 0), 0}));
    PlaybackController ctl(s, kDir);
    std::string error;
    CHECK(show(ctl, with.current(), error));
    CHECK(ctl.settle(2.0));
    Out o;
    run_block(s, o);  // ramps settle
    ctl.play();
    CHECK(ctl.settle(2.0));
    CHECK(s.transport().state() == TransportState::Rolling);

    // Track A alone is heard, hard left, before and after; C never.
    const auto only_a = [&]() {
        const int64_t at = static_cast<int64_t>(s.transport().playhead());
        run_block(s, o);
        for (uint32_t i = 0; i < kBlock; ++i) {
            if (o.l[i] != value(1, at + i)) {
                std::fprintf(stderr, "  frame %lld: %f, want %f\n",
                             static_cast<long long>(at + i), o.l[i], value(1, at + i));
                return false;
            }
        }
        return true;
    };
    for (int blk = 0; blk < 4; ++blk) {
        s.reader().run_pass();
        ctl.pump();
        CHECK(only_a());
    }
    const uint64_t published = ctl.plans_published();
    CHECK(show(ctl, without.current(), error));
    // No reader pass, no pump: the old plan plays these.
    for (int blk = 0; blk < 4; ++blk) CHECK(only_a());
    CHECK(ctl.plans_published() == published);
    for (int blk = 0; blk < 8; ++blk) {
        s.reader().run_pass();
        ctl.pump();
        CHECK(only_a());
    }
    CHECK(ctl.plans_published() == published + 1);
    CHECK(s.playback_misses() == 0);
    return 0;
}

// A take at another sample rate would play at the wrong speed: refused,
// with both rates named.
int test_sample_rate_must_match() {
    auto session = make_session(1);
    const std::string rel = "Audio Files/rate_44100.wav";
    const std::string path = kDir + "/" + rel;
    std::remove(path.c_str());
    sonora::WavWriter w;
    CHECK(w.open(path, 44100, 1, sonora::WavWriter::Format::Float32, 0));
    const std::vector<float> buf(1000, 0.25f);
    CHECK(w.write(buf.data(), buf.size()) && w.close());
    sonora::model::Document doc(sonora::engine_document(*session));
    std::vector<sonora::model::ReplicationUpdate> updates;
    std::string error;
    CHECK(!sonora::place_file(doc, kDir, rel, 0, updates, error));
    CHECK(updates.empty());
    CHECK(error.find("44100") != std::string::npos);
    CHECK(error.find("48000") != std::string::npos);
    std::remove(path.c_str());
    return 0;
}

// A second locate asked while the first is sent but not applied: play must
// wait for the second, or it can roll a block from the first target.
int test_locate_serial_is_monotonic() {
    auto session = make_session(1);
    Session& s = *session;
    CHECK(write_source(1, 40 * kChunk));
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, source_op(1, 40 * kChunk));
    CHECK_APPLY(h, track_op(100, 101, "A"));
    CHECK_APPLY(h, (PlaceRegion{101, region(210, 1, 0, 40 * kChunk, 0), 0}));
    PlaybackController ctl(s, kDir);
    std::string error;
    CHECK(show(ctl, h.current(), error));
    CHECK(ctl.settle(2.0));
    Out o;
    CHECK(ctl.locate(10 * kChunk, error));
    s.reader().run_pass();
    ctl.pump();
    CHECK(!ctl.locating());
    CHECK(ctl.locate(20 * kChunk, error));
    s.reader().run_pass();
    ctl.play();
    run_block(s, o);  // applies the first locate only
    CHECK(s.transport().locates_applied() == 1);
    for (int i = 0; i < 10 && s.transport().state() != TransportState::Rolling; ++i) {
        s.reader().run_pass();
        ctl.pump();
        CHECK(s.transport().state() != TransportState::Rolling ||
              s.transport().locates_applied() == 2);
        if (s.transport().state() != TransportState::Rolling) run_block(s, o);
    }
    CHECK(s.transport().state() == TransportState::Rolling);
    CHECK(s.transport().playhead() == 20 * kChunk);
    return 0;
}

// A gain the model would refuse still never reaches the audio thread as
// inf: a silent input times inf is NaN on the monitor bus.
int test_huge_gain_is_no_nan() {
    auto session = make_session(1);
    Session& s = *session;
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, track_op(100, 101, "A"));
    PlaybackController ctl(s, kDir);
    Snapshot snap = h.current();
    snap.mix.tracks[0].gain_db = 1e30f;
    snap.mix.tracks[0].monitor = MonitorMode::Input;
    snap.mix.bus.gain_db = 1e30f;
    ctl.apply_mix(snap);
    const std::vector<float> silent(kBlock, 0.0f);
    const std::vector<float> loud(kBlock, 1.0f);
    Out o;
    for (int i = 0; i < 3; ++i) {
        run_block_in(s, o, silent);
        CHECK(!has_nan(o));
        run_block_in(s, o, loud);
        CHECK(!has_nan(o));
    }
    return 0;
}

AddInsert insert_op(Id track, Id insert, std::uint8_t slot) {
    AddInsert op;
    op.track = track;
    op.insert = insert;
    op.slot = slot;
    op.plugin = PluginRef{PluginFormat::Lv2, "http://gareus.org/oss/lv2/dpl#mono",
                          "x42-dpl Digital Peak Limiter Mono", "0.9.2"};
    return op;
}

// ADR-0014 M-1: a session with MIDI and instrument tracks builds, those
// tracks play nothing, and the report counts them so the engine can say so.
// Regions in ticks never reach the frame segments.
int test_midi_tracks_build_silent() {
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, track_op(100, 101, "Vox"));
    AddTrack midi = track_op(102, 103, "Bass");
    midi.kind = TrackKind::Midi;
    midi.channels = 0;
    midi.mix = TrackMix{};
    CHECK_APPLY(h, midi);
    AddTrack inst = track_op(104, 105, "Piano");
    inst.kind = TrackKind::Instrument;
    inst.channels = 2;
    inst.mix = TrackMix{};
    CHECK_APPLY(h, inst);
    auto data = std::make_shared<MidiSequence>();
    data->notes.push_back(sonora::midi::Note{});
    MidiRegion r;
    r.id = 106;
    r.name = "Bass_01";
    r.length = kTicksPerQuarter;
    r.data = data;
    CHECK_APPLY(h, (PlaceMidiRegion{103, r, 0}));

    sonora::BuildLimits limits;
    limits.engine_tracks = 3;
    limits.channels_per_track = 2;
    limits.max_segments = 64;
    limits.max_chunk_slots = 1024;
    limits.chunk_frames = kChunk;
    limits.window_frames = 1024;
    limits.pool_chunks = 128;
    const sonora::SourceSlot no_sources = [](const Source&, uint32_t&, std::string& e) {
        e = "no source expected";
        return false;
    };
    PlaybackPlan* plan = nullptr;
    sonora::PlanReport report;
    std::string error;
    CHECK(sonora::build_plan(h.current(), limits, no_sources, 1, plan, report, error));
    CHECK(plan != nullptr);
    CHECK(report.silent_midi_tracks == 2);
    CHECK(report.segments == 0);
    CHECK(plan->tracks[0].channels == 1);
    CHECK(plan->tracks[1].segment_count == 0);
    CHECK(plan->tracks[2].channels == 2 && plan->tracks[2].segment_count == 0);
    sonora::destroy_plan(plan);
    return 0;
}

// PR #100 review: the engine track under a MIDI or instrument track passed
// its capture channel through under Input or Auto monitoring, and could be
// armed and record a WAV. Its monitoring is forced off, an arm left on it
// is cleared while no take runs, and arming refuses it.
int test_midi_tracks_are_never_monitored_or_armed() {
    auto session = make_session(3);
    Session& s = *session;
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, track_op(100, 101, "Vox"));
    AddTrack midi = track_op(102, 103, "Bass");
    midi.kind = TrackKind::Midi;
    midi.channels = 0;
    midi.mix = TrackMix{};
    midi.mix.monitor = MonitorMode::Input;
    CHECK_APPLY(h, midi);
    AddTrack inst = track_op(104, 105, "Piano");
    inst.kind = TrackKind::Instrument;
    inst.channels = 1;
    inst.mix = TrackMix{};
    inst.mix.monitor = MonitorMode::Auto;
    CHECK_APPLY(h, inst);
    for (const auto& t : s.tracks()) t->set_rec_armed(true);
    PlaybackController ctl(s, kDir);
    ctl.apply_mix(h.current());
    CHECK(s.tracks()[0]->rec_armed());
    CHECK(!s.tracks()[1]->rec_armed() && !s.tracks()[2]->rec_armed());
    CHECK(s.tracks()[1]->monitor() == sonora::Track::Monitor::Off);
    CHECK(s.tracks()[2]->monitor() == sonora::Track::Monitor::Off);
    CHECK(s.tracks()[0]->monitor() != sonora::Track::Monitor::Off);

    sonora::CommandRegistry reg;
    int take = 1;
    bool quit = false;
    sonora::install_session_commands(
        reg, s, take, quit, nullptr, &ctl, nullptr,
        [&h](std::size_t engine_track, std::string& why) {
            return sonora::may_arm_track(h.current(), engine_track, why);
        });
    CHECK(reg.dispatch("track.disarm_all") == DispatchResult::Ok);
    CHECK(reg.dispatch("track.arm_all") == DispatchResult::Ok);
    CHECK(s.tracks()[0]->rec_armed());
    CHECK(!s.tracks()[1]->rec_armed() && !s.tracks()[2]->rec_armed());
    sonora::CommandContext bass;
    bass.target = 1;
    std::string why;
    CHECK(reg.dispatch("track.toggle_arm", bass, why) == DispatchResult::Failed);
    CHECK(why.find("\"Bass\" is a MIDI track") != std::string::npos);
    CHECK(!s.tracks()[1]->rec_armed());
    return 0;
}

// Tracks A and B have active inserts, C an inactive one. The builder makes
// A and B TrackSource -> InsertChain -> TrackFader, keeps C a TrackOut,
// carries what the resolver bound (slot, generation, store index by A2.5,
// latency) in the chain node, and delays every edge into the bus so all
// three arrive on the slowest: 2048 frames.
int test_build_with_chains() {
    auto session = make_session(3);
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, track_op(100, 101, "A"));
    CHECK_APPLY(h, track_op(102, 103, "B"));
    CHECK_APPLY(h, track_op(104, 105, "C"));
    CHECK_APPLY(h, insert_op(100, 500, 0));
    CHECK_APPLY(h, insert_op(100, 501, 3));
    CHECK_APPLY(h, insert_op(102, 502, 1));
    CHECK_APPLY(h, insert_op(104, 503, 0));
    CHECK_APPLY(h, (SetInsertActive{503, false}));

    sonora::BuildLimits limits;
    limits.engine_tracks = 3;
    limits.max_segments = 64;
    limits.max_chunk_slots = 1024;
    limits.chunk_frames = kChunk;
    limits.window_frames = 1024;
    limits.pool_chunks = 128;
    limits.max_delay_frames = 16384;
    limits.delay_lines = 3;
    const sonora::ChainResolver resolve = [](const TrackEdit& t, uint32_t engine_track,
                                             sonora::ChainBinding& b) {
        b.channel_slot = 10 + engine_track;
        b.channel_generation = 3;
        b.param_store = 20 + engine_track;
        b.node_state = engine_track;
        b.latency_frames = t.id == 100 ? 2048 : 100;
    };
    sonora::PlanContinuity continuity(limits.delay_lines);
    const sonora::SourceSlot no_sources = [](const Source&, uint32_t&, std::string& e) {
        e = "no source expected";
        return false;
    };
    PlaybackPlan* plan = nullptr;
    sonora::PlanReport report;
    std::string error;
    CHECK(sonora::build_plan(h.current(), limits, no_sources, resolve, continuity, 1, plan, report,
                             error));
    const PlaybackPlan& p = *plan;
    CHECK(p.node_count == 9);
    CHECK(p.nodes[0].kind == sonora::NodeKind::TrackSource);
    const sonora::PlanNode& a = p.nodes[1];
    CHECK(a.kind == sonora::NodeKind::InsertChain);
    CHECK(a.chain_id == 100 && a.channel_slot == 10 && a.channel_generation == 3);
    CHECK(a.param_store == 20 && a.latency_frames == 2048 && a.channels == 1);
    CHECK(p.nodes[2].kind == sonora::NodeKind::TrackFader);
    CHECK(p.nodes[4].kind == sonora::NodeKind::InsertChain && p.nodes[4].chain_id == 102);
    CHECK(p.nodes[6].kind == sonora::NodeKind::TrackOut);   // C: inactive insert
    CHECK(p.last_insert_node == 4);
    const sonora::PlanNode& bus = p.nodes[7];
    CHECK(bus.kind == sonora::NodeKind::BusSum && bus.input_edge_count == 3);
    CHECK(p.edges[bus.first_input_edge].delay_frames == 0);
    CHECK(p.edges[bus.first_input_edge + 1].delay_frames == 1948);
    CHECK(p.edges[bus.first_input_edge + 2].delay_frames == 2048);
    CHECK(p.compensation_frames == 2048 && p.compensation_shortfall_frames == 0);
    CHECK(report.compensation_frames == 2048);
    CHECK(report.track_latency == (std::vector<uint32_t>{2048, 100, 0}));
    sonora::destroy_plan(plan);

    // Over a 1000-frame maximum: built anyway, clamped, and the shortfall
    // reported against the tracks it leaves early.
    limits.max_delay_frames = 1000;
    CHECK(sonora::build_plan(h.current(), limits, no_sources, resolve, continuity, 2, plan, report,
                             error));
    CHECK(report.clamped_edges == 2);
    CHECK(report.compensation_shortfall_frames == 1048);
    CHECK(report.track_shortfall == (std::vector<uint32_t>{0, 948, 1048}));
    CHECK(plan->edges[plan->nodes[7].first_input_edge + 2].delay_frames == 1000);
    sonora::destroy_plan(plan);

    // No resolver: every chain passes through, and nothing is delayed.
    CHECK(sonora::build_plan(h.current(), limits, no_sources, 3, plan, report, error));
    CHECK(plan->node_count == 9 && plan->nodes[1].channel_slot == sonora::kNoChannelSlot);
    CHECK(plan->compensation_frames == 0);
    for (uint32_t e = 0; e < plan->edge_count; ++e) CHECK(plan->edges[e].delay_frames == 0);
    sonora::destroy_plan(plan);

    // Review of PR #48 (M1): stable ids come from the model track, so a
    // reorder moves each track's ids and delay line with it.
    limits.max_delay_frames = 16384;
    CHECK(sonora::build_plan(h.current(), limits, no_sources, resolve, continuity, 4, plan, report,
                             error));
    const uint32_t chain_a = plan->nodes[1].stable_id;
    const uint32_t out_c = plan->nodes[6].stable_id;
    const uint32_t line_c = plan->edges[plan->nodes[7].first_input_edge + 2].delay_line;
    CHECK(line_c != sonora::kNoDelayLine);
    sonora::destroy_plan(plan);
    CHECK_APPLY(h, (ReorderTracks{{104, 100, 102}}));
    CHECK(sonora::build_plan(h.current(), limits, no_sources, resolve, continuity, 5, plan, report,
                             error));
    CHECK(plan->nodes[0].kind == sonora::NodeKind::TrackOut);   // C, first now
    CHECK(plan->nodes[0].stable_id == out_c);
    CHECK(plan->nodes[2].kind == sonora::NodeKind::InsertChain && plan->nodes[2].chain_id == 100);
    CHECK(plan->nodes[2].stable_id == chain_a);
    const sonora::PlanEdge& c_into_bus = plan->edges[plan->nodes[7].first_input_edge];
    CHECK(c_into_bus.delay_frames == 2048 && c_into_bus.delay_line == line_c);
    sonora::destroy_plan(plan);
    return 0;
}

// rt audit of PR #48: a plan cancelled before it plays must leave the
// delay-line pool as the playing plan left it. Otherwise it can move an
// edge that is still playing to another line, which restarts from silence.
int test_cancelled_plan_leaves_the_pool_alone() {
    auto session = make_session(3);
    Session& s = *session;
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, track_op(100, 101, "A"));
    CHECK_APPLY(h, track_op(102, 103, "B"));
    CHECK_APPLY(h, track_op(104, 105, "C"));
    CHECK_APPLY(h, insert_op(100, 500, 0));
    PlaybackController ctl(s, kDir);
    // Bound (a slot and a node state), or the builder lays the chain out
    // unbound with latency 0 (PR #59).
    ctl.set_chain_resolver([](const TrackEdit&, uint32_t engine_track, sonora::ChainBinding& b) {
        b.channel_slot = engine_track;
        b.node_state = engine_track;
        b.latency_frames = 256;
    });
    std::string error;
    CHECK(show(ctl, h.current(), error));
    CHECK(ctl.settle(2.0));
    const uint32_t lines = s.delay_lines();
    std::vector<uint64_t> owners;
    for (uint32_t l = 0; l < lines; ++l) owners.push_back(ctl.playing_continuity().pool.owner(l));

    // Pending, never published: A's insert moves to C, so B and A need
    // lines and C's goes to one of them.
    CHECK_APPLY(h, RemoveInsert{500});
    CHECK_APPLY(h, insert_op(104, 501, 0));
    CHECK(ctl.show(h.current(), error));
    CHECK(ctl.pending());
    for (uint32_t l = 0; l < lines; ++l) {
        CHECK(ctl.playing_continuity().pool.owner(l) == owners[l]);
    }
    CHECK(ctl.settle(2.0));
    // Published now: the next build starts from it.
    bool moved = false;
    for (uint32_t l = 0; l < lines; ++l) {
        moved = moved || ctl.playing_continuity().pool.owner(l) != owners[l];
    }
    CHECK(moved);
    return 0;
}

// A host reports a new latency: the controller builds a new plan with it,
// which plays once published, keeping the dry track's delay line; the
// playing report, which the state block shows, follows the swap and not
// the build.
int test_latency_change_is_a_new_plan() {
    auto session = make_session(2);
    Session& s = *session;
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, track_op(100, 101, "A"));
    CHECK_APPLY(h, track_op(102, 103, "B"));
    CHECK_APPLY(h, insert_op(100, 500, 0));
    PlaybackController ctl(s, kDir);
    uint32_t latency = 256;
    ctl.set_chain_resolver([&latency](const TrackEdit&, uint32_t engine_track, sonora::ChainBinding& b) {
        b.channel_slot = engine_track;
        b.node_state = engine_track;
        b.latency_frames = latency;
    });
    std::string error;
    CHECK(show(ctl, h.current(), error));
    CHECK(ctl.settle(2.0));
    CHECK(ctl.playing_report().compensation_frames == 256);
    const PlaybackPlan* first = s.plans().current();
    const uint32_t line = first->edges[first->nodes[4].first_input_edge + 1].delay_line;
    CHECK(line != sonora::kNoDelayLine);

    latency = 512;
    CHECK(ctl.show(h.current(), error));
    CHECK(ctl.last_report().compensation_frames == 512);
    CHECK(ctl.settle(2.0));
    CHECK(ctl.playing_report().compensation_frames == 512);
    CHECK(ctl.playing_report().track_latency[0] == 512);
    const PlaybackPlan* second = s.plans().current();
    CHECK(ctl.plans_published() == 2);
    const sonora::PlanEdge& dry = second->edges[second->nodes[4].first_input_edge + 1];
    CHECK(dry.delay_frames == 512 && dry.delay_line == line);
    return 0;
}

// PR #59 re-review (1): the report shows the latency the plan carries. A
// binding with a slot but no node state is laid out unbound, latency 0, so
// the state block must say 0 for its track, not the 256 the resolver gave.
int test_report_shows_the_planned_latency() {
    auto session = make_session(2);
    Session& s = *session;
    History h(new_session(SessionSettings{}));
    CHECK_APPLY(h, track_op(100, 101, "A"));
    CHECK_APPLY(h, track_op(102, 103, "B"));
    CHECK_APPLY(h, insert_op(100, 500, 0));
    CHECK_APPLY(h, insert_op(102, 501, 0));
    PlaybackController ctl(s, kDir);
    ctl.set_chain_resolver([](const TrackEdit&, uint32_t engine_track, sonora::ChainBinding& b) {
        b.channel_slot = engine_track;
        b.latency_frames = 256;
        if (engine_track == 1) b.node_state = engine_track;
    });
    std::string error;
    CHECK(show(ctl, h.current(), error));
    CHECK(ctl.settle(2.0));
    CHECK(ctl.playing_report().track_latency[0] == 0);
    CHECK(ctl.playing_report().track_latency[1] == 256);
    CHECK(ctl.playing_report().compensation_frames == 256);
    return 0;
}

}  // namespace

int main() {
    std::filesystem::remove_all(kDir);
    ::mkdir(kDir.c_str(), 0775);
    ::mkdir((kDir + "/Audio Files").c_str(), 0775);
    if (test_build_from_model()) return 1;
    if (test_crossfade_past_the_handle_is_refused()) return 1;
    if (test_limits_are_named()) return 1;
    if (test_undo_while_rolling()) return 1;
    if (test_locate_and_cancel()) return 1;
    if (test_transport_commands()) return 1;
    if (test_take_origin_is_first_recorded_frame()) return 1;
    if (test_takes_closed_while_rolling_join_the_document()) return 1;
    if (test_failure_in_a_take_punched_out_is_reported()) return 1;
    if (test_failure_does_not_follow_into_the_next_take()) return 1;
    if (test_unplaceable_track_keeps_the_earlier_failure()) return 1;
    if (test_return_to_zero_then_record()) return 1;
    if (test_pending_locate_dropped_by_a_take()) return 1;
    if (test_record_waits_for_residency()) return 1;
    if (test_no_input_monitoring_persists()) return 1;
    if (test_show_leaves_the_faders_alone()) return 1;
    if (test_tracks_know_their_document_ids()) return 1;
    if (test_undone_track_leaves_no_solo()) return 1;
    if (test_solo_safe_reaches_the_track()) return 1;
    if (test_removed_track_stays_muted_until_the_swap()) return 1;
    if (test_sample_rate_must_match()) return 1;
    if (test_locate_serial_is_monotonic()) return 1;
    if (test_huge_gain_is_no_nan()) return 1;
    if (test_midi_tracks_build_silent()) return 1;
    if (test_midi_tracks_are_never_monitored_or_armed()) return 1;
    if (test_build_with_chains()) return 1;
    if (test_latency_change_is_a_new_plan()) return 1;
    if (test_report_shows_the_planned_latency()) return 1;
    if (test_cancelled_plan_leaves_the_pool_alone()) return 1;
    std::filesystem::remove_all(kDir);
    std::printf("playback_control: ok\n");
    return 0;
}
