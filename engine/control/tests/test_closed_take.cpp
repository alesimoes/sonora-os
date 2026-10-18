// SPDX-License-Identifier: GPL-3.0-or-later
//
// A closed take reaches the document as one compound: one update, one undo
// step, every armed track (ADR-0009 update 2026-10-14, item 8). A take is
// never all-or-nothing: a track whose file does not read costs that track
// and says so by name; and if the model refuses the take, its audio still
// goes into the pool, so a save keeps it, and the engine says loudly that it
// is not on the timeline.
#include <unistd.h>

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

void wipe(const std::string& dir) {
    for (int t = 1; t <= 3; ++t) {
        const std::string f = dir + "/Audio Files/Audio " + std::to_string(t) + "_01.wav";
        std::remove(f.c_str());
    }
    ::rmdir((dir + "/Audio Files").c_str());
    ::rmdir(dir.c_str());
}

SessionConfig config(const char* dir) {
    SessionConfig cfg;
    cfg.dir = dir;
    cfg.track_count = 3;
    cfg.ring_seconds = 0.01;
    return cfg;
}

// Take 1 on all three tracks: 64 frames each, through the disk thread.
bool record(Session& s) {
    if (!s.create_dirs()) return false;
    for (auto& t : s.tracks()) t->set_rec_armed(true);
    s.disk().start();
    if (!s.start_recording(1)) return false;
    float block[64];
    for (float& f : block) f = 0.125f;
    const float* inputs[3] = {block, block, block};
    backend::AudioBlock b{};
    b.inputs = inputs;
    b.input_count = 3;
    b.nframes = 64;
    s.process(b);
    return s.stop();
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

int test_a_take_is_one_compound() {
    const SessionConfig cfg = config("test_closed_take_whole");
    wipe(cfg.dir);
    {
        Session s(cfg);
        CHECK(record(s));
        model::Document doc(engine_document(s));
        const std::uint64_t rev = doc.current().revision;
        const ClosedTake r = add_closed_take(doc, s, 1);
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
        const ClosedTake r = add_closed_take(doc, s, 1);
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
        const ClosedTake r = add_closed_take(doc, s, 1);
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

}  // namespace

int main() {
    int failed = 0;
    failed += test_a_take_is_one_compound();
    failed += test_one_unreadable_track_costs_that_track();
    failed += test_a_refused_take_keeps_its_audio_in_the_pool();
    if (failed != 0) {
        std::fprintf(stderr, "closed_take: %d failed\n", failed);
        return 1;
    }
    std::printf("closed_take: 3 passed\n");
    return 0;
}
