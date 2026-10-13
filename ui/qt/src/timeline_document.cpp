// SPDX-License-Identifier: GPL-3.0-or-later
#include "timeline_document.h"

#include <algorithm>

namespace sonora::ui {

using namespace model;

namespace {

Id max_id(const Snapshot& s) {
    Id m = 0;
    for (const auto& t : s.edit->tracks) {
        m = std::max(m, t->id);
        for (const auto& p : t->playlists) {
            m = std::max(m, p->id);
            for (const auto& r : p->regions) m = std::max(m, r.id);
        }
    }
    for (const auto& src : s.sources->sources) m = std::max(m, src.id);
    return m;
}

}  // namespace

LocalDocument::LocalDocument(Snapshot initial, std::size_t max_steps)
    : current_(std::move(initial)), max_steps_(std::max<std::size_t>(2, max_steps)) {
    history_.push_back(current_.edit);
    last_id_ = max_id(current_);
}

bool LocalDocument::submit(const Operation& op, std::string& why) {
    Snapshot next;
    if (!apply(current_, op, next, why)) return false;
    current_ = std::move(next);
    if (!undoable(op)) return true;
    // A new edit after an undo discards the redo branch, as every DAW does.
    history_.resize(at_ + 1);
    history_.push_back(current_.edit);
    if (history_.size() > max_steps_) history_.erase(history_.begin());
    at_ = history_.size() - 1;
    return true;
}

bool LocalDocument::undo(std::string& why) {
    if (!can_undo()) {
        why = "nothing to undo";
        return false;
    }
    current_.edit = history_[--at_];
    ++current_.revision;
    return true;
}

bool LocalDocument::redo(std::string& why) {
    if (!can_redo()) {
        why = "nothing to redo";
        return false;
    }
    current_.edit = history_[++at_];
    ++current_.revision;
    return true;
}

Snapshot make_empty_session(uint32_t tracks, uint32_t sample_rate) {
    Snapshot s;
    s.settings.sample_rate = sample_rate;
    s.settings.timecode_origin = 3600LL * sample_rate;
    auto edit = std::make_shared<EditState>();
    Id next = 0;
    for (uint32_t i = 0; i < tracks; ++i) {
        auto t = std::make_shared<TrackEdit>();
        t->id = ++next;
        t->name = "Audio " + std::to_string(i + 1);
        auto p = std::make_shared<Playlist>();
        p->id = ++next;
        p->name = t->name + ".01";
        t->active_playlist = p->id;
        t->playlists.push_back(std::move(p));
        TrackMix m;
        m.track = t->id;
        s.mix.tracks.push_back(m);
        edit->tracks.push_back(std::move(t));
    }
    s.edit = std::move(edit);
    s.sources = std::make_shared<SourcePool>();
    return s;
}

Snapshot make_demo_session(uint32_t tracks, uint32_t sample_rate) {
    static const char* const kNames[] = {
        "Kick",  "Snare",    "Hi-Hat", "Tom 1",  "Tom 2",  "OH L",     "OH R",   "Room",
        "Bass DI", "Bass Amp", "Gtr L",  "Gtr R",  "Keys L", "Keys R",   "Lead Vox", "BV 1",
        "BV 2",  "Talkback",
    };
    Snapshot s = make_empty_session(tracks, sample_rate);
    auto edit = std::make_shared<EditState>(*s.edit);
    auto pool = std::make_shared<SourcePool>();
    Id next = 0;
    for (const auto& t : edit->tracks) next = std::max({next, t->id, t->active_playlist});

    const Frames bar = static_cast<Frames>(sample_rate) * 2;  // 120 bpm, 4/4
    for (std::size_t i = 0; i < edit->tracks.size(); ++i) {
        auto t = std::make_shared<TrackEdit>(*edit->tracks[i]);
        if (i < sizeof kNames / sizeof kNames[0]) t->name = kNames[i];
        auto p = std::make_shared<Playlist>(*t->playlists[0]);
        p->name = t->name + ".01";

        // One take per track, 40 bars long, from which the regions are cut
        // the way a tracking session's first edit pass leaves them.
        Source src;
        src.id = ++next;
        src.name = t->name + "_01";
        src.length = bar * 40;
        pool->sources.push_back(src);

        const auto region = [&](Frames from_bar, Frames bars, Frames offset_bars) {
            Region r;
            r.id = ++next;
            r.name = src.name + (p->regions.empty() ? "" : "-0" + std::to_string(p->regions.size()));
            r.position = from_bar * bar;
            r.length = bars * bar;
            r.source_offset = offset_bars * bar;
            r.channels.push_back({src.id, 0});
            p->regions.push_back(r);
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
        t->playlists[0] = std::move(p);
        edit->tracks[i] = std::move(t);
    }
    s.edit = std::move(edit);
    s.sources = std::move(pool);

    // A rough balance, so the mixer looks like a mix rather than a reset.
    for (std::size_t i = 0; i < s.mix.tracks.size(); ++i) {
        TrackMix& m = s.mix.tracks[i];
        m.gain_db = -2.0f - static_cast<float>((i * 7) % 9);
        static const float kPan[] = {0.0f, 0.0f, 0.3f, -0.4f, 0.4f, -0.8f, 0.8f, 0.0f,
                                     0.0f, 0.0f, -0.7f, 0.7f, -0.5f, 0.5f, 0.0f, -0.3f,
                                     0.3f, 0.0f};
        m.pan = kPan[i % 18];
    }
    if (s.mix.tracks.size() > 17) s.mix.tracks[17].mute = true;  // talkback
    return s;
}

}  // namespace sonora::ui
