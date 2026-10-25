// SPDX-License-Identifier: GPL-3.0-or-later
//
// Takes on disk the open session does not name are offered for placement,
// never placed silently (session-format.md, "Crash recovery"). What is
// offered: this engine's takes of a session of the document's name, at its
// rate, with audio in them, and no source naming them. What placing does:
// each take on the track its Description names, at its TimeReference, one
// undo step; a punch take over its punch range only; a loop take as its
// passes on new playlists, said when they were placed by loop length; a
// take whose track is gone, or that cannot cover its punch range, into the
// pool only.
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "sonora/closed_take.h"
#include "sonora/model/replication.h"
#include "sonora/orphan_offer.h"
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
    std::fprintf(stderr, "  expected \"%s\" in one of %zu notices\n", needle.c_str(), lines.size());
    for (const std::string& l : lines) std::fprintf(stderr, "    %s\n", l.c_str());
    return false;
}

struct Rig {
    fs::path dir;
    std::unique_ptr<Session> session;
    std::unique_ptr<model::Document> doc;

    Rig(const char* name, int tracks = 2) {
        dir = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(dir, ec);
        SessionConfig cfg;
        cfg.dir = dir.string();
        cfg.track_count = tracks;
        cfg.ring_seconds = 0.01;
        session = std::make_unique<Session>(cfg);
        session->create_dirs();
        doc = std::make_unique<model::Document>(engine_document(*session));
    }
    ~Rig() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    model::Id track_id(std::size_t i) const { return doc->current().edit->tracks[i]->id; }

    // A closed take file with `description` in its bext, as the engine
    // writes one; an empty description writes none of ours.
    bool write(const std::string& name, const std::string& description, uint64_t time_reference,
               uint64_t frames, uint32_t rate = 48000) const {
        WavWriter w;
        if (!description.empty()) w.set_description(description);
        if (!w.open((dir / "Audio Files" / name).string(), rate, 1, WavWriter::Format::Int24,
                    time_reference)) {
            return false;
        }
        const std::vector<float> audio(frames, 0.25f);
        return (frames == 0 || w.write(audio.data(), audio.size())) && w.close();
    }

    const model::Playlist* active(std::size_t track) const {
        const model::TrackEdit& t = *doc->current().edit->tracks[track];
        for (const auto& p : t.playlists) {
            if (p->id == t.active_playlist) return p.get();
        }
        return nullptr;
    }

    const model::Region* region(std::size_t track, const std::string& name) const {
        const model::Playlist* p = active(track);
        if (p == nullptr) return nullptr;
        for (const model::Region& r : p->regions) {
            if (r.name == name) return &r;
        }
        return nullptr;
    }

    const model::Source* source(const std::string& path) const {
        for (const model::Source& s : doc->current().sources->sources) {
            if (s.path == path) return &s;
        }
        return nullptr;
    }
};

std::string whole(int take, uint64_t track, const std::string& session = "Untitled") {
    return take_description(take, track, session, nullptr);
}

// Only this engine's takes of this session, with audio, that no source
// names; in name order, each line saying where it would go.
int test_what_is_offered() {
    Rig rig("sonora_test_orphan_offered");
    const uint64_t t1 = rig.track_id(0);
    CHECK(rig.write("Audio 1_02.wav", whole(2, t1), 48000, 4800));
    CHECK(rig.write("Audio 1_01.wav", whole(1, t1), 0, 4800));
    CHECK(rig.write("Audio 1_03.wav", whole(3, t1, "Another Song"), 0, 4800));
    CHECK(rig.write("Audio 1_04.wav", whole(4, t1), 0, 0));
    CHECK(rig.write("Audio 1_05.wav", whole(5, t1), 0, 4800, 44100));
    CHECK(rig.write("Imported.wav", "", 0, 4800));
    CHECK(rig.write("Elsewhere.wav", "Recorded by another program", 0, 4800));

    OrphanOffer offer = find_orphan_takes(rig.doc->current(), rig.dir.string());
    CHECK(offer.takes.size() == 2);
    CHECK(offer.takes[0].path == "Audio Files/Audio 1_01.wav");
    CHECK(offer.takes[1].path == "Audio Files/Audio 1_02.wav");
    CHECK(offer.takes[1].time_reference == 48000 && offer.takes[1].frames == 4800);
    CHECK(mentions(offer.text, "Audio 1_02.wav: take 2 on \"Audio 1\" at 1.000 s, 0.100 s long"));
    CHECK(offer.text.find("Audio 1_03") == std::string::npos);

    // Placed, a take is named by a source and is no longer an orphan.
    const OrphansPlaced placed = place_orphan_takes(*rig.doc, *rig.session, offer);
    CHECK(placed.placed == 2 && placed.pooled == 0);
    CHECK(find_orphan_takes(rig.doc->current(), rig.dir.string()).takes.empty());

    // No folder yet: nothing to offer.
    Rig empty("sonora_test_orphan_none");
    fs::remove_all(empty.dir / "Audio Files");
    CHECK(find_orphan_takes(empty.doc->current(), empty.dir.string()).takes.empty());
    return 0;
}

// A whole take goes on its track at its TimeReference as one undo step,
// named for the placement; undo takes it off again.
int test_whole_take_is_one_undo_step() {
    Rig rig("sonora_test_orphan_whole");
    CHECK(rig.write("Audio 2_07.wav", whole(7, rig.track_id(1)), 96000, 4800));
    const OrphanOffer offer = find_orphan_takes(rig.doc->current(), rig.dir.string());
    CHECK(offer.takes.size() == 1);
    // Nothing is placed by finding it.
    CHECK(rig.region(1, "Audio 2_07") == nullptr);
    CHECK(rig.source("Audio Files/Audio 2_07.wav") == nullptr);

    const uint64_t revision = rig.doc->current().revision;
    const OrphansPlaced placed = place_orphan_takes(*rig.doc, *rig.session, offer);
    CHECK(placed.updates.size() == 1);
    CHECK(rig.doc->current().revision == revision + 1);
    const model::Region* r = rig.region(1, "Audio 2_07");
    CHECK(r != nullptr && r->position == 96000 && r->length == 4800);
    const model::Source* s = rig.source("Audio Files/Audio 2_07.wav");
    CHECK(s != nullptr && s->take == 7 && s->recorded_on_track == rig.track_id(1));
    CHECK(s->kind == model::SourceKind::Recorded);

    model::ReplicationUpdate u;
    std::string why;
    CHECK(rig.doc->undo(u, why));
    CHECK(u.label == kRecoveredLabel);
    CHECK(rig.region(1, "Audio 2_07") == nullptr);
    return 0;
}

// A punch take goes over [A, B) only, the region beneath kept either side;
// one that cannot cover its range, and one whose track is gone, go into
// the pool, never over existing material.
int test_punch_and_unplaceable_takes() {
    Rig rig("sonora_test_orphan_punch");
    CHECK(rig.write("Old.wav", "", 0, 20000));
    std::vector<model::ReplicationUpdate> updates;
    std::string error;
    CHECK(place_file(*rig.doc, rig.dir.string(), "Audio Files/Old.wav", 0, updates, error));

    PunchRange p;
    p.monitor_in = 10000;
    p.monitor_out = 12000;
    const uint64_t t1 = rig.track_id(0);
    CHECK(rig.write("Audio 1_05.wav", take_description(5, t1, "Untitled", &p), 8000, 6000));
    CHECK(rig.write("Audio 1_06.wav", take_description(6, t1, "Untitled", &p), 11000, 3600));
    CHECK(rig.write("Audio 9_06.wav", whole(6, 999), 0, 4800));
    const OrphanOffer offer = find_orphan_takes(rig.doc->current(), rig.dir.string());
    CHECK(offer.takes.size() == 3);
    CHECK(mentions(offer.text, "Audio 1_05.wav: take 5, punched on \"Audio 1\" from 0.208 s to 0.250 s"));
    CHECK(mentions(offer.text, "Audio 1_06.wav: take 6, 0.075 s, into the audio pool only: it does "
                               "not cover its punch range"));
    CHECK(mentions(offer.text, "Audio 9_06.wav: take 6, 0.100 s, into the audio pool only: its "
                               "track is not in this session"));

    const OrphansPlaced placed = place_orphan_takes(*rig.doc, *rig.session, offer);
    CHECK(placed.placed == 1 && placed.pooled == 2);
    const model::Region* punch = rig.region(0, "Audio 1_05");
    CHECK(punch != nullptr && punch->position == 10000 && punch->length == 2000);
    CHECK(punch->source_offset == 2000);
    // The region beneath, either side of the punch: split, not replaced.
    CHECK(rig.active(0)->regions.size() == 3);
    CHECK(rig.region(0, "Audio 1_06") == nullptr);
    CHECK(rig.source("Audio Files/Audio 1_06.wav") != nullptr);
    CHECK(rig.source("Audio Files/Audio 9_06.wav") != nullptr);
    CHECK(rig.active(1)->regions.empty());
    return 0;
}

// A loop take with no wrap list: its passes on new playlists, derived from
// its Description's loop range and its TimeReference, and said so.
int test_loop_take_passes() {
    Rig rig("sonora_test_orphan_loop");
    LoopRange l;
    l.from = 2000;
    l.to = 4000;
    const uint64_t t1 = rig.track_id(0);
    // From 500 frames of pre-roll, through two whole passes and half a third.
    CHECK(rig.write("Audio 1_08.wav", take_description(8, t1, "Untitled", nullptr, &l), 1500,
                    500 + 2 * 2000 + 1000));
    const OrphanOffer offer = find_orphan_takes(rig.doc->current(), rig.dir.string());
    CHECK(offer.takes.size() == 1);
    CHECK(mentions(offer.text, "loop on \"Audio 1\": 3 passes from 0.042 s to 0.083 s on new "
                               "playlists, placed by loop length"));
    const std::size_t playlists = rig.doc->current().edit->tracks[0]->playlists.size();
    const OrphansPlaced placed = place_orphan_takes(*rig.doc, *rig.session, offer);
    CHECK(placed.placed == 1);
    CHECK(rig.doc->current().edit->tracks[0]->playlists.size() == playlists + 3);
    CHECK(any_mentions(placed.notices, "placed by loop length"));
    // The last complete pass on the active playlist, at A.
    const model::Playlist* a = rig.active(0);
    CHECK(a != nullptr && a->regions.size() == 1);
    CHECK(a->regions[0].position == 2000 && a->regions[0].length == 2000);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    struct Test {
        const char* name;
        int (*run)();
    };
    const Test tests[] = {
        {"what_is_offered", test_what_is_offered},
        {"whole_take_is_one_undo_step", test_whole_take_is_one_undo_step},
        {"punch_and_unplaceable_takes", test_punch_and_unplaceable_takes},
        {"loop_take_passes", test_loop_take_passes},
    };
    for (const Test& t : tests) {
        if (argc > 1 && std::string(argv[1]) != t.name) continue;
        if (t.run()) return 1;
    }
    std::printf("orphan_offer: ok\n");
    return 0;
}
