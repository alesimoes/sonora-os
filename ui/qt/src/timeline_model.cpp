// SPDX-License-Identifier: GPL-3.0-or-later
#include "timeline_model.h"

#include <algorithm>
#include <cmath>

namespace sonora::ui::model {

bool Region::operator==(const Region& o) const {
    if (channels.size() != o.channels.size()) return false;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (channels[i].source != o.channels[i].source ||
            channels[i].channel != o.channels[i].channel) {
            return false;
        }
    }
    return id == o.id && name == o.name && position == o.position && length == o.length &&
           source_offset == o.source_offset && gain_db == o.gain_db && mute == o.mute &&
           fade_in.length == o.fade_in.length && fade_out.length == o.fade_out.length;
}

const Playlist* TrackEdit::active() const {
    for (const auto& p : playlists) {
        if (p->id == active_playlist) return p.get();
    }
    return nullptr;
}

const Source* SourcePool::find(Id id) const {
    for (const auto& s : sources) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

const TrackMix* MixState::find(Id track) const {
    for (const auto& t : tracks) {
        if (t.track == track) return &t;
    }
    return nullptr;
}

const TrackEdit* Snapshot::track(Id id) const {
    for (const auto& t : edit->tracks) {
        if (t->id == id) return t.get();
    }
    return nullptr;
}

int Snapshot::track_index(Id id) const {
    for (std::size_t i = 0; i < edit->tracks.size(); ++i) {
        if (edit->tracks[i]->id == id) return static_cast<int>(i);
    }
    return -1;
}

const Region* Snapshot::find_region(Id region, const TrackEdit** track) const {
    for (const auto& t : edit->tracks) {
        const Playlist* p = t->active();
        if (p == nullptr) continue;
        for (const auto& r : p->regions) {
            if (r.id == region) {
                if (track) *track = t.get();
                return &r;
            }
        }
    }
    return nullptr;
}

bool undoable(const Operation& op) { return !std::holds_alternative<SetMix>(op); }

const char* operation_name(const Operation& op) {
    switch (op.index()) {
        case 0: return "MoveRegion";
        case 1: return "TrimRegion";
        case 2: return "SplitRegion";
        case 3: return "RemoveRegion";
        case 4: return "SetMix";
    }
    return "?";
}

namespace {

struct PlaylistAt {
    std::size_t track = 0;
    std::size_t playlist = 0;
    const Playlist* p = nullptr;
};

bool locate(const EditState& e, Id playlist, PlaylistAt& at) {
    for (std::size_t t = 0; t < e.tracks.size(); ++t) {
        const auto& pls = e.tracks[t]->playlists;
        for (std::size_t i = 0; i < pls.size(); ++i) {
            if (pls[i]->id == playlist) {
                at = {t, i, pls[i].get()};
                return true;
            }
        }
    }
    return false;
}

bool id_in_use(const Snapshot& s, Id id) {
    if (id == kNoId) return true;
    for (const auto& t : s.edit->tracks) {
        if (t->id == id) return true;
        for (const auto& p : t->playlists) {
            if (p->id == id) return true;
            for (const auto& r : p->regions) {
                if (r.id == id) return true;
            }
        }
    }
    return s.sources->find(id) != nullptr;
}

// The frames of source material available from the region's first source
// frame: the region may not reach past the shortest source it plays.
Frames source_room(const SourcePool& pool, const Region& r) {
    Frames room = INT64_MAX;
    for (const auto& c : r.channels) {
        const Source* s = pool.find(c.source);
        if (s == nullptr) return 0;
        room = std::min(room, s->length);
    }
    return room == INT64_MAX ? 0 : room;
}

std::string id_text(Id id) { return std::to_string(id); }

// A new EditState with one playlist replaced; every other track, and every
// other playlist of the touched track, is shared with `before`.
std::shared_ptr<const EditState> with_playlist(const EditState& before, const PlaylistAt& at,
                                               Playlist replacement) {
    auto track = std::make_shared<TrackEdit>(*before.tracks[at.track]);
    track->playlists[at.playlist] = std::make_shared<const Playlist>(std::move(replacement));
    auto edit = std::make_shared<EditState>(before);
    edit->tracks[at.track] = std::move(track);
    return edit;
}

void sort_regions(std::vector<Region>& rs) {
    std::sort(rs.begin(), rs.end(),
              [](const Region& a, const Region& b) { return a.position < b.position; });
}

// session-format.md / timeline-model.md, "Overwrite": clears [a, b) in
// `rs`. Regions inside go, a region straddling a is cut to end at a, one
// straddling b starts at b, and one spanning both is split, its right part
// taking `split_id`.
bool overwrite(std::vector<Region>& rs, Frames a, Frames b, Id split_id, std::string& why) {
    std::vector<Region> out;
    out.reserve(rs.size() + 1);
    for (const Region& r : rs) {
        if (r.end() <= a || r.position >= b) {
            out.push_back(r);
            continue;
        }
        const bool keeps_left = r.position < a;
        const bool keeps_right = r.end() > b;
        if (keeps_left && keeps_right) {
            if (split_id == kNoId) {
                why = "the region would land inside region " + id_text(r.id) +
                      " and splitting it needs an id the operation does not carry";
                return false;
            }
            Region left = r;
            left.length = a - r.position;
            left.fade_out = {};
            Region right = r;
            right.id = split_id;
            right.source_offset += b - r.position;
            right.position = b;
            right.length = r.end() - b;
            right.fade_in = {};
            out.push_back(left);
            out.push_back(right);
        } else if (keeps_left) {
            Region left = r;
            left.length = a - r.position;
            left.fade_out = {};
            out.push_back(left);
        } else if (keeps_right) {
            Region right = r;
            right.source_offset += b - r.position;
            right.position = b;
            right.length = r.end() - b;
            right.fade_in = {};
            out.push_back(right);
        }
        // else: wholly covered, removed.
    }
    rs = std::move(out);
    return true;
}

std::size_t index_of(const Playlist& p, Id region) {
    for (std::size_t i = 0; i < p.regions.size(); ++i) {
        if (p.regions[i].id == region) return i;
    }
    return p.regions.size();
}

bool find_target(const Snapshot& s, Id playlist, Id region, PlaylistAt& at, std::size_t& idx,
                 std::string& why) {
    if (!locate(*s.edit, playlist, at)) {
        why = "there is no playlist " + id_text(playlist);
        return false;
    }
    idx = index_of(*at.p, region);
    if (idx == at.p->regions.size()) {
        why = "playlist " + id_text(playlist) + " has no region " + id_text(region);
        return false;
    }
    return true;
}

bool apply_move(const Snapshot& s, const MoveRegion& op, Snapshot& out, std::string& why) {
    PlaylistAt at;
    std::size_t idx = 0;
    if (!find_target(s, op.playlist, op.region, at, idx, why)) return false;
    if (op.to < 0) {
        why = "a region cannot start before the session start";
        return false;
    }
    if (op.split_id != kNoId && id_in_use(s, op.split_id)) {
        why = "id " + id_text(op.split_id) + " is already in use";
        return false;
    }
    Playlist p = *at.p;
    Region moved = p.regions[idx];
    p.regions.erase(p.regions.begin() + static_cast<std::ptrdiff_t>(idx));
    moved.position = op.to;
    if (!overwrite(p.regions, moved.position, moved.end(), op.split_id, why)) return false;
    p.regions.push_back(moved);
    sort_regions(p.regions);
    out = s;
    out.edit = with_playlist(*s.edit, at, std::move(p));
    return true;
}

bool apply_trim(const Snapshot& s, const TrimRegion& op, Snapshot& out, std::string& why) {
    PlaylistAt at;
    std::size_t idx = 0;
    if (!find_target(s, op.playlist, op.region, at, idx, why)) return false;
    Playlist p = *at.p;
    Region& r = p.regions[idx];
    const Frames room = source_room(*s.sources, r);
    if (op.edge == Edge::Start) {
        const Frames earliest = r.position - r.source_offset;
        const Frames prev_end = idx > 0 ? p.regions[idx - 1].end() : 0;
        if (op.to >= r.end()) {
            why = "trimming the start past the end would leave nothing";
            return false;
        }
        if (op.to < earliest || op.to < 0) {
            why = "there is no source material before the region's start";
            return false;
        }
        if (op.to < prev_end) {
            why = "the start would overlap the region before it";
            return false;
        }
        const Frames end = r.end();
        r.source_offset += op.to - r.position;
        r.position = op.to;
        r.length = end - op.to;
    } else {
        const Frames latest = r.position + (room - r.source_offset);
        const Frames next_start =
            idx + 1 < p.regions.size() ? p.regions[idx + 1].position : INT64_MAX;
        if (op.to <= r.position) {
            why = "trimming the end past the start would leave nothing";
            return false;
        }
        if (op.to > latest) {
            why = "there is no source material after the region's end";
            return false;
        }
        if (op.to > next_start) {
            why = "the end would overlap the region after it";
            return false;
        }
        r.length = op.to - r.position;
    }
    if (r.fade_in.length + r.fade_out.length > r.length) {
        r.fade_in.length = std::min(r.fade_in.length, r.length / 2);
        r.fade_out.length = std::min(r.fade_out.length, r.length - r.fade_in.length);
    }
    out = s;
    out.edit = with_playlist(*s.edit, at, std::move(p));
    return true;
}

bool apply_split(const Snapshot& s, const SplitRegion& op, Snapshot& out, std::string& why) {
    PlaylistAt at;
    std::size_t idx = 0;
    if (!find_target(s, op.playlist, op.region, at, idx, why)) return false;
    if (id_in_use(s, op.new_id)) {
        why = op.new_id == kNoId ? std::string("a split needs a new id for its right part")
                                 : "id " + id_text(op.new_id) + " is already in use";
        return false;
    }
    Playlist p = *at.p;
    const Region r = p.regions[idx];
    if (op.at <= r.position || op.at >= r.end()) {
        why = "the split point is not inside the region";
        return false;
    }
    Region left = r;
    left.length = op.at - r.position;
    left.fade_out = {};
    Region right = r;
    right.id = op.new_id;
    right.name = r.name + "-01";
    right.source_offset += op.at - r.position;
    right.position = op.at;
    right.length = r.end() - op.at;
    right.fade_in = {};
    p.regions[idx] = left;
    p.regions.insert(p.regions.begin() + static_cast<std::ptrdiff_t>(idx) + 1, right);
    out = s;
    out.edit = with_playlist(*s.edit, at, std::move(p));
    return true;
}

bool apply_remove(const Snapshot& s, const RemoveRegion& op, Snapshot& out, std::string& why) {
    PlaylistAt at;
    std::size_t idx = 0;
    if (!find_target(s, op.playlist, op.region, at, idx, why)) return false;
    Playlist p = *at.p;
    p.regions.erase(p.regions.begin() + static_cast<std::ptrdiff_t>(idx));
    out = s;
    out.edit = with_playlist(*s.edit, at, std::move(p));
    return true;
}

bool apply_mix(const Snapshot& s, const SetMix& op, Snapshot& out, std::string& why) {
    if (!std::isfinite(op.value)) {
        why = "a mix value must be a finite number";
        return false;
    }
    const auto gain = [](float v) { return std::clamp(v, kMinGainDb, kMaxGainDb); };
    if (op.field == MixField::BusGainDb) {
        out = s;
        out.mix.bus.gain_db = gain(op.value);
        return true;
    }
    for (std::size_t i = 0; i < s.mix.tracks.size(); ++i) {
        if (s.mix.tracks[i].track != op.track) continue;
        out = s;
        TrackMix& m = out.mix.tracks[i];
        switch (op.field) {
            case MixField::GainDb: m.gain_db = gain(op.value); break;
            case MixField::Pan: m.pan = std::clamp(op.value, -1.0f, 1.0f); break;
            case MixField::Mute: m.mute = op.value != 0.0f; break;
            case MixField::Solo: m.solo = op.value != 0.0f; break;
            case MixField::BusGainDb: break;
        }
        return true;
    }
    why = "there is no track " + id_text(op.track);
    return false;
}

}  // namespace

bool apply(const Snapshot& before, const Operation& op, Snapshot& out, std::string& why) {
    why.clear();
    Snapshot next;
    bool ok = false;
    if (const auto* m = std::get_if<MoveRegion>(&op)) ok = apply_move(before, *m, next, why);
    else if (const auto* t = std::get_if<TrimRegion>(&op)) ok = apply_trim(before, *t, next, why);
    else if (const auto* sp = std::get_if<SplitRegion>(&op)) ok = apply_split(before, *sp, next, why);
    else if (const auto* r = std::get_if<RemoveRegion>(&op)) ok = apply_remove(before, *r, next, why);
    else if (const auto* x = std::get_if<SetMix>(&op)) ok = apply_mix(before, *x, next, why);
    if (!ok) return false;
    next.revision = before.revision + 1;
    out = std::move(next);
    return true;
}

std::string check_invariants(const Snapshot& s) {
    if (!s.edit || !s.sources) return "snapshot has no edit state or source pool";
    if (s.mix.tracks.size() != s.edit->tracks.size()) return "mix and edit disagree on tracks";
    for (const auto& t : s.edit->tracks) {
        const Playlist* p = t->active();
        if (p == nullptr) return "track " + id_text(t->id) + " has no active playlist";
        for (std::size_t i = 0; i < p->regions.size(); ++i) {
            const Region& r = p->regions[i];
            if (r.length <= 0) return "region " + id_text(r.id) + " has no length";
            if (r.position < 0) return "region " + id_text(r.id) + " starts before 0";
            if (r.source_offset < 0 || r.source_offset + r.length > source_room(*s.sources, r)) {
                return "region " + id_text(r.id) + " reaches outside its source";
            }
            if (i > 0 && p->regions[i - 1].end() > r.position) {
                return "regions " + id_text(p->regions[i - 1].id) + " and " + id_text(r.id) +
                       " overlap or are out of order";
            }
        }
    }
    return {};
}

}  // namespace sonora::ui::model
