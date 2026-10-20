// SPDX-License-Identifier: GPL-3.0-or-later
//
// A closed take reaches the document as one compound: one update, one undo
// step, every armed track (ADR-0009 update 2026-10-14, item 8). A take is
// never all-or-nothing: a track whose file does not read costs that track
// and says so by name; a take larger than one compound is split into
// compounds within the limit; and if the model refuses the take, its audio
// still goes into the pool, so a save keeps it, and the engine says loudly
// that it is not on the timeline. A punched take goes in over its punch
// range [A, B) only, with its crossfades where it meets what it replaced
// (timeline-model.md, "Punch in").
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "sonora/closed_take.h"
#include "sonora/model/replication.h"
#include "sonora/session.h"
#include "sonora/wav_writer.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using namespace sonora;

bool mentions(const std::string& haystack, const std::string& needle) {
    if (haystack.find(needle) != std::string::npos) return true;
    std::fprintf(stderr, "  expected \"%s\" in: %s\n", needle.c_str(), haystack.c_str());
    return false;
}

// More tracks than any test records.
constexpr int kMaxTracks = 40;

void wipe(const std::string& dir) {
    for (int t = 1; t <= kMaxTracks; ++t) {
        const std::string f = dir + "/Audio Files/Audio " + std::to_string(t) + "_01.wav";
        std::remove(f.c_str());
    }
    ::rmdir((dir + "/Audio Files").c_str());
    ::rmdir(dir.c_str());
}

SessionConfig config(const char* dir, int tracks = 3) {
    SessionConfig cfg;
    cfg.dir = dir;
    cfg.track_count = tracks;
    cfg.ring_seconds = 0.01;
    return cfg;
}

// Take 1 on every track: 64 frames each, through the disk thread.
bool record(Session& s) {
    if (!s.create_dirs()) return false;
    for (auto& t : s.tracks()) t->set_rec_armed(true);
    s.disk().start();
    if (!s.start_recording(1)) return false;
    float block[64];
    for (float& f : block) f = 0.125f;
    const std::vector<const float*> inputs(s.tracks().size(), block);
    backend::AudioBlock b{};
    b.inputs = inputs.data();
    b.input_count = static_cast<uint32_t>(inputs.size());
    b.nframes = 64;
    s.process(b);
    return s.stop();
}

// The one take record() closed, as the disk thread reports it.
Session::ClosedTake the_take(Session& s) {
    std::vector<Session::ClosedTake> closed = s.closed_takes();
    return closed.size() == 1 ? closed[0] : Session::ClosedTake{};
}

std::string file_of(const SessionConfig& cfg, int track) {
    return cfg.dir + "/Audio Files/Audio " + std::to_string(track) + "_01.wav";
}

std::size_t regions_on(const model::Snapshot& s, std::size_t track) {
    const model::TrackEdit& t = *s.edit->tracks[track];
    for (const auto& p : t.playlists) {
        if (p->id == t.active_playlist) return p->regions.size();
    }
    return 0;
}

// The one region on track `track`'s active playlist; nullptr if not one.
const model::Region* only_region(const model::Snapshot& s, std::size_t track) {
    const model::TrackEdit& t = *s.edit->tracks[track];
    for (const auto& p : t.playlists) {
        if (p->id == t.active_playlist && p->regions.size() == 1) return &p->regions[0];
    }
    return nullptr;
}

const model::Source* source_by_id(const model::Snapshot& s, model::Id id) {
    for (const model::Source& src : s.sources->sources) {
        if (src.id == id) return &src;
    }
    return nullptr;
}

constexpr std::size_t kSplitTracks = 8;
constexpr std::size_t kFilesPerPart = 3;   // parts of 3, 3 and 2 files

int test_a_take_is_one_compound() {
    const SessionConfig cfg = config("test_closed_take_whole");
    wipe(cfg.dir);
    {
        Session s(cfg);
        CHECK(record(s));
        model::Document doc(engine_document(s));
        const std::uint64_t rev = doc.current().revision;
        const TakeAdded r = add_closed_take(doc, s, the_take(s));
        CHECK(r.track_notices.empty() && r.refused.empty());
        CHECK(r.updates.size() == 1);
        CHECK(r.updates[0].revision == rev + 1);
        CHECK(doc.history().undo_depth() == 1);
        for (std::size_t t = 0; t < 3; ++t) CHECK(regions_on(doc.current(), t) == 1);
        CHECK(doc.current().sources->sources.size() == 3);
        // One undo removes the take from every track; its audio stays.
        model::ReplicationUpdate u;
        std::string why;
        CHECK(doc.undo(u, why));
        for (std::size_t t = 0; t < 3; ++t) CHECK(regions_on(doc.current(), t) == 0);
        CHECK(doc.current().sources->sources.size() == 3);
    }
    wipe(cfg.dir);
    return 0;
}

// Track 2's file does not read: tracks 1 and 3 land, as one step, and the
// notice names track 2 and its file. It used to drop track 3 as well, and
// say the whole take was not in the session.
int test_one_unreadable_track_costs_that_track() {
    const SessionConfig cfg = config("test_closed_take_bad");
    wipe(cfg.dir);
    {
        Session s(cfg);
        CHECK(record(s));
        std::FILE* f = std::fopen(file_of(cfg, 2).c_str(), "wb");
        CHECK(f != nullptr);
        std::fputs("not a wave", f);
        CHECK(std::fclose(f) == 0);

        model::Document doc(engine_document(s));
        const TakeAdded r = add_closed_take(doc, s, the_take(s));
        CHECK(r.refused.empty());
        CHECK(r.track_notices.size() == 1);
        CHECK(mentions(r.track_notices[0], "Audio 2"));
        CHECK(mentions(r.track_notices[0], "Audio 2_01.wav"));
        CHECK(r.updates.size() == 1);
        CHECK(doc.history().undo_depth() == 1);
        CHECK(regions_on(doc.current(), 0) == 1);
        CHECK(regions_on(doc.current(), 1) == 0);
        CHECK(regions_on(doc.current(), 2) == 1);
        CHECK(doc.current().sources->sources.size() == 2);
    }
    wipe(cfg.dir);
    return 0;
}

// The model refuses the take (every file says it starts past the end of the
// timeline): each source still goes into the pool on its own, nothing is on
// the timeline, and `refused` says so for the engine to show as an error.
int test_a_refused_take_keeps_its_audio_in_the_pool() {
    const SessionConfig cfg = config("test_closed_take_refused");
    wipe(cfg.dir);
    {
        Session s(cfg);
        CHECK(record(s));
        float frames[64];
        for (float& x : frames) x = 0.25f;
        for (int t = 1; t <= 3; ++t) {
            CHECK(std::remove(file_of(cfg, t).c_str()) == 0);
            WavWriter w;
            CHECK(w.open(file_of(cfg, t), cfg.sample_rate, 1, WavWriter::Format::Int24,
                         static_cast<uint64_t>(model::kMaxInteger - 10)));
            CHECK(w.write(frames, 64));
            CHECK(w.close());
        }
        model::Document doc(engine_document(s));
        const TakeAdded r = add_closed_take(doc, s, the_take(s));
        CHECK(r.track_notices.empty());
        CHECK(!r.refused.empty());
        CHECK(mentions(r.refused, "not on the timeline"));
        CHECK(mentions(r.refused, "past the end of the timeline"));
        CHECK(r.updates.size() == 3);   // one AddSource each
        CHECK(doc.current().sources->sources.size() == 3);
        CHECK(doc.history().undo_depth() == 0);
        for (std::size_t t = 0; t < 3; ++t) CHECK(regions_on(doc.current(), t) == 0);
    }
    wipe(cfg.dir);
    return 0;
}

// More armed tracks than one compound holds (two members a track): the take
// is split into compounds within the limit, in track order, and every
// track lands on the timeline. Without the split the one compound was
// refused and every track fell back to the pool.
//
// Since the limit rose past 2 * kMaxTracks members (BACKLOG Track 2 item 8),
// a take that needs the split has more files than this test records; the
// take on kMaxTracks tracks is then checked to land as one compound. The
// split itself is tested below with a smaller files_per_compound.
int test_a_take_larger_than_one_compound_lands_whole() {
    constexpr bool kSplits = model::kMaxCompoundMembers / 2 + 2 <= std::size_t{kMaxTracks};
    constexpr std::size_t kTracks =
        kSplits ? model::kMaxCompoundMembers / 2 + 2 : std::size_t{kMaxTracks};
    const SessionConfig cfg = config("test_closed_take_large", static_cast<int>(kTracks));
    wipe(cfg.dir);
    {
        Session s(cfg);
        CHECK(record(s));
        model::Document doc(engine_document(s));
        const TakeAdded r = add_closed_take(doc, s, the_take(s));
        CHECK(r.track_notices.empty());
        if (!r.refused.empty()) std::fprintf(stderr, "  refused: %s\n", r.refused.c_str());
        CHECK(r.refused.empty());
        // Split: a full compound, then 2 tracks.
        CHECK(r.updates.size() == (kSplits ? 2u : 1u));
        CHECK(doc.history().undo_depth() == (kSplits ? 2u : 1u));
        for (std::size_t t = 0; t < kTracks; ++t) CHECK(regions_on(doc.current(), t) == 1);
        CHECK(doc.current().sources->sources.size() == kTracks);
    }
    wipe(cfg.dir);
    return 0;
}

// A take split into parts of three files on eight tracks, which reaches the
// split without 130 armed tracks: three updates, three undo steps, every
// file on its own track once, in track order, and every id fresh across the
// parts (each part takes its ids above the last part's).
int test_a_split_take_lands_in_track_order() {
    const SessionConfig cfg = config("test_closed_take_split", static_cast<int>(kSplitTracks));
    wipe(cfg.dir);
    {
        Session s(cfg);
        CHECK(record(s));
        model::Document doc(engine_document(s));
        const model::Id first_id = doc.current().next_id;
        const TakeAdded r = add_closed_take(doc, s, the_take(s), kFilesPerPart);
        CHECK(r.track_notices.empty());
        if (!r.refused.empty()) std::fprintf(stderr, "  refused: %s\n", r.refused.c_str());
        CHECK(r.refused.empty());
        CHECK(r.updates.size() == 3);
        CHECK(doc.history().undo_depth() == 3);

        std::vector<model::Id> ids;
        for (std::size_t t = 0; t < kSplitTracks; ++t) {
            const model::Region* region = only_region(doc.current(), t);
            CHECK(region != nullptr);
            CHECK(region->channels.size() == 1);
            const model::Source* src = source_by_id(doc.current(), region->channels[0].source);
            CHECK(src != nullptr);
            CHECK(src->recorded_on_track == doc.current().edit->tracks[t]->id);
            CHECK(mentions(src->path, "Audio " + std::to_string(t + 1) + "_01.wav"));
            ids.push_back(region->id);
            ids.push_back(src->id);
        }
        CHECK(doc.current().sources->sources.size() == kSplitTracks);
        std::sort(ids.begin(), ids.end());
        CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
        CHECK(ids.front() >= first_id);
        CHECK(ids.back() < doc.current().next_id);

        // Undo takes the parts back last first: tracks 7-8, then 4-6.
        model::ReplicationUpdate u;
        std::string why;
        CHECK(doc.undo(u, why));
        for (std::size_t t = 0; t < kSplitTracks; ++t) {
            CHECK(regions_on(doc.current(), t) == (t < 6 ? 1u : 0u));
        }
        CHECK(doc.undo(u, why));
        for (std::size_t t = 0; t < kSplitTracks; ++t) {
            CHECK(regions_on(doc.current(), t) == (t < 3 ? 1u : 0u));
        }
    }
    wipe(cfg.dir);
    return 0;
}

// The second of three parts is refused (its files say they start past the
// end of the timeline). The first and third land as their own steps, the
// second's files go into the pool, and `refused` names its tracks.
int test_a_refused_part_keeps_its_audio_and_the_others_land() {
    const SessionConfig cfg =
        config("test_closed_take_split_refused", static_cast<int>(kSplitTracks));
    wipe(cfg.dir);
    {
        Session s(cfg);
        CHECK(record(s));
        float frames[64];
        for (float& x : frames) x = 0.25f;
        for (int t = 4; t <= 6; ++t) {
            CHECK(std::remove(file_of(cfg, t).c_str()) == 0);
            WavWriter w;
            CHECK(w.open(file_of(cfg, t), cfg.sample_rate, 1, WavWriter::Format::Int24,
                         static_cast<uint64_t>(model::kMaxInteger - 10)));
            CHECK(w.write(frames, 64));
            CHECK(w.close());
        }
        model::Document doc(engine_document(s));
        const TakeAdded r = add_closed_take(doc, s, the_take(s), kFilesPerPart);
        CHECK(r.track_notices.empty());
        CHECK(mentions(r.refused, "take 1 (tracks 4 to 6)"));
        CHECK(mentions(r.refused, "not on the timeline"));
        CHECK(mentions(r.refused, "past the end of the timeline"));
        CHECK(r.refused.find("tracks 1 to 3") == std::string::npos);
        CHECK(r.refused.find("tracks 7 to 8") == std::string::npos);
        CHECK(r.updates.size() == 1 + 3 + 1);   // part 1, an AddSource each, part 3
        CHECK(doc.history().undo_depth() == 2);
        CHECK(doc.current().sources->sources.size() == kSplitTracks);
        for (std::size_t t = 0; t < kSplitTracks; ++t) {
            CHECK(regions_on(doc.current(), t) == (t >= 3 && t < 6 ? 0u : 1u));
        }
    }
    wipe(cfg.dir);
    return 0;
}

// --- punched takes (timeline-model.md, "Punch in") -------------------------

// At 1 kHz one second of handle is 1000 frames, so a whole punch with its
// handles is a few dozen blocks.
constexpr uint32_t kPunchRate = 1000;
constexpr model::Frames kA = 3000, kB = 5000, kH = 1000, kX = 40;
constexpr model::Frames kOldLength = 20000;

SessionConfig punch_config(const char* dir) {
    SessionConfig cfg;
    cfg.dir = dir;
    cfg.track_count = 1;
    cfg.sample_rate = kPunchRate;
    // Holds the whole punch: the disk thread is never what a test waits on.
    cfg.ring_seconds = 20.0;
    return cfg;
}

PunchRange punch_range() {
    PunchRange r;
    r.capture_from = static_cast<uint64_t>(kA - kH);
    r.capture_to = static_cast<uint64_t>(kB + kH);
    r.monitor_in = static_cast<uint64_t>(kA);
    r.monitor_out = static_cast<uint64_t>(kB);
    r.crossfade = static_cast<uint32_t>(kX);
    r.shape = FadeCurve::EqualPower;
    return r;
}

// The take recorded before, as an imported file on track 1 over
// [0, kOldLength): what the punch goes in over.
bool place_old_take(model::Document& doc, const SessionConfig& cfg) {
    WavWriter w;
    if (!w.open(cfg.dir + "/Audio Files/Old.wav", kPunchRate, 1, WavWriter::Format::Int24, 0)) {
        return false;
    }
    const std::vector<float> frames(static_cast<std::size_t>(kOldLength), 0.25f);
    if (!w.write(frames.data(), frames.size()) || !w.close()) return false;
    std::vector<model::ReplicationUpdate> updates;
    std::string error;
    return place_file(doc, cfg.dir, "Audio Files/Old.wav", 0, updates, error);
}

// Blocks of 64 until the playhead reaches `to`.
void roll_to(Session& s, uint64_t to) {
    float block[64];
    for (float& f : block) f = 0.5f;
    const float* inputs[1] = {block};
    backend::AudioBlock b{};
    b.inputs = inputs;
    b.input_count = 1;
    b.nframes = 64;
    while (s.transport().playhead() < to) s.process(b);
}

const model::Playlist& active_playlist(const model::Snapshot& s, std::size_t track) {
    const model::TrackEdit& t = *s.edit->tracks[track];
    for (const auto& p : t.playlists) {
        if (p->id == t.active_playlist) return *p;
    }
    return *t.playlists.front();
}

struct Punched {
    Session::ClosedTake take;
    model::Frames length = 0;   // the file's frames on disk
};

// Arms track 1, starts a punch with `range`, rolls to `stop_at`, stops.
bool punch(Session& s, const PunchRange& range, uint64_t stop_at, Punched& out) {
    for (auto& t : s.tracks()) t->set_rec_armed(true);
    s.disk().start();
    if (!s.start_punch(1, range)) return false;
    roll_to(s, stop_at);
    if (!s.stop()) return false;
    std::vector<Session::ClosedTake> closed = s.closed_takes();
    if (closed.size() != 1 || closed[0].files.size() != 1) return false;
    out.take = closed[0];
    out.length = static_cast<model::Frames>(out.take.files[0].frames);
    return true;
}

// The Phase 2 exit criterion in the engine: a take captured over
// [A - H, B + H) goes in over [A, B) only, splitting the old take into
// [0, A) and [B, ..), with a centred equal-power crossfade of the punch's
// length at A and at B; one undo restores the old take whole.
int test_a_punch_goes_in_over_its_range() {
    const SessionConfig cfg = punch_config("test_closed_take_punch");
    std::filesystem::remove_all(cfg.dir);
    {
        Session s(cfg);
        CHECK(s.create_dirs());
        model::Document doc(engine_document(s));
        CHECK(place_old_take(doc, cfg));
        const model::Region old = active_playlist(doc.current(), 0).regions.at(0);
        Punched p;
        CHECK(punch(s, punch_range(), static_cast<uint64_t>(kB + kH + 64), p));
        CHECK(p.take.punched);
        CHECK(p.length == (kB + kH) - (kA - kH));
        const TakeAdded r = add_closed_take(doc, s, p.take);
        CHECK(r.track_notices.empty() && r.refused.empty());
        CHECK(r.fade_notices.empty());
        CHECK(r.updates.size() == 1);

        const model::Playlist& pl = active_playlist(doc.current(), 0);
        CHECK(pl.regions.size() == 3);
        const model::Region& left = pl.regions[0];
        const model::Region& n = pl.regions[1];
        const model::Region& right = pl.regions[2];
        CHECK(left.id == old.id && left.position == 0 && left.end() == kA);
        CHECK(n.position == kA && n.end() == kB);
        // The take's first frame is A - H on the timeline: A is H into it.
        CHECK(n.source_offset == kH);
        CHECK(right.position == kB && right.end() == kOldLength && right.source_offset == kB);
        CHECK(pl.crossfades.size() == 2);
        CHECK(pl.crossfades[0].left == left.id && pl.crossfades[0].right == n.id);
        CHECK(pl.crossfades[1].left == n.id && pl.crossfades[1].right == right.id);
        for (const model::Crossfade& x : pl.crossfades) {
            CHECK(x.length == kX && x.align == model::Align::Centre &&
                  x.shape == model::FadeShape::EqualPower);
        }

        // One step: undo brings back the old take whole; the punch's audio
        // stays in the pool.
        model::ReplicationUpdate u;
        std::string why;
        CHECK(doc.undo(u, why));
        const model::Playlist& back = active_playlist(doc.current(), 0);
        CHECK(back.regions.size() == 1 && back.regions[0] == old);
        CHECK(back.crossfades.empty());
        CHECK(doc.current().sources->sources.size() == 2);
    }
    std::filesystem::remove_all(cfg.dir);
    return 0;
}

// Stopped inside the range: the take ends where the recording did, with
// the crossfade at A and none at the stop, which is a cut the operator
// made, not a punch point.
int test_a_punch_stopped_inside_its_range_ends_there() {
    const SessionConfig cfg = punch_config("test_closed_take_punch_stopped");
    std::filesystem::remove_all(cfg.dir);
    {
        Session s(cfg);
        CHECK(s.create_dirs());
        model::Document doc(engine_document(s));
        CHECK(place_old_take(doc, cfg));
        Punched p;
        CHECK(punch(s, punch_range(), 4000, p));
        const model::Frames stopped = (kA - kH) + p.length;
        CHECK(stopped > kA && stopped < kB);
        const TakeAdded r = add_closed_take(doc, s, p.take);
        CHECK(r.track_notices.empty() && r.refused.empty());

        const model::Playlist& pl = active_playlist(doc.current(), 0);
        CHECK(pl.regions.size() == 3);
        CHECK(pl.regions[0].end() == kA);
        CHECK(pl.regions[1].position == kA && pl.regions[1].end() == stopped);
        CHECK(pl.regions[2].position == stopped && pl.regions[2].end() == kOldLength);
        CHECK(pl.crossfades.size() == 1);
        CHECK(pl.crossfades[0].right == pl.regions[1].id);
    }
    std::filesystem::remove_all(cfg.dir);
    return 0;
}

// Armed with the playhead already past A: capture starts at the next block,
// and the take starts there, butting the old take with no crossfade (it has
// no handle before its first frame), and crossfades at B.
int test_a_punch_armed_late_starts_where_it_recorded() {
    const SessionConfig cfg = punch_config("test_closed_take_punch_late");
    std::filesystem::remove_all(cfg.dir);
    {
        Session s(cfg);
        CHECK(s.create_dirs());
        model::Document doc(engine_document(s));
        CHECK(place_old_take(doc, cfg));
        s.play();
        roll_to(s, 3500);
        const model::Frames armed_at = static_cast<model::Frames>(s.transport().playhead());
        Punched p;
        CHECK(punch(s, punch_range(), static_cast<uint64_t>(kB + kH + 64), p));
        const TakeAdded r = add_closed_take(doc, s, p.take);
        CHECK(r.track_notices.empty() && r.refused.empty());

        const model::Playlist& pl = active_playlist(doc.current(), 0);
        CHECK(pl.regions.size() == 3);
        CHECK(pl.regions[0].end() == armed_at);
        CHECK(pl.regions[1].position == armed_at && pl.regions[1].end() == kB);
        CHECK(pl.regions[1].source_offset == 0);
        CHECK(pl.crossfades.size() == 1);
        CHECK(pl.crossfades[0].left == pl.regions[1].id);
    }
    std::filesystem::remove_all(cfg.dir);
    return 0;
}

// Over an empty track the take goes in over [A, B) with nothing to
// crossfade with: never refused for a crossfade it cannot have.
int test_a_punch_over_nothing_has_no_crossfade() {
    const SessionConfig cfg = punch_config("test_closed_take_punch_empty");
    std::filesystem::remove_all(cfg.dir);
    {
        Session s(cfg);
        CHECK(s.create_dirs());
        model::Document doc(engine_document(s));
        Punched p;
        CHECK(punch(s, punch_range(), static_cast<uint64_t>(kB + kH + 64), p));
        const TakeAdded r = add_closed_take(doc, s, p.take);
        CHECK(r.track_notices.empty() && r.refused.empty());
        const model::Playlist& pl = active_playlist(doc.current(), 0);
        CHECK(pl.regions.size() == 1);
        CHECK(pl.regions[0].position == kA && pl.regions[0].end() == kB);
        CHECK(pl.crossfades.empty());
    }
    std::filesystem::remove_all(cfg.dir);
    return 0;
}

// A rule only the model checks costs the crossfades, never the take: here
// the old take was edited at 1000 with a crossfade there, and a punch at
// 1030 leaves 30 frames between it and the new one at A, which the model
// refuses (two crossfades overlapping in one region). The take still goes
// in over its range, butt-spliced, and the operator is told.
int test_a_punch_the_model_refuses_crossfades_for_lands_without_them() {
    const SessionConfig cfg = punch_config("test_closed_take_punch_tight");
    std::filesystem::remove_all(cfg.dir);
    {
        Session s(cfg);
        CHECK(s.create_dirs());
        model::Document doc(engine_document(s));
        CHECK(place_old_take(doc, cfg));
        const model::Id playlist = active_playlist(doc.current(), 0).id;
        const model::Id first = active_playlist(doc.current(), 0).regions.at(0).id;
        const model::Id second = doc.current().next_id;
        model::ReplicationUpdate u;
        std::string why;
        CHECK(doc.apply(model::SplitRegion{playlist, first, 1000, second}, u, why));
        model::Crossfade edit{first, second, kX, model::Align::Centre,
                              model::FadeShape::EqualPower};
        CHECK(doc.apply(model::SetCrossfade{playlist, edit}, u, why));

        PunchRange range = punch_range();
        range.monitor_in = 1030;
        range.capture_from = 30;
        Punched p;
        CHECK(punch(s, range, static_cast<uint64_t>(kB + kH + 64), p));
        const TakeAdded r = add_closed_take(doc, s, p.take);
        CHECK(r.refused.empty());
        CHECK(r.track_notices.size() == 1);
        CHECK(mentions(r.track_notices[0], "take 1 was punched in without its crossfades"));
        CHECK(r.updates.size() == 1);

        const model::Playlist& pl = active_playlist(doc.current(), 0);
        CHECK(pl.regions.size() == 4);
        CHECK(pl.regions[1].id == second && pl.regions[1].end() == 1030);
        CHECK(pl.regions[2].position == 1030 && pl.regions[2].end() == kB);
        CHECK(pl.crossfades.size() == 1 && pl.crossfades[0] == edit);
    }
    std::filesystem::remove_all(cfg.dir);
    return 0;
}

// The fade the old take carries, which a take over it clamps or a punch
// fits: fade-in to 10 frames before A, fade-out from 10 frames after B.
bool fade_old_take(model::Document& doc) {
    const model::Playlist& pl = active_playlist(doc.current(), 0);
    const model::Id playlist = pl.id;
    const model::Id region = pl.regions.at(0).id;
    model::ReplicationUpdate u;
    std::string why;
    return doc.apply(model::SetFade{playlist, region, model::FadeEnd::In,
                                    {kA - 10, model::FadeShape::Linear}},
                     u, why) &&
           doc.apply(model::SetFade{playlist, region, model::FadeEnd::Out,
                                    {kOldLength - kB - 10, model::FadeShape::Linear}},
                     u, why);
}

// Pro Tools shortens a fade beside a punch to make room for the punch's
// crossfade and never refuses the record; the model does the same
// (RecordTake fits fades), and the take's report names each one, so the
// operator hears what changed. Here the old take's fade-in runs to 10
// frames before A and its fade-out from 10 frames after B: each meets half
// the crossfade (20 frames) and loses 10. The fade-out the punch cut at A
// is where the crossfade now sits, which is not news, and is not named.
int test_a_punch_names_the_fades_it_shortened() {
    const SessionConfig cfg = punch_config("test_closed_take_punch_fades");
    std::filesystem::remove_all(cfg.dir);
    {
        Session s(cfg);
        CHECK(s.create_dirs());
        model::Document doc(engine_document(s));
        CHECK(place_old_take(doc, cfg));
        CHECK(fade_old_take(doc));

        Punched p;
        CHECK(punch(s, punch_range(), static_cast<uint64_t>(kB + kH + 64), p));
        const TakeAdded r = add_closed_take(doc, s, p.take);
        CHECK(r.refused.empty() && r.track_notices.empty());
        const model::Playlist& pl = active_playlist(doc.current(), 0);
        CHECK(pl.regions.size() == 3 && pl.crossfades.size() == 2);
        CHECK(pl.regions[0].fade_in.length == kA - kX / 2);
        CHECK(pl.regions[2].fade_out.length == kOldLength - kB - kX / 2);

        CHECK(r.fade_notices.size() == 2);
        CHECK(mentions(r.fade_notices[0], "take 1 shortened the fade-in of \"Old\" on track 1 "
                                          "from 2990.0 ms to 2980.0 ms"));
        CHECK(mentions(r.fade_notices[1], "take 1 shortened the fade-out of \"Old\" on track 1 "
                                          "from 14990.0 ms to 14980.0 ms"));
    }
    std::filesystem::remove_all(cfg.dir);
    return 0;
}

// A take that is not a punch has no crossfade. Recorded over [0, 6000) it
// cuts the old take's start, leaving [6000, 20000), shorter than its
// fade-out, which the overwrite clamps: not a crossfade making room, and
// not reported as one (PR #103 review).
int test_an_unpunched_take_names_no_fade() {
    const SessionConfig cfg = punch_config("test_closed_take_unpunched_fades");
    std::filesystem::remove_all(cfg.dir);
    {
        Session s(cfg);
        CHECK(s.create_dirs());
        model::Document doc(engine_document(s));
        CHECK(place_old_take(doc, cfg));
        const model::Playlist& old = active_playlist(doc.current(), 0);
        model::ReplicationUpdate u;
        std::string why;
        CHECK(doc.apply(model::SetFade{old.id, old.regions.at(0).id, model::FadeEnd::Out,
                                       {18000, model::FadeShape::Linear}},
                        u, why));
        for (auto& t : s.tracks()) t->set_rec_armed(true);
        s.disk().start();
        CHECK(s.start_recording(1));
        roll_to(s, 6000);
        CHECK(s.stop());
        std::vector<Session::ClosedTake> closed = s.closed_takes();
        CHECK(closed.size() == 1 && !closed[0].punched);
        const TakeAdded r = add_closed_take(doc, s, closed[0]);
        CHECK(r.refused.empty() && r.track_notices.empty());
        const model::Playlist& pl = active_playlist(doc.current(), 0);
        CHECK(pl.crossfades.empty() && pl.regions.size() == 2);
        // The case that must stay quiet: the fade-out was clamped.
        CHECK(pl.regions[1].fade_out.length == pl.regions[1].length);
        CHECK(pl.regions[1].length < 18000);
        CHECK(r.fade_notices.empty());
    }
    std::filesystem::remove_all(cfg.dir);
    return 0;
}

// A punch the model refused the crossfades for goes in without them: no
// crossfade made room for anything, so no fade is named, even one the
// overwrite clamped (PR #103 review).
int test_a_punch_without_its_crossfades_names_no_fade() {
    const SessionConfig cfg = punch_config("test_closed_take_punch_tight_fades");
    std::filesystem::remove_all(cfg.dir);
    {
        Session s(cfg);
        CHECK(s.create_dirs());
        model::Document doc(engine_document(s));
        CHECK(place_old_take(doc, cfg));
        const model::Id playlist = active_playlist(doc.current(), 0).id;
        const model::Id first = active_playlist(doc.current(), 0).regions.at(0).id;
        const model::Id second = doc.current().next_id;
        model::ReplicationUpdate u;
        std::string why;
        CHECK(doc.apply(model::SplitRegion{playlist, first, 1000, second}, u, why));
        model::Crossfade edit{first, second, kX, model::Align::Centre,
                              model::FadeShape::EqualPower};
        CHECK(doc.apply(model::SetCrossfade{playlist, edit}, u, why));
        // Longer than the part the punch leaves after B, which the
        // overwrite clamps with no crossfade there to blame.
        CHECK(doc.apply(model::SetFade{playlist, second, model::FadeEnd::Out,
                                       {18000, model::FadeShape::Linear}},
                        u, why));

        PunchRange range = punch_range();
        range.monitor_in = 1030;
        range.capture_from = 30;
        Punched p;
        CHECK(punch(s, range, static_cast<uint64_t>(kB + kH + 64), p));
        const TakeAdded r = add_closed_take(doc, s, p.take);
        CHECK(r.refused.empty());
        CHECK(r.track_notices.size() == 1);
        CHECK(mentions(r.track_notices[0], "take 1 was punched in without its crossfades"));
        const model::Playlist& pl = active_playlist(doc.current(), 0);
        CHECK(pl.regions.size() == 4 && pl.regions[3].fade_out.length == kOldLength - kB);
        CHECK(r.fade_notices.empty());
    }
    std::filesystem::remove_all(cfg.dir);
    return 0;
}

// Stopped before A: nothing was punched, so nothing goes on the timeline
// and no undo step is made, but what the handle captured is kept in the
// pool rather than lost.
int test_a_punch_stopped_before_a_places_nothing() {
    const SessionConfig cfg = punch_config("test_closed_take_punch_before");
    std::filesystem::remove_all(cfg.dir);
    {
        Session s(cfg);
        CHECK(s.create_dirs());
        model::Document doc(engine_document(s));
        CHECK(place_old_take(doc, cfg));
        const model::Region old = active_playlist(doc.current(), 0).regions.at(0);
        const std::size_t undo_depth = doc.history().undo_depth();
        Punched p;
        CHECK(punch(s, punch_range(), 2500, p));
        CHECK(p.length > 0);
        const TakeAdded r = add_closed_take(doc, s, p.take);
        CHECK(r.track_notices.empty() && r.refused.empty());
        const model::Playlist& pl = active_playlist(doc.current(), 0);
        CHECK(pl.regions.size() == 1 && pl.regions[0] == old);
        CHECK(doc.current().sources->sources.size() == 2);
        CHECK(doc.history().undo_depth() == undo_depth);
    }
    std::filesystem::remove_all(cfg.dir);
    return 0;
}

// The playhead start_punch() checks against B is read before the files
// open, and the audio thread rolls on meanwhile: pressed a few frames
// before B, capture can start at or past it. Nothing is then inside
// [A, B); the take goes into the pool, and the operator is told so rather
// than finding nothing on the timeline. Driven here with a take whose
// capture starts past B, marked as that punch.
int test_a_punch_captured_after_its_out_point_is_said() {
    const SessionConfig cfg = punch_config("test_closed_take_punch_after");
    std::filesystem::remove_all(cfg.dir);
    {
        Session s(cfg);
        CHECK(s.create_dirs());
        model::Document doc(engine_document(s));
        CHECK(place_old_take(doc, cfg));
        const model::Region old = active_playlist(doc.current(), 0).regions.at(0);
        const std::size_t undo_depth = doc.history().undo_depth();
        for (auto& t : s.tracks()) t->set_rec_armed(true);
        s.disk().start();
        s.play();
        roll_to(s, static_cast<uint64_t>(kB));
        CHECK(s.transport().playhead() >= static_cast<uint64_t>(kB));
        CHECK(s.start_recording(1));
        roll_to(s, static_cast<uint64_t>(kB + 500));
        CHECK(s.stop());
        std::vector<Session::ClosedTake> closed = s.closed_takes();
        CHECK(closed.size() == 1 && closed[0].files.size() == 1);
        Session::ClosedTake take = closed[0];
        take.punched = true;
        take.punch = punch_range();

        const TakeAdded r = add_closed_take(doc, s, take);
        CHECK(r.refused.empty());
        CHECK(r.track_notices.size() == 1);
        CHECK(mentions(r.track_notices[0], "take 1 is in the session's audio, not on the timeline"));
        CHECK(mentions(r.track_notices[0], "after the punch-out at frame 5000"));
        const model::Playlist& pl = active_playlist(doc.current(), 0);
        CHECK(pl.regions.size() == 1 && pl.regions[0] == old);
        CHECK(doc.current().sources->sources.size() == 2);
        CHECK(doc.history().undo_depth() == undo_depth);
    }
    std::filesystem::remove_all(cfg.dir);
    return 0;
}

// A punch that ends with nothing captured says so, and why: stopped before
// its capture began, or (the #91 race past B + H) begun after its capture
// ended, where the take's files close at once, empty, and the transport
// drops out of record. Positions are "frame N" for the interface to format.
int test_a_punch_that_captured_nothing_is_said() {
    const SessionConfig cfg = punch_config("test_closed_take_punch_nothing");
    std::filesystem::remove_all(cfg.dir);
    {
        Session s(cfg);
        CHECK(s.create_dirs());
        model::Document doc(engine_document(s));
        const std::size_t undo_depth = doc.history().undo_depth();

        // Stopped at 1500, before A - H = 2000.
        Punched early;
        CHECK(punch(s, punch_range(), 1500, early));
        CHECK(early.length == 0);
        TakeAdded r = add_closed_take(doc, s, early.take);
        CHECK(r.refused.empty());
        CHECK(r.track_notices.size() == 1);
        CHECK(mentions(r.track_notices[0], "take 1 captured nothing"));
        CHECK(mentions(r.track_notices[0], "before frame 2000"));
        CHECK(mentions(r.track_notices[0], "punch-in at frame 3000"));

        // Take 2: pressed before B, its first block past B + H = 6000.
        s.disk().start();
        CHECK(s.start_punch(2, punch_range()));
        s.transport().set_playhead(6100);
        roll_to(s, 6200);
        CHECK(s.follow_punch_out());
        CHECK(s.transport().state() == TransportState::Rolling);
        CHECK(s.stop());
        std::vector<Session::ClosedTake> closed = s.closed_takes();
        CHECK(closed.size() == 1 && closed[0].take == 2);
        r = add_closed_take(doc, s, closed[0]);
        CHECK(r.refused.empty());
        CHECK(r.track_notices.size() == 1);
        CHECK(mentions(r.track_notices[0], "take 2 captured nothing"));
        CHECK(mentions(r.track_notices[0], "began at frame 6100"));
        CHECK(mentions(r.track_notices[0], "ended at frame 6000"));
        CHECK(mentions(r.track_notices[0], "punch-out at frame 5000"));

        // Armed before A - H and rolled well into the range, every frame
        // lost to an overrun: the ring is to blame, not an early stop.
        Session::ClosedTake lost;
        lost.take = 3;
        lost.punched = true;
        lost.punch = punch_range();
        lost.origin = static_cast<uint64_t>(kA - kH);
        lost.end = static_cast<uint64_t>(kA + 500);
        DiskThread::ClosedFile empty;
        empty.take = 3;
        empty.path = file_of(cfg, 1);
        lost.files.push_back(empty);
        r = add_closed_take(doc, s, lost);
        CHECK(r.track_notices.size() == 1);
        CHECK(mentions(r.track_notices[0], "take 3 captured nothing"));
        CHECK(mentions(r.track_notices[0], "from frame 2000 on"));
        CHECK(mentions(r.track_notices[0], "overran"));
        CHECK(r.track_notices[0].find("ended before") == std::string::npos);

        CHECK(doc.current().sources->sources.empty());
        CHECK(doc.history().undo_depth() == undo_depth);
    }
    std::filesystem::remove_all(cfg.dir);
    return 0;
}

// The punch the record commands start, from the document's transport
// settings: [A - H, B + H) captured, H = max(punch_crossfade, 1 s), cut at
// the timeline's start; Auto monitoring switching at A and B with the
// crossfade the take will be placed with. No range set: no punch.
int test_the_document_punch() {
    model::SessionSettings settings;
    settings.sample_rate = kPunchRate;
    model::Snapshot doc = model::new_session(settings);
    PunchRange r;
    CHECK(!document_punch(doc, r));

    doc.settings.transport.punch = {{kA, kB}};
    doc.settings.transport.punch_crossfade = kX;
    CHECK(document_punch(doc, r));
    CHECK(r.capture_from == static_cast<uint64_t>(kA - kH));
    CHECK(r.capture_to == static_cast<uint64_t>(kB + kH));
    CHECK(r.monitor_in == static_cast<uint64_t>(kA));
    CHECK(r.monitor_out == static_cast<uint64_t>(kB));
    CHECK(r.crossfade == static_cast<uint32_t>(kX));
    CHECK(r.shape == FadeCurve::EqualPower);

    doc.settings.transport.punch = {{500, kB}};
    doc.settings.transport.punch_crossfade = 2400;
    CHECK(document_punch(doc, r));
    CHECK(r.capture_from == 0);
    CHECK(r.capture_to == static_cast<uint64_t>(kB + 2400));

    // [B, A) and [A, A) are no range, as the interface shows them.
    doc.settings.transport.punch = {{kB, kA}};
    CHECK(!document_punch(doc, r));
    doc.settings.transport.punch = {{kA, kA}};
    CHECK(!document_punch(doc, r));
    return 0;
}

}  // namespace

int main() {
    int failed = 0;
    failed += test_a_take_larger_than_one_compound_lands_whole();
    failed += test_a_take_is_one_compound();
    failed += test_one_unreadable_track_costs_that_track();
    failed += test_a_refused_take_keeps_its_audio_in_the_pool();
    failed += test_a_split_take_lands_in_track_order();
    failed += test_a_refused_part_keeps_its_audio_and_the_others_land();
    failed += test_a_punch_goes_in_over_its_range();
    failed += test_a_punch_stopped_inside_its_range_ends_there();
    failed += test_a_punch_armed_late_starts_where_it_recorded();
    failed += test_a_punch_over_nothing_has_no_crossfade();
    failed += test_a_punch_the_model_refuses_crossfades_for_lands_without_them();
    failed += test_a_punch_names_the_fades_it_shortened();
    failed += test_an_unpunched_take_names_no_fade();
    failed += test_a_punch_without_its_crossfades_names_no_fade();
    failed += test_a_punch_stopped_before_a_places_nothing();
    failed += test_a_punch_captured_after_its_out_point_is_said();
    failed += test_a_punch_that_captured_nothing_is_said();
    failed += test_the_document_punch();
    if (failed != 0) {
        std::fprintf(stderr, "closed_take: %d failed\n", failed);
        return 1;
    }
    std::printf("closed_take: 18 passed\n");
    return 0;
}
