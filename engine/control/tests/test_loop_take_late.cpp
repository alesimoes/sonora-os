// SPDX-License-Identifier: GPL-3.0-or-later
//
// A loop take whose window opened late on a track (its Open lost to a full
// boundary ring): the track's passes keep the take's pass numbers, not
// renumbered from P1, and a window opened past the pass cap puts none of its
// passes on a playlist, whatever its wrap list kept (ADR-0016 §5, ADR-0141).
#include <cstdio>
#include <string>
#include <vector>

#include "loop_take_rig.h"
#include "sonora/wav_writer.h"
#include "sonora/wrap_sidecar.h"

namespace {

using namespace loop_take_test;

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

// BACKLOG Track 1, loop record: a track whose window opens after the take
// has wrapped past the cap holds only passes past it. With a cap of 2 its
// list is [before, before], cut there, and the take's third wrap came
// before the file too: its passes are P4 and P5, both past the cap, so it
// puts none on a playlist, not one labelled P3.
int test_a_late_window_past_the_cap_places_no_pass() {
    // Two opens in a ring of one: track 2's waits, undrained, until after
    // wrap 3 at file frame 6500; the take runs on to 8750, past wrap 4,
    // whose record for track 2 the same ring loses.
    Rig rig("sonora_test_loop_take_late_cap", 2, 2, 2);
    LoopRange l;
    l.from = kA;
    l.to = kB;
    l.start = kA - 500;
    std::string why;
    CHECK(rig.session->start_loop(1, l, &why));
    for (int b = 0; b < 27; ++b) rig.block(250, false);   // 6750 frames
    for (int b = 27; b < 35; ++b) rig.block(250);         // 8750 frames
    CHECK(rig.session->stop());
    const Session::ClosedTake take = rig.take();
    CHECK(take.loop_wraps == 4);
    CHECK(take.files.size() == 2);
    const DiskThread::ClosedFile& f = take.files[0].track == 1 ? take.files[0] : take.files[1];
    CHECK(f.track == 1);
    // Its Open finds room in the block after the first drain: the file
    // starts at 7000, A + 500 in pass 4.
    CHECK(f.frames == 8750 - 7000);
    CHECK((f.wraps == std::vector<uint64_t>{WrapSidecar::kBeforeFile, WrapSidecar::kBeforeFile}));
    CHECK(f.wraps_before_file == 3);

    model::Document doc(engine_document(*rig.session));
    const TakeAdded r = add_closed_take(doc, *rig.session, take);
    CHECK(r.refused.empty());
    const model::TrackEdit& first = *doc.current().edit->tracks[0];
    const model::TrackEdit& late = *doc.current().edit->tracks[1];
    CHECK(first.playlists.size() == 3);
    if (late.playlists.size() != 1) {
        for (const auto& p : late.playlists) std::fprintf(stderr, "  late: %s\n", p->name.c_str());
    }
    CHECK(late.playlists.size() == 1);
    CHECK(active(doc.current(), 1)->regions.empty());
    // Its audio is in the pool, and the stop says where the rest went.
    CHECK(doc.current().sources->sources.size() == 2);
    CHECK(any_mentions(r.track_notices, "take 1 recorded 5 passes; the first 2 are on playlists"));
    return 0;
}

// ADR-0141, with a cap of 3. Two opens in a ring of one: track 2's waits,
// undrained, through `held` blocks of 250 and opens two blocks later; from
// there every block drains, and track 2's wrap records lose to track 1's.
bool late_take(Rig& rig, int held, int blocks) {
    LoopRange l;
    l.from = kA;
    l.to = kB;
    l.start = kA - 500;
    std::string why;
    if (!rig.session->start_loop(1, l, &why)) return false;
    for (int b = 0; b < blocks; ++b) rig.block(250, b >= held);
    return rig.session->stop();
}

const DiskThread::ClosedFile* file_of(const Session::ClosedTake& take, int track) {
    for (const auto& f : take.files) {
        if (f.track == track) return &f;
    }
    return nullptr;
}

// Exactly these passes on playlists, by the take's numbers.
bool names_are(const model::TrackEdit& t, const std::vector<int>& want) {
    bool ok = t.playlists.size() == want.size() + 1;
    for (std::size_t k = 0; ok && k < want.size(); ++k) {
        ok = t.playlists[k + 1]->name == t.name + ".T1.P" + std::to_string(want[k]);
    }
    if (!ok) {
        for (const auto& p : t.playlists) std::fprintf(stderr, "  got: %s\n", p->name.c_str());
    }
    return ok;
}

// Opened after wrap 3 and stopped in pass 4, its list [before x3] is whole;
// opened after wrap 4, its fourth before-file place is past the list's cap
// and only on disk. Either way none of its passes is on a playlist.
int test_a_late_window_past_a_cap_of_3() {
    struct Case {
        const char* name;
        int held, blocks;
        uint64_t before, past_cap;
        const char* said;
    };
    const Case cases[] = {
        {"sonora_test_loop_take_late_3", 27, 33, 3, 0, "take 1 recorded 4 passes; the first 3"},
        {"sonora_test_loop_take_late_3_past", 35, 43, 4, 1,
         "take 1 recorded 6 passes; the first 3"}};
    for (const Case& c : cases) {
        Rig rig(c.name, 2, 2, 3);
        CHECK(late_take(rig, c.held, c.blocks));
        const Session::ClosedTake take = rig.take();
        const DiskThread::ClosedFile* f = file_of(take, 1);
        CHECK(f != nullptr);
        CHECK(f->frames == static_cast<uint64_t>(c.blocks - c.held - 1) * 250);
        CHECK(f->wraps == std::vector<uint64_t>(3, WrapSidecar::kBeforeFile));
        CHECK(f->wraps_before_file == c.before && f->wraps_past_cap == c.past_cap);
        WrapList list;
        std::string error;
        CHECK(read_wrap_sidecar(WrapSidecar::path_for(f->path), list, error));
        CHECK(list.wraps == std::vector<uint64_t>(c.before, WrapSidecar::kBeforeFile));

        model::Document doc(engine_document(*rig.session));
        const TakeAdded r = add_closed_take(doc, *rig.session, take);
        CHECK(r.refused.empty());
        CHECK(names_are(*doc.current().edit->tracks[0], {1, 2, 3}));
        CHECK(names_are(*doc.current().edit->tracks[1], {}));
        CHECK(active(doc.current(), 1)->regions.empty());
        CHECK(any_mentions(r.track_notices, c.said));
    }
    return 0;
}

// Opened in pass 2 of six, cap 3: P2 and P3 go on playlists under track 1's,
// P4 to P6 stay in the file and are said, and track 1's list on disk still
// holds wraps 4 and 5, past the cap.
int test_a_late_window_under_a_cap_of_3_keeps_its_numbers() {
    Rig rig("sonora_test_loop_take_late_3_kept", 2, 2, 3);
    CHECK(late_take(rig, 11, 43));
    const Session::ClosedTake take = rig.take();
    CHECK(take.loop_wraps == 5);
    const DiskThread::ClosedFile* first = file_of(take, 0);
    CHECK(first != nullptr && first->wraps_past_cap == 2);
    WrapList list;
    std::string error;
    CHECK(read_wrap_sidecar(WrapSidecar::path_for(first->path), list, error));
    CHECK((list.wraps == std::vector<uint64_t>{WrapSidecar::kLostWrap, 4500, 6500, 8500, 10500}));

    model::Document doc(engine_document(*rig.session));
    const TakeAdded r = add_closed_take(doc, *rig.session, take);
    CHECK(r.refused.empty());
    CHECK(names_are(*doc.current().edit->tracks[0], {1, 2, 3}));
    const model::TrackEdit& late = *doc.current().edit->tracks[1];
    CHECK(names_are(late, {2, 3}));
    // Opened at file frame 3000, A + 500 in pass 2.
    CHECK(late.playlists[1]->regions[0].position == static_cast<model::Frames>(kA + 500));
    CHECK(late.playlists[1]->regions[0].length == static_cast<model::Frames>(kLen - 500));
    CHECK(late.playlists[2]->regions[0].source_offset == static_cast<model::Frames>(kLen - 500));
    CHECK(any_mentions(r.track_notices, "take 1 recorded 6 passes; the first 3 are on playlists"));
    return 0;
}

}  // namespace

int main() {
    if (test_a_late_window_keeps_the_take_s_pass_numbers()) return 1;
    if (test_a_late_window_past_the_cap_places_no_pass()) return 1;
    if (test_a_late_window_past_a_cap_of_3()) return 1;
    if (test_a_late_window_under_a_cap_of_3_keeps_its_numbers()) return 1;
    std::printf("loop_take_late: ok\n");
    return 0;
}
