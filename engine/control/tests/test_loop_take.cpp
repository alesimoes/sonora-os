// SPDX-License-Identifier: GPL-3.0-or-later
//
// A loop take reaches the document (ADR-0016 §3, §5): one AddSource and one
// RecordLoopTake per track, one undo step; every pass on its own playlist,
// the last complete one on the active playlist (O1) with the punch
// crossfades where the take has the handle, a crossfade it has none for
// left out and said (C6); wraps a full boundary ring lost placed by loop
// length and said (C2); passes past the cap left in the files and said
// (O2). The record commands start one when the document's loop range is
// set, refuse it with a punch range set too (C3), and end it on punch.
#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "sonora/closed_take.h"
#include "sonora/command.h"
#include "sonora/model/replication.h"
#include "sonora/session.h"
#include "sonora/session_commands.h"
#include "sonora/wav_writer.h"
#include "sonora/wrap_sidecar.h"

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
using namespace sonora;

bool mentions(const std::string& haystack, const std::string& needle) {
    if (haystack.find(needle) != std::string::npos) return true;
    std::fprintf(stderr, "  expected \"%s\" in: %s\n", needle.c_str(), haystack.c_str());
    return false;
}

bool any_mentions(const std::vector<std::string>& lines, const std::string& needle) {
    for (const std::string& l : lines) {
        if (l.find(needle) != std::string::npos) return true;
    }
    std::fprintf(stderr, "  expected \"%s\" in one of %zu notices:\n", needle.c_str(), lines.size());
    for (const std::string& l : lines) std::fprintf(stderr, "    %s\n", l.c_str());
    return false;
}

// A = 2000, B = 4000: two blocks of max_block_frames (1024) and more.
constexpr uint64_t kA = 2000, kB = 4000, kLen = kB - kA;

struct Rig {
    fs::path dir;
    SessionConfig cfg;
    std::unique_ptr<Session> session;
    std::vector<float> in;
    std::vector<const float*> inputs;

    Rig(const char* name, int tracks, uint32_t boundary_records = 0, uint32_t cap = 99) {
        dir = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(dir, ec);
        cfg.dir = dir.string();
        cfg.track_count = tracks;
        cfg.ring_seconds = 1.0;
        cfg.boundary_records = boundary_records;
        cfg.loop_pass_cap = cap;
        session = std::make_unique<Session>(cfg);
        session->create_dirs();
        for (auto& t : session->tracks()) t->set_rec_armed(true);
        in.assign(1024, 0.25f);
        inputs.assign(static_cast<std::size_t>(tracks), in.data());
    }
    ~Rig() {
        session.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    void block(uint32_t frames, bool drain = true) {
        backend::AudioBlock b{};
        b.inputs = inputs.data();
        b.input_count = static_cast<uint32_t>(inputs.size());
        b.nframes = frames;
        session->process(b);
        if (drain) session->disk().drain_once();
    }

    // A loop take of `frames` frames from A - pre_roll, in blocks of `n`.
    bool loop_take(uint64_t pre_roll, uint64_t frames, uint32_t n = 250) {
        LoopRange l;
        l.from = kA;
        l.to = kB;
        l.start = kA - pre_roll;
        std::string why;
        if (!session->start_loop(session->next_free_take(), l, &why)) {
            std::fprintf(stderr, "  %s\n", why.c_str());
            return false;
        }
        for (uint64_t done = 0; done < frames; done += n) block(n);
        return session->stop();
    }

    Session::ClosedTake take() {
        std::vector<Session::ClosedTake> closed = session->closed_takes();
        return closed.size() == 1 ? closed[0] : Session::ClosedTake{};
    }
};

const model::Playlist* active(const model::Snapshot& s, std::size_t track) {
    const model::TrackEdit& t = *s.edit->tracks[track];
    for (const auto& p : t.playlists) {
        if (p->id == t.active_playlist) return p.get();
    }
    return nullptr;
}

// Three whole passes and one cut short, on two tracks: 2 + 2 members, one
// step; P1 to P4 in pass order, P3 on the active playlist.
int test_passes_go_on_their_playlists() {
    Rig rig("sonora_test_loop_take_passes", 2);
    // From 1500: wraps at file frames 2500, 4500 and 6500; stopped at 6750.
    CHECK(rig.loop_take(500, 6750));
    const Session::ClosedTake take = rig.take();
    CHECK(take.looped && take.loop_wraps == 3);
    model::Document doc(engine_document(*rig.session));
    const TakeAdded r = add_closed_take(doc, *rig.session, take);
    for (const auto& n : r.track_notices) std::fprintf(stderr, "  notice: %s\n", n.c_str());
    CHECK(r.track_notices.empty() && r.refused.empty());
    CHECK(r.updates.size() == 1);
    CHECK(doc.history().undo_label() == "Loop Record");
    CHECK(doc.history().undo_depth() == 1);
    const model::Snapshot& s = doc.current();
    for (std::size_t t = 0; t < 2; ++t) {
        const model::TrackEdit& tr = *s.edit->tracks[t];
        CHECK(tr.playlists.size() == 5);
        const uint64_t offsets[] = {500, 2500, 4500, 6500};
        const uint64_t lengths[] = {kLen, kLen, kLen, 250};
        for (std::size_t k = 0; k < 4; ++k) {
            const model::Playlist& p = *tr.playlists[k + 1];
            CHECK(p.name == tr.name + ".T1.P" + std::to_string(k + 1));
            CHECK(p.regions.size() == 1);
            CHECK(p.regions[0].position == static_cast<model::Frames>(kA));
            CHECK(p.regions[0].source_offset == static_cast<model::Frames>(offsets[k]));
            CHECK(p.regions[0].length == static_cast<model::Frames>(lengths[k]));
        }
        const model::Playlist* a = active(s, t);
        CHECK(a != nullptr && a->regions.size() == 1);
        CHECK(a->regions[0].source_offset == 4500 && a->regions[0].length == kLen);
    }
    // One undo takes the playlists away; the audio stays in the pool.
    model::ReplicationUpdate u;
    std::string why;
    CHECK(doc.undo(u, why));
    CHECK(doc.current().edit->tracks[0]->playlists.size() == 1);
    CHECK(doc.current().sources->sources.size() == 2);
    return 0;
}

// A region beneath the loop on the active playlist, and punch_crossfade set.
model::Document document_under_the_loop(const Session& session, model::Frames crossfade) {
    model::Document doc(engine_document(session));
    model::Snapshot s = doc.current();
    model::SessionSettings st = s.settings;
    st.transport.punch_crossfade = crossfade;
    model::ReplicationUpdate u;
    std::string why;
    doc.apply(model::SetSettings{st}, u, why);
    model::AddSource src;
    src.source.id = doc.current().next_id;
    src.source.path = "Audio Files/bed.wav";
    src.source.channels = 1;
    src.source.length = 100000;
    src.source.kind = model::SourceKind::Imported;
    doc.apply(src, u, why);
    model::Region r;
    r.id = doc.current().next_id;
    r.name = "bed";
    r.position = 0;
    r.length = 20000;
    r.source_offset = 1000;
    r.channels = {{src.source.id, 0}};
    doc.apply(model::PlaceRegion{active(doc.current(), 0)->id, r, 0}, u, why);
    return doc;
}

// C6: a take with handle either side of the chosen pass gets both
// crossfades; one stopped on its first wrap with no pre-roll has neither,
// says so for each, and still goes in.
int test_crossfades_need_the_take_s_handle() {
    {
        Rig rig("sonora_test_loop_take_handles", 1);
        CHECK(rig.loop_take(500, 6750));
        model::Document doc = document_under_the_loop(*rig.session, 240);
        const TakeAdded r = add_closed_take(doc, *rig.session, rig.take());
        CHECK(r.track_notices.empty() && r.refused.empty());
        const model::Playlist* a = active(doc.current(), 0);
        CHECK(a != nullptr && a->regions.size() == 3 && a->crossfades.size() == 2);
        CHECK(a->regions[1].position == static_cast<model::Frames>(kA));
        CHECK(a->regions[1].source_offset == 4500);
    }
    Rig rig("sonora_test_loop_take_no_handles", 1);
    // From A, stopped on the first wrap: pass 1 is the whole file.
    CHECK(rig.loop_take(0, kLen));
    const Session::ClosedTake take = rig.take();
    CHECK(take.loop_wraps == 1);
    model::Document doc = document_under_the_loop(*rig.session, 240);
    const TakeAdded r = add_closed_take(doc, *rig.session, take);
    CHECK(r.refused.empty());
    CHECK(r.track_notices.size() == 2);
    CHECK(mentions(r.track_notices[0], "take 1 (track 1) has no crossfade at the loop start, "
                                       "frame 2000: pass 1 has no audio before it"));
    CHECK(mentions(r.track_notices[1], "at the loop end, frame 4000: pass 1 has no audio after it"));
    const model::Playlist* a = active(doc.current(), 0);
    CHECK(a != nullptr && a->regions.size() == 3 && a->crossfades.empty());
    CHECK(a->regions[1].source_offset == 0 && a->regions[1].length == kLen);
    return 0;
}

// C2: a Wrap record the full ring lost costs nothing on the timeline: the
// pass is placed by loop length, where it would have been, and said.
int test_a_lost_wrap_is_placed_by_loop_length() {
    // Four opens stay undrained in a ring of seven, so the first wrap's
    // four records find room for three.
    Rig rig("sonora_test_loop_take_lost", 4, 8);
    LoopRange l;
    l.from = kA;
    l.to = kB;
    l.start = kA - 500;
    std::string why;
    CHECK(rig.session->start_loop(1, l, &why));
    for (int b = 0; b < 11; ++b) rig.block(250, false);   // 2750 frames: wrap 1 at 2500
    CHECK(rig.session->boundary_overruns() == 1);
    for (int b = 11; b < 27; ++b) rig.block(250);         // 6750 frames
    CHECK(rig.session->stop());
    const Session::ClosedTake take = rig.take();
    CHECK(take.loop_wraps == 3);
    int lost = 0;
    for (const auto& f : take.files) lost += f.wraps.size() == 3 && f.wraps[0] == WrapSidecar::kLostWrap;
    CHECK(lost == 1);
    model::Document doc(engine_document(*rig.session));
    const TakeAdded r = add_closed_take(doc, *rig.session, take);
    CHECK(r.refused.empty());
    CHECK(r.track_notices.size() == 1);
    CHECK(mentions(r.track_notices[0], "take 1's passes on track "));
    CHECK(mentions(r.track_notices[0], "were placed by loop length"));
    for (std::size_t t = 0; t < 4; ++t) {
        const model::TrackEdit& tr = *doc.current().edit->tracks[t];
        CHECK(tr.playlists.size() == 5);
        CHECK(tr.playlists[1]->regions[0].source_offset == 500);
        CHECK(tr.playlists[2]->regions[0].source_offset == 2500);
    }
    return 0;
}

// Code review of PR #117, N1. Track 2's Open was lost to a full ring until
// after wrap 1, so its window opened inside pass 2, at A + 250, and its list
// is [lost, 1750, 3750] (DiskThread::wrap() keeps wrap 1's place). Its
// playlists are named by the take's pass, P2 to P4, under track 1's P2 to
// P4, not renumbered from P1.
int test_a_late_window_keeps_the_take_s_pass_numbers() {
    Rig rig("sonora_test_loop_take_late", 2);
    CHECK(rig.loop_take(500, 6750));
    Session::ClosedTake take = rig.take();
    CHECK(take.loop_wraps == 3);
    CHECK(take.files.size() == 2);
    // Track 2's file as that window leaves it: from block 11 of 27.
    DiskThread::ClosedFile& f = take.files[0].track == 1 ? take.files[0] : take.files[1];
    CHECK(f.track == 1);
    const uint64_t late_frames = 6750 - 2750;
    fs::remove(f.path);
    fs::remove(WrapSidecar::path_for(f.path));
    {
        WavWriter w;
        CHECK(w.open(f.path, rig.cfg.sample_rate, 1, rig.cfg.format, kA + 250));
        const std::vector<float> silence(late_frames, 0.0f);
        CHECK(w.write(silence.data(), late_frames));
        CHECK(w.close());
    }
    f.frames = late_frames;
    f.wraps = {WrapSidecar::kLostWrap, 1750, 3750};

    model::Document doc(engine_document(*rig.session));
    const TakeAdded r = add_closed_take(doc, *rig.session, take);
    CHECK(r.refused.empty());
    const model::TrackEdit& first = *doc.current().edit->tracks[0];
    const model::TrackEdit& late = *doc.current().edit->tracks[1];
    CHECK(first.playlists.size() == 5);
    for (std::size_t k = 1; k <= 4; ++k) {
        CHECK(first.playlists[k]->name == first.name + ".T1.P" + std::to_string(k));
    }
    CHECK(late.playlists.size() == 4);
    for (std::size_t k = 1; k <= 3; ++k) {
        CHECK(late.playlists[k]->name == late.name + ".T1.P" + std::to_string(k + 1));
    }
    CHECK(late.playlists[1]->regions[0].position == static_cast<model::Frames>(kA + 250));
    CHECK(late.playlists[1]->regions[0].length == 1750);
    CHECK(late.playlists[2]->regions[0].source_offset == 1750);
    return 0;
}

// O2: past the cap the passes stay in the file, and the stop says how many.
int test_passes_past_the_cap_are_said() {
    Rig rig("sonora_test_loop_take_cap", 1, 0, 2);
    CHECK(rig.loop_take(500, 6750));
    model::Document doc(engine_document(*rig.session));
    const TakeAdded r = add_closed_take(doc, *rig.session, rig.take());
    CHECK(r.refused.empty());
    CHECK(any_mentions(r.track_notices, "take 1 recorded 4 passes; the first 2 are on playlists, "
                                        "and the other 2 are in its files and wrap lists"));
    const model::TrackEdit& tr = *doc.current().edit->tracks[0];
    CHECK(tr.playlists.size() == 3);
    // The last complete pass the cap keeps.
    CHECK(active(doc.current(), 0)->regions.size() == 1);
    CHECK(active(doc.current(), 0)->regions[0].source_offset == 2500);
    return 0;
}

// O1: a take stopped before its first wrap has one pass, cut short, on its
// own playlist only; one stopped in its pre-roll has none, and says so.
int test_no_complete_pass() {
    {
        Rig rig("sonora_test_loop_take_short", 1);
        CHECK(rig.loop_take(500, 1500));
        model::Document doc(engine_document(*rig.session));
        const TakeAdded r = add_closed_take(doc, *rig.session, rig.take());
        CHECK(r.track_notices.empty() && r.updates.size() == 1);
        CHECK(active(doc.current(), 0)->regions.empty());
        const model::TrackEdit& tr = *doc.current().edit->tracks[0];
        CHECK(tr.playlists.size() == 2);
        CHECK(tr.playlists[1]->regions[0].source_offset == 500);
        CHECK(tr.playlists[1]->regions[0].length == 1000);
    }
    Rig rig("sonora_test_loop_take_pre_roll", 1);
    CHECK(rig.loop_take(500, 250));
    model::Document doc(engine_document(*rig.session));
    const TakeAdded r = add_closed_take(doc, *rig.session, rig.take());
    CHECK(r.track_notices.size() == 1);
    CHECK(mentions(r.track_notices[0], "take 1 stopped in its pre-roll, before the loop start at "
                                       "frame 2000"));
    CHECK(doc.current().sources->sources.size() == 1);
    CHECK(doc.history().undo_depth() == 0);
    return 0;
}

model::Snapshot with_ranges(std::array<model::Frames, 2> loop, std::array<model::Frames, 2> punch,
                            model::Frames pre_roll, bool loop_playback = true) {
    model::SessionSettings st;
    st.name = "Loop";
    st.transport.loop = loop;
    st.transport.loop_playback = loop_playback;
    st.transport.punch = punch;
    st.transport.pre_roll = pre_roll;
    return model::new_session(st);
}

int test_the_document_loop() {
    LoopRange l;
    std::string refusal;
    CHECK(!document_loop(with_ranges({0, 0}, {0, 0}, 96000), 0, l, refusal) && refusal.empty());
    // A range kept with loop playback off records straight through, and
    // a punch range beside it is no conflict.
    CHECK(!document_loop(with_ranges({48000, 96000}, {0, 0}, 96000, false), 0, l, refusal) &&
          refusal.empty());
    CHECK(!document_loop(with_ranges({48000, 96000}, {100000, 200000}, 0, false), 0, l, refusal) &&
          refusal.empty());
    CHECK(document_loop(with_ranges({48000, 96000}, {0, 0}, 96000), 7, l, refusal));
    CHECK(l.from == 48000 && l.to == 96000 && l.start == 0 && l.latency == 7);
    // H = max(punch_crossfade, 1 s), as for a punch.
    CHECK(l.handle == 48000);
    CHECK(document_loop(with_ranges({480000, 960000}, {0, 0}, 96000), 0, l, refusal));
    CHECK(l.start == 384000);
    // C3: loop and punch together are refused, naming both ranges.
    CHECK(!document_loop(with_ranges({48000, 96000}, {100000, 200000}, 0), 0, l, refusal));
    CHECK(mentions(refusal, "loop record and punch cannot run together: the loop is from "
                            "frame 48000 to frame 96000 and the punch from frame 100000 to "
                            "frame 200000"));
    return 0;
}

// The record commands: with a loop range set a take is a loop take, located
// to its pre-roll; with a punch range too it is refused before any file is
// made; transport.punch ends a loop take as stop does.
int test_record_commands_start_a_loop_take() {
    Rig rig("sonora_test_loop_take_commands", 1);
    Session& s = *rig.session;
    model::Snapshot doc = with_ranges({static_cast<model::Frames>(kA),
                                       static_cast<model::Frames>(kB)}, {0, 0}, 500);
    CommandRegistry reg;
    int take = 1;
    bool quit = false;
    install_session_commands(reg, s, take, quit, nullptr, nullptr, nullptr, nullptr,
                             [&doc](LoopRange& loop, std::string& refusal) {
                                 if (!document_loop(doc, 0, loop, refusal)) return false;
                                 // The rig's blocks are shorter than a second.
                                 loop.handle = 0;
                                 return true;
                             });
    s.disk().start();
    CHECK(reg.dispatch("transport.record") == DispatchResult::Ok);
    CHECK(s.loop_take_running());
    rig.block(250, false);
    CHECK(s.take_origin() == kA - 500);
    for (int b = 0; b < 12; ++b) rig.block(250, false);
    CHECK(reg.dispatch("transport.punch") == DispatchResult::Ok);
    CHECK(s.transport().state() == TransportState::Stopped);
    CHECK(!s.loop_take_running());
    const Session::ClosedTake closed = rig.take();
    CHECK(closed.looped && closed.loop_wraps == 1);

    doc = with_ranges({static_cast<model::Frames>(kA), static_cast<model::Frames>(kB)},
                      {100, 200}, 500);
    s.disk().start();
    std::string why;
    CHECK(reg.dispatch("transport.record", CommandContext{}, why) == DispatchResult::Failed);
    CHECK(mentions(why, "loop record and punch cannot run together"));
    CHECK(s.transport().state() == TransportState::Stopped);
    std::error_code ec;
    std::size_t files = 0;
    for (const auto& e : fs::directory_iterator(rig.dir / "Audio Files", ec)) {
        files += e.path().filename().string().find("_02") != std::string::npos;
    }
    CHECK(files == 0);
    return 0;
}

}  // namespace

int main() {
    if (test_passes_go_on_their_playlists()) return 1;
    if (test_crossfades_need_the_take_s_handle()) return 1;
    if (test_a_lost_wrap_is_placed_by_loop_length()) return 1;
    if (test_a_late_window_keeps_the_take_s_pass_numbers()) return 1;
    if (test_passes_past_the_cap_are_said()) return 1;
    if (test_no_complete_pass()) return 1;
    if (test_the_document_loop()) return 1;
    if (test_record_commands_start_a_loop_take()) return 1;
    std::printf("loop_take: ok\n");
    return 0;
}
