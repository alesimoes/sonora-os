// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/model/render_in_place.h"

#include <algorithm>
#include <utility>

namespace sonora::model {

namespace {

const TrackEdit* find_track(const Snapshot& s, Id id) {
    for (const auto& t : s.edit->tracks) {
        if (t->id == id) return t.get();
    }
    return nullptr;
}

const Playlist* find_playlist(const TrackEdit& t, Id id) {
    for (const auto& p : t.playlists) {
        if (p->id == id) return p.get();
    }
    return nullptr;
}

std::string unique_playlist_name(const Snapshot& s, const std::string& base) {
    if (!playlist_name_taken(s, base)) return base;
    for (int n = 2; n < 10000; ++n) {
        const std::string c = base + " " + std::to_string(n);
        if (!playlist_name_taken(s, c)) return c;
    }
    return base;
}

}  // namespace

const char* render_kind_name(RenderKind k) {
    switch (k) {
        case RenderKind::Freeze: return "freeze";
        case RenderKind::Commit: return "commit";
        case RenderKind::Process: break;
    }
    return "process";
}

const Playlist* frozen_playlist(const TrackEdit& t) {
    const Playlist* p = find_playlist(t, t.active_playlist);
    return p != nullptr && p->freeze ? p : nullptr;
}

std::vector<Id> active_inserts(const TrackEdit& t) {
    std::vector<const Insert*> list;
    for (const Insert& i : t.inserts) {
        if (i.active) list.push_back(&i);
    }
    std::sort(list.begin(), list.end(),
              [](const Insert* a, const Insert* b) { return a->slot < b->slot; });
    std::vector<Id> ids;
    for (const Insert* i : list) ids.push_back(i->id);
    return ids;
}

bool render_range_of(const Snapshot& s, Id track, RenderKind kind, Frames sel_start,
                     Frames sel_end, RenderRange& out, std::string& why) {
    out = RenderRange{};
    why.clear();
    const std::string what = kind == RenderKind::Process ? "process" : render_kind_name(kind);
    const TrackEdit* t = find_track(s, track);
    if (t == nullptr) {
        why = "no such track; nothing was rendered";
        return false;
    }
    if (t->kind != TrackKind::Audio) {
        why = t->name + " is a MIDI track, and only audio is rendered; nothing to " + what;
        return false;
    }
    if (frozen_playlist(*t) != nullptr) {
        why = t->name + " is frozen already; unfreeze it first";
        return false;
    }
    if (active_inserts(*t).empty()) {
        why = t->name + " has no active plug-in insert, so there is nothing to " + what;
        return false;
    }
    const Playlist* p = find_playlist(*t, t->active_playlist);
    if (p == nullptr || p->regions.empty()) {
        why = t->name + " holds no audio to " + what;
        return false;
    }
    if (kind == RenderKind::Process) {
        if (sel_end <= sel_start) {
            why = "select a range on " + t->name + " to process";
            return false;
        }
        out.start = std::max<Frames>(0, sel_start);
        out.end = sel_end;
        return true;
    }
    out.start = p->regions.front().position;
    out.end = p->regions.back().end();
    for (const Region& r : p->regions) out.end = std::max(out.end, r.end());
    return true;
}

bool render_members(const Snapshot& s, const RenderPlacement& pl,
                    const std::function<Id()>& next_id, std::vector<Member>& out,
                    std::string& why) {
    out.clear();
    why.clear();
    const TrackEdit* t = find_track(s, pl.track);
    if (t == nullptr) {
        why = "no such track; nothing was rendered";
        return false;
    }
    if (frozen_playlist(*t) != nullptr) {
        why = t->name + " is frozen already; unfreeze it first";
        return false;
    }
    const Playlist* from = find_playlist(*t, t->active_playlist);
    if (from == nullptr) {
        why = t->name + " has no active playlist; nothing was rendered";
        return false;
    }
    const auto src = std::find_if(s.sources->sources.begin(), s.sources->sources.end(),
                                  [&](const Source& e) { return e.id == pl.source; });
    if (src == s.sources->sources.end()) {
        why = "the rendered file is not in the session; nothing was rendered";
        return false;
    }
    if (src->channels != t->channels) {
        why = "the rendered file has " + std::to_string(src->channels) + " channels, " + t->name +
              " has " + std::to_string(t->channels) + "; nothing was rendered";
        return false;
    }
    if (src->length <= 0) {
        why = "the rendered file is empty; nothing was rendered";
        return false;
    }
    const std::vector<Id> inserts = active_inserts(*t);
    if (inserts.empty()) {
        why = t->name + " has no active plug-in insert; nothing was rendered";
        return false;
    }

    Region r;
    r.name = pl.name;
    r.position = pl.start;
    r.length = src->length;
    r.source_offset = 0;
    for (std::uint16_t ch = 0; ch < t->channels; ++ch) r.channels.push_back({pl.source, ch});

    Id playlist = 0;
    if (pl.kind == RenderKind::Process) {
        DuplicatePlaylist dup;
        dup.track = pl.track;
        dup.source = from->id;
        dup.name = next_playlist_name(s, pl.track);
        const std::size_t ids = duplicate_playlist_ids(s, dup);
        if (ids == 0) {
            why = "the playlist cannot be copied; nothing was rendered";
            return false;
        }
        // The copy and its regions take consecutive ids from the first.
        playlist = next_id();
        for (std::size_t i = 1; i < ids; ++i) next_id();
        dup.playlist = playlist;
        out.push_back(std::move(dup));
    } else {
        AddPlaylist add;
        add.track = pl.track;
        playlist = next_id();
        add.playlist = playlist;
        add.name = unique_playlist_name(
            s, pl.kind == RenderKind::Freeze ? t->name + " (frozen)" : t->name + " (committed)");
        if (pl.kind == RenderKind::Freeze) add.freeze = PlaylistFreeze{from->id, inserts};
        out.push_back(std::move(add));
    }
    r.id = next_id();
    out.push_back(PlaceRegion{playlist, std::move(r), next_id()});
    out.push_back(SetActivePlaylist{pl.track, playlist});
    for (const Id i : inserts) out.push_back(SetInsertActive{i, false});
    return true;
}

bool unfreeze_members(const Snapshot& s, Id track, std::vector<Member>& out, std::string& why) {
    out.clear();
    why.clear();
    const TrackEdit* t = find_track(s, track);
    if (t == nullptr) {
        why = "no such track; nothing was unfrozen";
        return false;
    }
    const Playlist* frozen = frozen_playlist(*t);
    if (frozen == nullptr) {
        why = t->name + " is not frozen";
        return false;
    }
    if (find_playlist(*t, frozen->freeze->from) == nullptr) {
        why = "the playlist " + t->name + " was frozen from is gone; its frozen audio stays as it is";
        return false;
    }
    out.push_back(SetActivePlaylist{track, frozen->freeze->from});
    for (const Id i : frozen->freeze->inserts) {
        const bool still = std::any_of(t->inserts.begin(), t->inserts.end(),
                                       [i](const Insert& in) { return in.id == i; });
        if (still) out.push_back(SetInsertActive{i, true});
    }
    out.push_back(RemovePlaylist{track, frozen->id});
    return true;
}

}  // namespace sonora::model
