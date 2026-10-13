// SPDX-License-Identifier: GPL-3.0-or-later
#include "timeline_document.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace sonora::ui {

using namespace model;

// --- queries ----------------------------------------------------------------

const Playlist* active_playlist(const TrackEdit* t) {
    if (t == nullptr) return nullptr;
    for (const auto& p : t->playlists) {
        if (p->id == t->active_playlist) return p.get();
    }
    return nullptr;
}

const TrackEdit* find_track(const Snapshot& s, Id track) {
    const int i = track_index(s, track);
    return i < 0 ? nullptr : s.edit->tracks[static_cast<std::size_t>(i)].get();
}

int track_index(const Snapshot& s, Id track) {
    const auto& ts = s.edit->tracks;
    for (std::size_t i = 0; i < ts.size(); ++i) {
        if (ts[i]->id == track) return static_cast<int>(i);
    }
    return -1;
}

const Region* find_region(const Snapshot& s, Id region, const TrackEdit** track) {
    if (region == kNoId) return nullptr;
    for (const auto& t : s.edit->tracks) {
        const Playlist* p = active_playlist(t.get());
        if (p == nullptr) continue;
        for (const Region& r : p->regions) {
            if (r.id != region) continue;
            if (track) *track = t.get();
            return &r;
        }
    }
    return nullptr;
}

// --- the document -------------------------------------------------------------

ModelDocument::ModelDocument(Snapshot initial, std::size_t budget_bytes)
    : budget_(budget_bytes), history_(std::move(initial), budget_bytes) {}

bool ModelDocument::submit(const Operation& op, std::string& why) {
    return history_.apply(op, why);
}

bool ModelDocument::undo(std::string& why) {
    if (history_.undo()) return true;
    why = "nothing to undo";
    return false;
}

bool ModelDocument::redo(std::string& why) {
    if (history_.redo()) return true;
    why = "nothing to redo";
    return false;
}

Id ModelDocument::reserve_id() {
    // An id reserved and then not used (a move that split nothing) is never
    // handed out again; apply() raising next_id past it is harmless.
    next_unreserved_ = std::max(next_unreserved_, history_.current().next_id);
    return next_unreserved_++;
}

bool ModelDocument::reset(Snapshot s, std::string&) {
    history_ = History(std::move(s), budget_);
    next_unreserved_ = 0;
    ++session_generation_;
    return true;
}

// --- sessions built from operations -------------------------------------------

namespace {

// Applies `op` to a session under construction. These builders feed fixed,
// valid operations; one refused is a defect in this file, and a session
// quietly missing its tracks would be worse than stopping.
void build(Snapshot& s, const Operation& op) {
    std::string why;
    Snapshot next;
    if (!apply(s, op, next, why)) {
        std::fprintf(stderr, "sonora-ui: building a session: %s\n", why.c_str());
        std::abort();
    }
    s = std::move(next);
}

}  // namespace

Snapshot make_empty_session(uint32_t tracks, uint32_t sample_rate) {
    SessionSettings settings;
    settings.sample_rate = sample_rate;
    settings.timecode.origin = 3600LL * sample_rate;  // 01:00:00:00
    Snapshot s = new_session(settings);
    for (uint32_t i = 0; i < tracks; ++i) {
        AddTrack t;
        t.track = s.next_id;
        t.playlist = s.next_id + 1;
        t.name = "Audio " + std::to_string(i + 1);
        t.playlist_name = t.name + ".01";
        t.mix.input = {static_cast<uint16_t>(i)};
        build(s, t);
    }
    // A starting point, as a loaded file is: nothing to undo, revision 0.
    s.revision = 0;
    return s;
}

Snapshot make_demo_session(uint32_t tracks, uint32_t sample_rate) {
    static const char* const kNames[] = {
        "Kick",  "Snare",    "Hi-Hat", "Tom 1",  "Tom 2",  "OH L",     "OH R",   "Room",
        "Bass DI", "Bass Amp", "Gtr L",  "Gtr R",  "Keys L", "Keys R",   "Lead Vox", "BV 1",
        "BV 2",  "Talkback",
    };
    static const float kPan[] = {0.0f, 0.0f, 0.3f, -0.4f, 0.4f, -0.8f, 0.8f, 0.0f, 0.0f,
                                 0.0f, -0.7f, 0.7f, -0.5f, 0.5f, 0.0f, -0.3f, 0.3f, 0.0f};
    constexpr std::size_t kNamed = sizeof kNames / sizeof kNames[0];

    SessionSettings settings;
    settings.name = "Demo session";
    settings.sample_rate = sample_rate;
    settings.timecode.origin = 3600LL * sample_rate;
    Snapshot s = new_session(settings);
    const Frames bar = static_cast<Frames>(sample_rate) * 2;  // 120 bpm, 4/4

    for (uint32_t i = 0; i < tracks; ++i) {
        const std::string name = i < kNamed ? kNames[i] : "Audio " + std::to_string(i + 1);
        AddTrack t;
        t.track = s.next_id;
        t.playlist = s.next_id + 1;
        t.name = name;
        t.playlist_name = name + ".01";
        t.mix.input = {static_cast<uint16_t>(i)};
        // A rough balance, so the mixer looks like a mix rather than a reset.
        t.mix.gain_db = -2.0f - static_cast<float>((i * 7) % 9);
        t.mix.pan = kPan[i % 18];
        t.mix.mute = i == 17;  // talkback
        build(s, t);

        // One take per track, 40 bars long, from which the regions are cut
        // the way a tracking session's first edit pass leaves them.
        AddSource src;
        src.source.id = s.next_id;
        src.source.path = "Audio Files/" + name + "_01.wav";
        src.source.length = bar * 40;
        src.source.take = 1;
        src.source.recorded_on_track = t.track;
        build(s, src);

        int cut = 0;
        const auto region = [&](Frames from_bar, Frames bars, Frames offset_bars) {
            PlaceRegion p;
            p.playlist = t.playlist;
            p.region.id = s.next_id;
            p.region.name = name + "_01" + (cut == 0 ? "" : "-0" + std::to_string(cut));
            p.region.position = from_bar * bar;
            p.region.length = bars * bar;
            p.region.source_offset = offset_bars * bar;
            p.region.channels = {{src.source.id, 0}};
            build(s, p);
            ++cut;
        };
        const Frames k = static_cast<Frames>(i);
        if (i == 14) {          // lead vocal: verse and chorus phrases
            region(4, 4, 4);
            region(9, 3, 9);
            region(13, 6, 13);
        } else if (i == 17) {   // talkback: one short note
            region(1, 1, 1);
        } else if (i == 15 || i == 16) {  // backing vocals: choruses only
            region(12, 4, 12);
            region(20, 4, 20);
        } else {
            region(0, 8 + (k % 3), 0);
            region(9 + (k % 3), 7, 9 + (k % 3));
            region(19 + (k % 2), 6, 19 + (k % 2));
        }
    }
    s.revision = 0;
    return s;
}

}  // namespace sonora::ui
