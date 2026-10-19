// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "checks.h"
#include "local_edit.h"
#include "sonora/model/op_codec.h"
#include "sonora/model/operations.h"

namespace sonora::model {

namespace {

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

std::string str(std::uint64_t n) { return std::to_string(n); }
std::string str(std::int64_t n) { return std::to_string(n); }

struct Located {
    std::size_t track = kNone;
    std::size_t playlist = kNone;
};

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

std::size_t locate_track(const EditState& e, Id track) {
    for (std::size_t t = 0; t < e.tracks.size(); ++t) {
        if (e.tracks[t]->id == track) return t;
    }
    return kNone;
}

struct InsertAt {
    std::size_t track = kNone;
    std::size_t index = kNone;
};

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

using detail::clamp_fades;

#if SONORA_MODEL_AUDIT
// The whole-playlist operators the range-local ones replaced (local_edit.h),
// kept verbatim in audited builds: every edit is run through both, and the
// results must be equal.
namespace reference {

void insert_sorted(Playlist& p, Region r) {
    const auto it = std::lower_bound(
        p.regions.begin(), p.regions.end(), r.position,
        [](const Region& a, Frames pos) { return a.position < pos; });
    p.regions.insert(it, std::move(r));
}

// Clears [a, b) on the playlist (timeline-model.md, "Overwrite"): a region
// inside goes, one straddling a is cut to end at a, one straddling b is cut
// to start at b, and one spanning both is split, its right part taking
// `split_id`. A cut edge loses its fade, which belonged to the old edge.
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

// After an edit moved region edges, a crossfade stays only if it would
// still be accepted as new: both regions there, butting, with the handle
// it needs, and not meeting another crossfade inside a region. The same
// rule decides for every crossfade, which is what "removed or kept by the
// same rule" asks of the overwrite.
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

class Applier {
public:
    Applier(const Snapshot& before, Snapshot& after, std::string& error, bool in_fold)
        : before_(before), after_(after), error_(error), in_fold_(in_fold) {}

    // apply() folds a compound itself; the visit never reaches here.
    bool operator()(const Compound&) { return fail("a compound inside a compound"); }

    // --- not undoable ---

    bool operator()(const AddSource& op) {
        const Source& s = op.source;
        const std::string why = detail::source_problem(s);
        if (!why.empty()) return fail("source " + str(s.id) + ": " + why);
        if (!fresh(s.id)) return false;
        for (const Source& existing : before_.sources->sources) {
            if (existing.path == s.path) {
                return fail("source " + str(s.id) + ": path \"" + s.path +
                            "\" is already source " + str(existing.id));
            }
        }
        // Every id below next_id is taken, so a fresh id is the largest and
        // appending keeps the pool sorted.
        auto pool = std::make_shared<SourcePool>(*before_.sources);
        pool->sources.push_back(s);
        after_ = before_;
        after_.sources = std::move(pool);
        after_.next_id = s.id + 1;
        return true;
    }

    bool operator()(const SetMix& op) {
        const std::size_t t = locate_track(*before_.edit, op.mix.track);
        if (t == kNone) return fail("track " + str(op.mix.track) + " does not exist");
        std::string where;
        const std::string why =
            detail::mix_problem(op.mix, before_.edit->tracks[t]->channels, where);
        if (!why.empty()) return fail("mix." + where + ": " + why);
        after_ = before_;
        set_mix(after_.mix, op.mix);
        return true;
    }

    bool operator()(const SetMonitorBus& op) {
        std::string where;
        const std::string why = detail::bus_problem(op.bus, where);
        if (!why.empty()) return fail("monitor_bus." + where + ": " + why);
        after_ = before_;
        after_.mix.bus = op.bus;
        return true;
    }

    bool operator()(const SetSettings& op) {
        std::string where;
        const std::string why = detail::settings_problem(op.settings, where);
        if (!why.empty()) return fail(where + ": " + why);
        if (op.settings.sample_rate != before_.settings.sample_rate) {
            return fail("the sample rate is fixed when the session is created"
                        " (every source was recorded at " +
                        std::to_string(before_.settings.sample_rate) + " Hz)");
        }
        after_ = before_;
        after_.settings = op.settings;
        return true;
    }

    // --- undoable ---

    bool operator()(const AddTrack& op) {
        if (op.channels < 1 || op.channels > 2) {
            return fail("a track has 1 or 2 channels in this version");
        }
        if (!detail::valid_text(op.name) || !detail::valid_text(op.playlist_name)) {
            return fail("a track or playlist name is not valid UTF-8");
        }
        if (!fresh(op.track) || !fresh(op.playlist)) return false;
        if (op.track == op.playlist) return fail("the track and its playlist need two ids");
        TrackMix mix = op.mix;
        mix.track = op.track;
        std::string where;
        const std::string why = detail::mix_problem(mix, op.channels, where);
        if (!why.empty()) return fail("mix." + where + ": " + why);

        auto playlist = std::make_shared<Playlist>();
        playlist->id = op.playlist;
        playlist->name = op.playlist_name;
        auto track = std::make_shared<TrackEdit>();
        track->id = op.track;
        track->name = op.name;
        track->channels = op.channels;
        track->active_playlist = op.playlist;
        track->playlists.push_back(std::move(playlist));
        auto edit = std::make_shared<EditState>(*before_.edit);
        edit->tracks.push_back(std::move(track));

        after_ = before_;
        after_.edit = std::move(edit);
        set_mix(after_.mix, mix);
        after_.next_id = std::max(op.track, op.playlist) + 1;
        return true;
    }

    bool operator()(const PlaceRegion& op) {
        return place(op.playlist, op.region, op.split_right_id, {});
    }

    bool operator()(const RecordTake& op) {
        Located at;
        if (!locate_playlist(*before_.edit, op.playlist, at)) {
            return fail("playlist " + str(op.playlist) + " does not exist");
        }
        if (before_.edit->tracks[at.track]->id != op.track) {
            return fail("playlist " + str(op.playlist) + " is not on track " +
                        str(op.track));
        }
        return place(op.playlist, op.region, op.split_right_id, op.crossfades);
    }

    bool operator()(const MoveRegion& op) {
        Id used_id = 0;
        const bool ok = edit_playlist(op.playlist, [&](Playlist& p, std::uint16_t) {
            const std::size_t i = region_index(p, op.region);
            if (i == kNone) return no_region(op.region, op.playlist);
            Region r = p.regions[i];
            erase_region(p, i);
            if (op.position < 0 || op.position > kMaxInteger - r.length) {
                return fail("position " + str(op.position) + " is outside the timeline");
            }
            r.position = op.position;
            bool used = false;
            if (!overwrite(p, r.position, r.end(), op.split_right_id, used)) return false;
            if (used) {
                if (!fresh(op.split_right_id)) return false;
                used_id = op.split_right_id;
            }
            insert_region(p, std::move(r));
            drop_broken_crossfades(p);
            return true;
        });
        if (ok && used_id != 0) after_.next_id = used_id + 1;
        return ok;
    }

    bool operator()(const TrimRegion& op) {
        return edit_playlist(op.playlist, [&](Playlist& p, std::uint16_t) {
            const std::size_t i = region_index(p, op.region);
            if (i == kNone) return no_region(op.region, op.playlist);
            Region r = p.regions[i];
            erase_region(p, i);
            const Frames start = r.position;
            const Frames end = r.end();
            if (op.edge == Edge::Start) {
                if (op.to >= end) return fail("trimming the start to or past the end");
                if (op.to < 0) return fail("trimming the start before timeline frame 0");
                // The audio stays where it is on the timeline: the source
                // frame under every timeline frame is unchanged.
                r.source_offset += op.to - start;
                if (r.source_offset < 0) {
                    return fail("region " + str(r.id) + " has only " +
                                str(r.source_offset + (start - op.to)) +
                                " frames of source before its start");
                }
                r.position = op.to;
                r.length = end - op.to;
            } else {
                if (op.to <= start) return fail("trimming the end to or before the start");
                if (op.to > kMaxInteger) return fail("trimming past the end of the timeline");
                r.length = op.to - start;
            }
            clamp_fades(r);
            // Extending never needs a split: the region itself bounds the
            // range on one side, so nothing beneath can span both ends.
            bool used = false;
            if (!overwrite(p, r.position, r.end(), 0, used)) return false;
            insert_region(p, std::move(r));
            drop_broken_crossfades(p);
            return true;
        });
    }

    bool operator()(const SplitRegion& op) {
        const bool ok = edit_playlist(op.playlist, [&](Playlist& p, std::uint16_t) {
            const std::size_t i = region_index(p, op.region);
            if (i == kNone) return no_region(op.region, op.playlist);
            const Region& r = p.regions[i];
            if (op.at <= r.position || op.at >= r.end()) {
                return fail("split point " + str(op.at) + " is not inside region " +
                            str(op.region));
            }
            if (!fresh(op.new_right_id)) return false;
            bool used = false;
            // A split is an overwrite of the empty range [at, at).
            if (!overwrite(p, op.at, op.at, op.new_right_id, used)) return false;
            drop_broken_crossfades(p);
            return true;
        });
        if (ok) after_.next_id = op.new_right_id + 1;
        return ok;
    }

    bool operator()(const RemoveRegion& op) {
        return edit_playlist(op.playlist, [&](Playlist& p, std::uint16_t) {
            const std::size_t i = region_index(p, op.region);
            if (i == kNone) return no_region(op.region, op.playlist);
            gone(op.region);
            erase_region(p, i);
            drop_broken_crossfades(p);
            return true;
        });
    }

    bool operator()(const SetFade& op) {
        return edit_playlist(op.playlist, [&](Playlist& p, std::uint16_t) {
            const std::size_t i = region_index(p, op.region);
            if (i == kNone) return no_region(op.region, op.playlist);
            (op.end == FadeEnd::In ? p.regions[i].fade_in : p.regions[i].fade_out) =
                op.fade;
            mark(i, i);
            return true;
        });
    }

    bool operator()(const SetCrossfade& op) {
        return edit_playlist(op.playlist, [&](Playlist& p, std::uint16_t) {
            const Crossfade& x = op.crossfade;
            const std::string why = crossfade_problem(x, p);
            if (!why.empty()) return fail("crossfade: " + why);
            auto it = std::find_if(p.crossfades.begin(), p.crossfades.end(),
                                   [&](const Crossfade& c) {
                                       return c.left == x.left && c.right == x.right;
                                   });
            if (it != p.crossfades.end()) {
                *it = x;
                mark_crossfade(p, x);
            } else {
                add_crossfade(p, x);
            }
            return true;
        });
    }

    bool operator()(const RemoveCrossfade& op) {
        return edit_playlist(op.playlist, [&](Playlist& p, std::uint16_t) {
            auto it = std::find_if(p.crossfades.begin(), p.crossfades.end(),
                                   [&](const Crossfade& c) {
                                       return c.left == op.left && c.right == op.right;
                                   });
            if (it == p.crossfades.end()) {
                return fail("no crossfade between regions " + str(op.left) + " and " +
                            str(op.right));
            }
            const Crossfade removed = *it;
            p.crossfades.erase(it);
            mark_crossfade(p, removed);
            // The fades kept under it come back; one that would run into
            // the crossfade at its region's other end shortens to meet it.
            // The operator never saw that fade, and cannot edit it while
            // the other end is crossfaded, so refusing would leave no way
            // out short of clearing the other crossfade first.
            fit_fades(p);
            return true;
        });
    }

    bool operator()(const AddPlaylist& op) {
        return edit_track(op.track, [&](TrackEdit& t) {
            if (!detail::valid_text(op.name)) return fail("name is not valid UTF-8");
            if (!fresh(op.playlist)) return false;
            auto p = std::make_shared<Playlist>();
            p->id = op.playlist;
            p->name = op.name;
            t.playlists.push_back(std::move(p));
            next_id_ = op.playlist + 1;
            return true;
        });
    }

    bool operator()(const SetActivePlaylist& op) {
        return edit_track(op.track, [&](TrackEdit& t) {
            for (const auto& p : t.playlists) {
                if (p->id == op.playlist) {
                    t.active_playlist = op.playlist;
                    return true;
                }
            }
            return fail("playlist " + str(op.playlist) + " is not on track " +
                        str(op.track));
        });
    }

    bool operator()(const SetTrackName& op) {
        return edit_track(op.track, [&](TrackEdit& t) {
            if (!detail::valid_text(op.name)) return fail("name is not valid UTF-8");
            t.name = op.name;
            return true;
        });
    }

    bool operator()(const ReorderTracks& op) {
        const auto& tracks = before_.edit->tracks;
        if (op.order.size() != tracks.size()) {
            return fail("the new order names " + str(static_cast<std::uint64_t>(op.order.size())) +
                        " tracks; the session has " +
                        str(static_cast<std::uint64_t>(tracks.size())));
        }
        auto edit = std::make_shared<EditState>(*before_.edit);
        std::vector<bool> taken(tracks.size(), false);
        for (std::size_t i = 0; i < op.order.size(); ++i) {
            const std::size_t t = locate_track(*before_.edit, op.order[i]);
            if (t == kNone) return fail("track " + str(op.order[i]) + " does not exist");
            if (taken[t]) return fail("track " + str(op.order[i]) + " is named twice");
            taken[t] = true;
            edit->tracks[i] = tracks[t];
        }
        after_ = before_;
        after_.edit = std::move(edit);
        return true;
    }

    bool operator()(const AddMarker& op) {
        return edit_markers([&](MarkerList& m) {
            const std::string why = detail::marker_problem(op.marker);
            if (!why.empty()) return fail("marker: " + why);
            if (!fresh(op.marker.id)) return false;
            m.markers.push_back(op.marker);
            next_id_ = op.marker.id + 1;
            return true;
        });
    }

    bool operator()(const MoveMarker& op) {
        return edit_markers([&](MarkerList& m) {
            for (Marker& k : m.markers) {
                if (k.id != op.marker) continue;
                k.position = op.position;
                const std::string why = detail::marker_problem(k);
                if (!why.empty()) return fail("marker: " + why);
                return true;
            }
            return fail("marker " + str(op.marker) + " does not exist");
        });
    }

    bool operator()(const RemoveMarker& op) {
        return edit_markers([&](MarkerList& m) {
            for (auto it = m.markers.begin(); it != m.markers.end(); ++it) {
                if (it->id == op.marker) {
                    m.markers.erase(it);
                    return true;
                }
            }
            return fail("marker " + str(op.marker) + " does not exist");
        });
    }

    // --- inserts: structure, undoable (O1's default) ---

    bool operator()(const AddInsert& op) {
        std::string where;
        std::string why = detail::plugin_problem(op.plugin, where);
        if (!why.empty()) return fail(where + ": " + why);
        InsertMix values{op.insert, false, op.params, op.state};
        why = detail::insert_mix_problem(values, op.plugin.format, where);
        if (!why.empty()) return fail(where + ": " + why);
        if (!fresh(op.insert)) return false;
        const bool ok = edit_track(op.track, [&](TrackEdit& t) {
            if (!free_slot(t, op.slot, 0)) return false;
            insert_by_slot(t.inserts, Insert{op.insert, op.slot, op.plugin, true});
            next_id_ = op.insert + 1;
            return true;
        });
        if (ok) set_insert_mix(after_.mix, std::move(values));
        return ok;
    }

    bool operator()(const RemoveInsert& op) {
        return edit_insert(op.insert, [&](std::vector<Insert>& v, std::size_t i) {
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        });
    }

    bool operator()(const MoveInsert& op) {
        InsertAt at;
        if (!locate_insert(*before_.edit, op.insert, at)) return no_insert(op.insert);
        const std::size_t to = locate_track(*before_.edit, op.to_track);
        if (to == kNone) return fail("track " + str(op.to_track) + " does not exist");
        // A chain runs at its track's channel count, in and out (ADR-0012
        // §7.1), and the plugin was chosen for this one. Moving it across
        // would keep a mono plugin on a stereo track, or the reverse.
        const std::uint16_t from_channels = before_.edit->tracks[at.track]->channels;
        const std::uint16_t to_channels = before_.edit->tracks[to]->channels;
        if (from_channels != to_channels) {
            return fail("insert " + str(op.insert) + " cannot move from a " +
                        str(static_cast<std::uint64_t>(from_channels)) + "-channel track onto a " +
                        str(static_cast<std::uint64_t>(to_channels)) +
                        "-channel one; add the plugin there instead");
        }
        auto edit = std::make_shared<EditState>(*before_.edit);
        auto from_track = std::make_shared<TrackEdit>(*edit->tracks[at.track]);
        Insert moving = from_track->inserts[at.index];
        from_track->inserts.erase(from_track->inserts.begin() +
                                  static_cast<std::ptrdiff_t>(at.index));
        std::shared_ptr<TrackEdit> to_track =
            to == at.track ? from_track : std::make_shared<TrackEdit>(*edit->tracks[to]);
        if (!free_slot(*to_track, op.to_slot, op.insert)) return false;
        moving.slot = op.to_slot;
        insert_by_slot(to_track->inserts, std::move(moving));
        edit->tracks[at.track] = std::move(from_track);
        if (to != at.track) edit->tracks[to] = std::move(to_track);
        after_ = before_;
        after_.edit = std::move(edit);
        return true;
    }

    bool operator()(const SetInsertActive& op) {
        return edit_insert(op.insert, [&](std::vector<Insert>& v, std::size_t i) {
            v[i].active = op.active;
            return true;
        });
    }

    bool operator()(const ReplaceInsert& op) {
        std::string where;
        std::string why = detail::plugin_problem(op.plugin, where);
        if (!why.empty()) return fail(where + ": " + why);
        InsertMix values{op.replacement, false, op.params, op.state};
        why = detail::insert_mix_problem(values, op.plugin.format, where);
        if (!why.empty()) return fail(where + ": " + why);
        if (!fresh(op.replacement)) return false;
        const bool ok = edit_insert(op.insert, [&](std::vector<Insert>& v, std::size_t i) {
            v[i] = Insert{op.replacement, v[i].slot, op.plugin, v[i].active};
            return true;
        });
        if (ok) {
            after_.next_id = op.replacement + 1;
            set_insert_mix(after_.mix, std::move(values));
        }
        return ok;
    }

    // --- inserts: values, mix state ---

    bool operator()(const SetInsertBypass& op) {
        return edit_insert_mix(op.insert, [&](InsertMix& m) {
            m.bypass = op.bypass;
            return true;
        });
    }

    bool operator()(const SetInsertParams& op) {
        return edit_insert_mix(op.insert, [&](InsertMix& m) {
            m.params = op.params;
            return true;
        });
    }

    bool operator()(const SetInsertState& op) {
        return edit_insert_mix(op.insert, [&](InsertMix& m) {
            m.state = op.state;
            return true;
        });
    }

private:
    bool no_insert(Id insert) {
        return fail("insert " + str(insert) + " is not on any track");
    }

    // `slot` on `t` holds nothing, or only the insert `moving` itself.
    bool free_slot(const TrackEdit& t, std::uint8_t slot, Id moving) {
        if (slot >= kInsertSlots) {
            return fail("slot " + str(static_cast<std::uint64_t>(slot)) +
                        " is not one of the " + str(static_cast<std::uint64_t>(kInsertSlots)) +
                        " insert slots (A to J)");
        }
        const Insert* there = in_slot(t.inserts, slot);
        if (there != nullptr && there->id != moving) {
            return fail("slot " + slot_name(slot) + " on track " + str(t.id) +
                        " already holds insert " + str(there->id) + " (" +
                        there->plugin.name + ")");
        }
        return true;
    }

    static void set_insert_mix(MixState& mix, InsertMix m) {
        for (InsertMix& existing : mix.inserts) {
            if (existing.insert == m.insert) {
                existing = std::move(m);
                return;
            }
        }
        mix.inserts.push_back(std::move(m));
    }

    // The track holding `insert` copied, its insert list edited by `f`.
    template <typename F>
    bool edit_insert(Id insert, F&& f) {
        InsertAt at;
        if (!locate_insert(*before_.edit, insert, at)) return no_insert(insert);
        return edit_track(before_.edit->tracks[at.track]->id,
                          [&](TrackEdit& t) { return f(t.inserts, at.index); });
    }

    // An insert's values, checked against its plugin's format. Only an
    // insert that is on a track: values left behind by a removal are kept
    // for undo, not edited.
    template <typename F>
    bool edit_insert_mix(Id insert, F&& f) {
        InsertAt at;
        if (!locate_insert(*before_.edit, insert, at)) return no_insert(insert);
        const Insert& in = before_.edit->tracks[at.track]->inserts[at.index];
        const InsertMix* existing = before_.mix.find_insert(insert);
        InsertMix m = existing != nullptr ? *existing : InsertMix{insert, false, {}, {}};
        if (!f(m)) return false;
        std::string where;
        const std::string why = detail::insert_mix_problem(m, in.plugin.format, where);
        if (!why.empty()) return fail("insert " + str(insert) + "." + where + ": " + why);
        after_ = before_;
        set_insert_mix(after_.mix, std::move(m));
        return true;
    }
    bool fail(std::string msg) {
        error_ = std::move(msg);
        return false;
    }

    bool no_region(Id region, Id playlist) {
        return fail("region " + str(region) + " is not on playlist " + str(playlist));
    }

    // Ids come from the engine's counter, so every new one is at least
    // next_id. That makes "unique in the session" an O(1) check.
    bool fresh(Id id) {
        if (id == 0) return fail("a new object needs an id; 0 is not one");
        if (id < before_.next_id) {
            return fail("id " + str(id) + " is already taken (new ids start at " +
                        str(before_.next_id) + ")");
        }
        if (id > static_cast<Id>(kMaxInteger)) return fail("id " + str(id) + " is out of range");
        return true;
    }

    static void set_mix(MixState& mix, const TrackMix& m) {
        for (TrackMix& existing : mix.tracks) {
            if (existing.track == m.track) {
                existing = m;
                return;
            }
        }
        mix.tracks.push_back(m);
    }

    bool place(Id playlist, const Region& region, Id split_id,
               const std::vector<Crossfade>& crossfades) {
        if (!fresh(region.id)) return false;
        Id top = region.id;
        const bool ok = edit_playlist(playlist, [&](Playlist& p, std::uint16_t ch) {
            const std::string why = detail::region_problem(region, *before_.sources, ch);
            if (!why.empty()) return fail("region " + str(region.id) + ": " + why);
            bool used = false;
            if (!overwrite(p, region.position, region.end(), split_id, used)) return false;
            if (used) {
                if (split_id == region.id) {
                    return fail("the split id must differ from the region's id");
                }
                if (!fresh(split_id)) return false;
                top = std::max(top, split_id);
            }
            insert_region(p, region);
            drop_broken_crossfades(p);
            for (const Crossfade& x : crossfades) {
                if (x.left != region.id && x.right != region.id) {
                    return fail("a take's crossfade must join the take (region " +
                                str(region.id) + ")");
                }
                const std::string bad = crossfade_problem(x, p);
                if (!bad.empty()) return fail("crossfade: " + bad);
                add_crossfade(p, x);
            }
            // A punch near a region's start must not be refused for that
            // region's fade-in: losing a take is worse than a shorter fade.
            fit_fades(p);
            return true;
        });
        if (ok) after_.next_id = top + 1;
        return ok;
    }

    bool refuse_playlist(Id playlist, const std::string& why, const std::string& where) {
        return fail("playlist " + str(playlist) + (where.empty() ? "" : "." + where) + ": " +
                    why);
    }

    // The playlist operators. Each records what it changed in `touched_`,
    // which is all that edit_playlist() checks afterwards. In an audited
    // build's reference run they are the whole-playlist versions instead.
    bool overwrite(Playlist& p, Frames a, Frames b, Id split_id, bool& used) {
#if SONORA_MODEL_AUDIT
        if (reference_) return reference::overwrite(p, a, b, split_id, used, error_);
#endif
        return detail::overwrite_range(p, a, b, split_id, used, error_, touched_);
    }

    void insert_region(Playlist& p, Region r) {
#if SONORA_MODEL_AUDIT
        if (reference_) return reference::insert_sorted(p, std::move(r));
#endif
        detail::insert_region(p, std::move(r), touched_);
    }

    void erase_region(Playlist& p, std::size_t i) {
#if SONORA_MODEL_AUDIT
        if (reference_) {
            p.regions.erase(p.regions.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
#endif
        detail::erase_region(p, i, touched_);
    }

    void gone(Id region) { touched_.add_gone(region); }

    void mark(std::size_t first, std::size_t last) { touched_.mark(first, last); }

    // The two regions a crossfade on `p` joins.
    void mark_crossfade(const Playlist& p, const Crossfade& x) {
        const std::size_t l = detail::find_index(p, x.left, touched_);
        const std::size_t r = detail::find_index(p, x.right, touched_);
        if (l != detail::kNoIndex && r != detail::kNoIndex) {
            mark(std::min(l, r), std::max(l, r));
        }
    }

    void drop_broken_crossfades(Playlist& p) {
#if SONORA_MODEL_AUDIT
        if (reference_) return reference::drop_broken_crossfades(p, *before_.sources);
#endif
        detail::drop_broken_crossfades_near(p, *before_.sources, touched_);
        detail::fit_fades_near(p, touched_);
    }

    void fit_fades(Playlist& p) {
#if SONORA_MODEL_AUDIT
        if (reference_) {
            detail::fit_fades_to_crossfades(p);
            return;
        }
#endif
        detail::fit_fades_near(p, touched_);
    }

    std::string crossfade_problem(const Crossfade& x, const Playlist& p) const {
        const std::size_t l = detail::find_index(p, x.left, touched_);
        const std::size_t r = detail::find_index(p, x.right, touched_);
        return detail::crossfade_problem(x, l == detail::kNoIndex ? nullptr : &p.regions[l],
                                         r == detail::kNoIndex ? nullptr : &p.regions[r],
                                         *before_.sources);
    }

    // One that passed crossfade_problem(), so both its regions are there.
    void add_crossfade(Playlist& p, const Crossfade& x) {
#if SONORA_MODEL_AUDIT
        if (reference_) {
            p.crossfades.push_back(x);
            return;
        }
#endif
        detail::insert_crossfade(p, x, detail::find_index(p, x.left, touched_));
        mark_crossfade(p, x);
    }

#if SONORA_MODEL_AUDIT
    // Audited builds (Debug, and the model's own tests) run every playlist
    // edit twice: through the whole-playlist operators and playlist_problem(),
    // as before BACKLOG Track 2 item 8, and through the range-local ones.
    // Both must reach the same verdict in the same words, and the same
    // playlist.
    struct Audit {
        bool ok = false;
        std::string error;
        Playlist playlist;
    };

    template <typename F>
    Audit run_reference(const Playlist& before, Id playlist, std::uint16_t channels, F& f) {
        Audit a;
        a.playlist = before;
        const std::string saved = error_;
        reference_ = true;
        a.ok = f(a.playlist, channels);
        reference_ = false;
        if (a.ok) {
            detail::sort_crossfades(a.playlist);
            std::string where;
            const std::string why =
                detail::playlist_problem(a.playlist, *before_.sources, channels, where);
            if (!why.empty()) a.ok = refuse_playlist(playlist, why, where);
        }
        a.error = error_;
        error_ = saved;
        return a;
    }

    [[noreturn]] static void audit_failed(const std::string& what) {
        std::fprintf(stderr, "common/model audit: %s\n", what.c_str());
        std::abort();
    }

    void audit_verdict(const Playlist& p, std::uint16_t channels, const std::string& why,
                       const std::string& where) const {
        std::string full_where;
        const std::string full =
            detail::playlist_problem(p, *before_.sources, channels, full_where);
        if (full != why || full_where != where) {
            audit_failed("the local check says \"" + where + ": " + why +
                         "\", playlist_problem says \"" + full_where + ": " + full + "\"");
        }
    }

    void audit_against_reference(const Audit& a, bool ok, const Playlist& p) const {
        if (a.ok != ok) {
            audit_failed(std::string("the whole-playlist edit ") +
                         (a.ok ? "succeeds" : "fails (" + a.error + ")") +
                         ", the range-local one " + (ok ? "succeeds" : "fails (" + error_ + ")"));
        }
        if (!ok && a.error != error_) {
            audit_failed("refused as \"" + a.error + "\" by the whole-playlist edit and as \"" +
                         error_ + "\" by the range-local one");
        }
        if (ok && !(a.playlist == p)) {
            audit_failed("the whole-playlist and range-local edits of playlist " + str(p.id) +
                         " differ");
        }
    }
#endif

    // Copies the one playlist, lets `f` edit the copy, checks the result
    // against every invariant where `f` could have broken one (local_edit.h),
    // and shares everything else with `before`.
    //
    // Inside a compound, a playlist an earlier member already copied is
    // edited where it is, and so are the track and edit state above it
    // (ADR-0009 update 2026-10-14, item 5): k members on one n-region
    // playlist then copy it once, not k times. Only when the whole chain
    // from the edit state down has one owner, which is then the fold's
    // working state: `before`, which the fold's caller holds, never does.
    //
    // The const_cast below is defined only because no Playlist, TrackEdit or
    // EditState anywhere in common/model is created const: every one is
    // make_shared<T>, never make_shared<const T> (the loader, the window
    // decoder and this file). Keep it that way.
    template <typename F>
    bool edit_playlist(Id playlist, F&& f) {
        Located at;
        if (!locate_playlist(*before_.edit, playlist, at)) {
            return fail("playlist " + str(playlist) + " does not exist");
        }
        const auto& track_ptr = before_.edit->tracks[at.track];
        const auto& playlist_ptr = track_ptr->playlists[at.playlist];
        const bool edit_mine = in_fold_ && before_.edit.use_count() == 1;
        const bool track_mine = edit_mine && track_ptr.use_count() == 1;
        const bool playlist_mine = track_mine && playlist_ptr.use_count() == 1;
        const std::uint16_t channels = track_ptr->channels;

        Playlist copy;
        if (!playlist_mine) copy = *playlist_ptr;
        Playlist& p = playlist_mine ? const_cast<Playlist&>(*playlist_ptr) : copy;
#if SONORA_MODEL_AUDIT
        const Audit reference = run_reference(*playlist_ptr, playlist, channels, f);
#endif
        touched_.clear();
        bool ok = f(p, channels);
        if (ok) {
            // The playlist was valid before the edit, so only what the edit
            // touched can be wrong now (local_edit.h). No crossfade sort either:
            // every operator keeps the list in region order.
            std::string where;
            const std::string why =
                detail::playlist_problem_near(p, *before_.sources, channels, touched_, where);
#if SONORA_MODEL_AUDIT
            audit_verdict(p, channels, why, where);
#endif
            if (!why.empty()) ok = refuse_playlist(playlist, why, where);
        }
#if SONORA_MODEL_AUDIT
        audit_against_reference(reference, ok, p);
#endif
        if (!ok) return false;
        if (playlist_mine) {
            after_ = before_;
            return true;
        }
        std::shared_ptr<const Playlist> made = std::make_shared<Playlist>(std::move(copy));
        if (track_mine) {
            const_cast<TrackEdit&>(*track_ptr).playlists[at.playlist] = std::move(made);
            after_ = before_;
            return true;
        }
        auto track = std::make_shared<TrackEdit>(*track_ptr);
        track->playlists[at.playlist] = std::move(made);
        if (edit_mine) {
            const_cast<EditState&>(*before_.edit).tracks[at.track] = std::move(track);
            after_ = before_;
            return true;
        }
        auto edit = std::make_shared<EditState>(*before_.edit);
        edit->tracks[at.track] = std::move(track);
        after_ = before_;
        after_.edit = std::move(edit);
        return true;
    }

    template <typename F>
    bool edit_track(Id track, F&& f) {
        const std::size_t t = locate_track(*before_.edit, track);
        if (t == kNone) return fail("track " + str(track) + " does not exist");
        auto copy = std::make_shared<TrackEdit>(*before_.edit->tracks[t]);
        next_id_ = before_.next_id;
        if (!f(*copy)) return false;
        auto edit = std::make_shared<EditState>(*before_.edit);
        edit->tracks[t] = std::move(copy);
        after_ = before_;
        after_.edit = std::move(edit);
        after_.next_id = next_id_;
        return true;
    }

    template <typename F>
    bool edit_markers(F&& f) {
        MarkerList m = *before_.edit->markers;
        next_id_ = before_.next_id;
        if (!f(m)) return false;
        detail::sort_markers(m);
        auto edit = std::make_shared<EditState>(*before_.edit);
        edit->markers = std::make_shared<const MarkerList>(std::move(m));
        after_ = before_;
        after_.edit = std::move(edit);
        after_.next_id = next_id_;
        return true;
    }

    const Snapshot& before_;
    Snapshot& after_;
    std::string& error_;
    const bool in_fold_;
    Id next_id_ = 0;
    detail::Touched touched_;
#if SONORA_MODEL_AUDIT
    bool reference_ = false;
#endif
};

bool fold(const Snapshot& before, const Compound& c, Snapshot& after, std::string& error,
          const MemberCheck& check) {
    if (c.ops.empty()) {
        error = "a compound needs at least one operation; this one is empty";
        return false;
    }
    if (c.ops.size() > kMaxCompoundMembers) {
        error = "at most " + std::to_string(kMaxCompoundMembers) +
                " operations in a compound; this one has " + std::to_string(c.ops.size());
        return false;
    }
    if (!detail::valid_text(c.label)) {
        error = "the compound's label is not valid UTF-8";
        return false;
    }
    // The fold's own state: it shares everything with `before` until a
    // member copies something, and from then on that copy is the fold's
    // alone (Applier::edit_playlist).
    Snapshot work = before;
    for (std::size_t i = 0; i < c.ops.size(); ++i) {
        const Member& m = c.ops[i];
        std::string why;
        if (check) why = check(work, MemberRef(m));
        bool ok = why.empty();
        if (ok) {
            Snapshot next;
            Applier applier(work, next, why, true);
            ok = std::visit(applier, m);
            if (ok) work = std::move(next);
        }
        if (!ok) {
            error = "operation " + std::to_string(i + 1) + " of " +
                    std::to_string(c.ops.size()) + " (" + member_name(m) + "): " + why;
            return false;
        }
    }
    after = std::move(work);
    return true;
}

struct ToMember {
    Member& out;
    template <typename T>
    bool operator()(T&& leaf) const {
        out = std::forward<T>(leaf);
        return true;
    }
    bool operator()(Compound&&) const { return false; }
};

}  // namespace

Operation to_operation(const Member& m) {
    return std::visit([](const auto& leaf) { return Operation(leaf); }, m);
}

bool to_member(Operation op, Member& out) { return std::visit(ToMember{out}, std::move(op)); }

namespace {

// Mix, plug-in values, sources and settings are outside edit undo.
bool leaf_is_undoable(MemberRef m) {
    return !(m.holds<AddSource>() || m.holds<SetMix>() || m.holds<SetMonitorBus>() ||
             m.holds<SetSettings>() || m.holds<SetInsertBypass>() ||
             m.holds<SetInsertParams>() || m.holds<SetInsertState>());
}

}  // namespace

bool member_is_undoable(const Member& m) { return leaf_is_undoable(MemberRef(m)); }

bool is_undoable(const Operation& op) {
    if (const auto* c = std::get_if<Compound>(&op)) {
        return std::any_of(c->ops.begin(), c->ops.end(), member_is_undoable);
    }
    return leaf_is_undoable(MemberRef(op));
}

bool apply(const Snapshot& before, const Operation& op, Snapshot& after,
           std::string& error, const MemberCheck& check) {
    Snapshot result;
    if (const auto* c = std::get_if<Compound>(&op)) {
        if (!fold(before, *c, result, error, check)) return false;
    } else {
        if (check) {
            std::string why = check(before, MemberRef(op));
            if (!why.empty()) {
                error = std::move(why);
                return false;
            }
        }
        Applier applier(before, result, error, false);
        if (!std::visit(applier, op)) return false;
    }
    result.revision = before.revision + 1;
    after = std::move(result);
    return true;
}

}  // namespace sonora::model
