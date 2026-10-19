// SPDX-License-Identifier: GPL-3.0-or-later
//
// A closed take reaches the document as one compound: one update, one undo
// step, every armed track (ADR-0009 update 2026-10-14, item 8). A take is
// never all-or-nothing: a track whose file does not read costs that track
// and says so by name; a take larger than one compound is split into
// compounds within the limit; and if the model refuses the take, its audio
// still goes into the pool, so a save keeps it, and the engine says loudly
// that it is not on the timeline.
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
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

}  // namespace

int main() {
    int failed = 0;
    failed += test_a_take_larger_than_one_compound_lands_whole();
    failed += test_a_take_is_one_compound();
    failed += test_one_unreadable_track_costs_that_track();
    failed += test_a_refused_take_keeps_its_audio_in_the_pool();
    failed += test_a_split_take_lands_in_track_order();
    failed += test_a_refused_part_keeps_its_audio_and_the_others_land();
    if (failed != 0) {
        std::fprintf(stderr, "closed_take: %d failed\n", failed);
        return 1;
    }
    std::printf("closed_take: 6 passed\n");
    return 0;
}
