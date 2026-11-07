// SPDX-License-Identifier: GPL-3.0-or-later
//
// The lookups and copies the operations share (apply_detail.h).
#include "apply_detail.h"

namespace sonora::model::apply_detail {

bool locate_playlist(const EditState& e, Id playlist, Located& at) {
    for (std::size_t t = 0; t < e.tracks.size(); ++t) {
        const auto& pls = e.tracks[t]->playlists;
        for (std::size_t p = 0; p < pls.size(); ++p) {
            if (pls[p]->id == playlist) {
                at.track = t;
                at.playlist = p;
                return true;
            }
        }
    }
    return false;
}

bool locate_midi_playlist(const EditState& e, Id playlist, Located& at) {
    for (std::size_t t = 0; t < e.tracks.size(); ++t) {
        const auto& pls = e.tracks[t]->midi_playlists;
        for (std::size_t p = 0; p < pls.size(); ++p) {
            if (pls[p]->id == playlist) {
                at.track = t;
                at.playlist = p;
                return true;
            }
        }
    }
    return false;
}

std::size_t midi_region_index(const MidiPlaylist& p, Id region) {
    for (std::size_t i = 0; i < p.regions.size(); ++i) {
        if (p.regions[i].id == region) return i;
    }
    return std::numeric_limits<std::size_t>::max();
}

void insert_midi_region(MidiPlaylist& p, MidiRegion r) {
    const auto it = std::lower_bound(
        p.regions.begin(), p.regions.end(), r.position,
        [](const MidiRegion& a, Ticks pos) { return a.position < pos; });
    p.regions.insert(it, std::move(r));
}

std::size_t locate_track(const EditState& e, Id track) {
    for (std::size_t t = 0; t < e.tracks.size(); ++t) {
        if (e.tracks[t]->id == track) return t;
    }
    return kNone;
}

std::shared_ptr<Playlist> copy_playlist(const Playlist& p, const std::string& name, Id& next) {
    auto c = std::make_shared<Playlist>();
    c->id = next++;
    c->name = name;
    std::vector<std::pair<Id, Id>> ids;   // source region id, copy's
    c->regions = p.regions;
    for (Region& r : c->regions) {
        ids.emplace_back(r.id, next);
        r.id = next++;
    }
    const auto map = [&](Id id) {
        for (const auto& [from, to] : ids)
            if (from == id) return to;
        return Id{0};
    };
    c->crossfades = p.crossfades;
    for (Crossfade& x : c->crossfades) {
        x.left = map(x.left);
        x.right = map(x.right);
    }
    return c;
}

std::shared_ptr<MidiPlaylist> copy_playlist(const MidiPlaylist& p, const std::string& name,
                                            Id& next) {
    auto c = std::make_shared<MidiPlaylist>();
    c->id = next++;
    c->name = name;
    // Each region keeps its pointer to the sequence: copy-on-write
    // (ADR-0014 §1), so the copy holds no notes of its own.
    c->regions = p.regions;
    for (MidiRegion& r : c->regions) r.id = next++;
    return c;
}

bool locate_insert(const EditState& e, Id insert, InsertAt& at) {
    for (std::size_t t = 0; t < e.tracks.size(); ++t) {
        const auto& ins = e.tracks[t]->inserts;
        for (std::size_t i = 0; i < ins.size(); ++i) {
            if (ins[i].id == insert) {
                at.track = t;
                at.index = i;
                return true;
            }
        }
    }
    return false;
}

void insert_by_slot(std::vector<Insert>& v, Insert in) {
    const auto it = std::lower_bound(
        v.begin(), v.end(), in.slot,
        [](const Insert& a, std::uint8_t slot) { return a.slot < slot; });
    v.insert(it, std::move(in));
}

const Insert* in_slot(const std::vector<Insert>& v, std::uint8_t slot) {
    for (const Insert& in : v) {
        if (in.slot == slot) return &in;
    }
    return nullptr;
}

std::string slot_name(std::uint8_t slot) {
    return slot < kInsertSlots ? std::string(1, static_cast<char>('A' + slot))
                               : std::to_string(slot);
}

std::size_t region_index(const Playlist& p, Id region) {
    for (std::size_t i = 0; i < p.regions.size(); ++i) {
        if (p.regions[i].id == region) return i;
    }
    return kNone;
}

#if SONORA_MODEL_AUDIT
namespace reference {

void insert_sorted(Playlist& p, Region r) {
    const auto it = std::lower_bound(
        p.regions.begin(), p.regions.end(), r.position,
        [](const Region& a, Frames pos) { return a.position < pos; });
    p.regions.insert(it, std::move(r));
}

bool overwrite(Playlist& p, Frames a, Frames b, Id split_id, bool& used_split,
               std::string& error) {
    std::vector<Region> out;
    out.reserve(p.regions.size() + 1);
    for (Region& r : p.regions) {
        const Frames s = r.position;
        const Frames e = r.end();
        if (e <= a || s >= b) {
            out.push_back(std::move(r));
        } else if (s >= a && e <= b) {
            // Covered: removed. Its source stays in the pool.
        } else if (s < a && e > b) {
            if (split_id == 0) {
                error = "region " + str(r.id) +
                        " spans the whole range and must be split, but the"
                        " operation carries no split id";
                return false;
            }
            Region right = r;
            right.id = split_id;
            right.position = b;
            right.source_offset += b - s;
            right.length = e - b;
            right.fade_in = Fade{};
            clamp_fades(right);
            // The crossfade at r's old end now belongs to the right part.
            for (Crossfade& x : p.crossfades) {
                if (x.left == r.id) x.left = split_id;
            }
            r.length = a - s;
            r.fade_out = Fade{};
            clamp_fades(r);
            out.push_back(std::move(r));
            out.push_back(std::move(right));
            used_split = true;
        } else if (s < a) {
            r.length = a - s;
            r.fade_out = Fade{};
            clamp_fades(r);
            out.push_back(std::move(r));
        } else {
            r.source_offset += b - s;
            r.length = e - b;
            r.position = b;
            r.fade_in = Fade{};
            clamp_fades(r);
            out.push_back(std::move(r));
        }
    }
    p.regions = std::move(out);
    return true;
}

void drop_broken_crossfades(Playlist& p, const SourcePool& pool) {
    // Both maps are over every region, built for nothing when there is no
    // crossfade to keep or drop.
    if (p.crossfades.empty()) return;
    std::unordered_map<Id, std::pair<Frames, Frames>> used;   // in_after, out_before
    std::unordered_map<Id, Frames> length;
    for (const Region& r : p.regions) length[r.id] = r.length;
    std::vector<Crossfade> kept;
    for (const Crossfade& x : p.crossfades) {
        if (!detail::crossfade_problem(x, p, pool).empty()) continue;
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        auto& l = used[x.left];
        auto& r = used[x.right];
        if (l.second != 0 || r.first != 0) continue;
        if (l.first + before > length[x.left]) continue;
        if (after + r.second > length[x.right]) continue;
        l.second = before;
        r.first = after;
        kept.push_back(x);
    }
    p.crossfades = std::move(kept);
    detail::fit_fades_to_crossfades(p);
}

}  // namespace reference
#endif

}  // namespace sonora::model::apply_detail
