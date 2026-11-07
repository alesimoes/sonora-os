// SPDX-License-Identifier: GPL-3.0-or-later
//
// apply() for the family RegionOp (ADR-0059):
// what places, moves, cuts or records audio regions.
#include "apply_detail.h"

namespace sonora::model::apply_detail {

bool Applier::operator()(const PlaceRegion& op) {
    if (!not_midi(op.playlist, "place a MIDI region on it instead")) return false;
    return place(op.playlist, op.region, op.split_right_id, {});
}

bool Applier::operator()(const RecordTake& op) {
    if (!not_midi(op.playlist, "a MIDI take is AddMidiSource and PlaceMidiRegion")) {
        return false;
    }
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

bool Applier::operator()(const RecordLoopTake& op) {
    if (!not_midi(op.active_playlist, "a MIDI take is AddMidiSource and PlaceMidiRegion")) {
        return false;
    }
    Located at;
    if (!locate_playlist(*before_.edit, op.active_playlist, at)) {
        return fail("playlist " + str(op.active_playlist) + " does not exist");
    }
    if (before_.edit->tracks[at.track]->id != op.track) {
        return fail("playlist " + str(op.active_playlist) + " is not on track " +
                    str(op.track));
    }
    if (op.passes.empty()) return fail("a loop take needs at least one pass");
    if (op.passes.size() > kMaxLoopTakePasses) {
        return fail("a loop take places at most " + std::to_string(kMaxLoopTakePasses) +
                    " passes; this one has " + std::to_string(op.passes.size()));
    }
    if (op.chosen_region != 0 && op.chosen >= op.passes.size()) {
        return fail("pass " + std::to_string(op.chosen + 1) + " is not one of the take's " +
                    std::to_string(op.passes.size()));
    }
    if (op.chosen_region == 0 && !op.crossfades.empty()) {
        return fail("a crossfade on the active playlist needs a pass placed there");
    }
    Snapshot work = before_;
    const auto step = [&](const Member& m, const std::string& which) {
        Snapshot next;
        std::string why;
        // The work state's own copies, as in a compound: what a step
        // made, a later one may edit in place.
        Applier applier(work, next, why, true);
        if (!m.visit(applier)) return fail(which + ": " + why);
        work = std::move(next);
        return true;
    };
    for (std::size_t k = 0; k < op.passes.size(); ++k) {
        const LoopTakePass& p = op.passes[k];
        const std::string which = "pass " + std::to_string(k + 1);
        if (!step(AddPlaylist{op.track, p.playlist, p.name}, which)) return false;
        if (!step(PlaceRegion{p.playlist, p.region, 0}, which)) return false;
    }
    if (op.chosen_region != 0) {
        RecordTake take;
        take.track = op.track;
        take.playlist = op.active_playlist;
        take.region = op.passes[op.chosen].region;
        take.region.id = op.chosen_region;
        take.split_right_id = op.split_right_id;
        take.crossfades = op.crossfades;
        if (!step(take, "the active playlist")) return false;
    }
    after_ = std::move(work);
    return true;
}

bool Applier::operator()(const MoveRegion& op) {
    if (!timebase_matches(op.playlist, op.timebase)) return false;
    if (op.timebase == Timebase::Ticks) return move_midi(op);
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

bool Applier::operator()(const TrimRegion& op) {
    if (!timebase_matches(op.playlist, op.timebase)) return false;
    if (op.timebase == Timebase::Ticks) return trim_midi(op);
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

bool Applier::operator()(const SplitRegion& op) {
    if (!timebase_matches(op.playlist, op.timebase)) return false;
    if (op.timebase == Timebase::Ticks) return split_midi(op);
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

bool Applier::operator()(const RemoveRegion& op) {
    Located midi;
    if (locate_midi_playlist(*before_.edit, op.playlist, midi)) {
        return edit_midi_playlist(op.playlist, [&](MidiPlaylist& p) {
            const std::size_t i = midi_region_index(p, op.region);
            if (i == kNone) return no_region(op.region, op.playlist);
            p.regions.erase(p.regions.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        });
    }
    return edit_playlist(op.playlist, [&](Playlist& p, std::uint16_t) {
        const std::size_t i = region_index(p, op.region);
        if (i == kNone) return no_region(op.region, op.playlist);
        gone(op.region);
        erase_region(p, i);
        drop_broken_crossfades(p);
        return true;
    });
}

bool Applier::operator()(const ShiftRegions& op) {
    if (!not_midi(op.playlist, "a MIDI playlist has no Shuffle ripple")) return false;
    if (op.delta == 0) return fail("shifting by 0 frames");
    return edit_playlist(op.playlist, [&](Playlist& p, std::uint16_t) {
        // Regions are sorted by position, so those that move are a tail.
        std::size_t first = p.regions.size();
        Frames stay_end = 0;
        for (std::size_t i = 0; i < p.regions.size(); ++i) {
            if (p.regions[i].position >= op.from) {
                first = i;
                break;
            }
            stay_end = std::max(stay_end, p.regions[i].end());
        }
        if (first == p.regions.size()) return fail("no region starts at or after " + str(op.from));
        const Frames lowest = p.regions[first].position;
        Frames highest_end = 0;
        for (std::size_t i = first; i < p.regions.size(); ++i) {
            highest_end = std::max(highest_end, p.regions[i].end());
        }
        if (op.delta < 0) {
            if (lowest + op.delta < 0) {
                return fail("shifting before timeline frame 0");
            }
            if (lowest + op.delta < stay_end) {
                return fail("shifting by " + str(op.delta) +
                            " would land on a region that stays");
            }
        } else if (op.delta > kMaxInteger - highest_end) {
            return fail("shifting past the end of the timeline");
        }
        std::vector<Id> ids;
        for (std::size_t i = first; i < p.regions.size(); ++i) ids.push_back(p.regions[i].id);
        // Later regions first when moving later, so none is moved onto one
        // not yet moved.
        if (op.delta > 0) std::reverse(ids.begin(), ids.end());
        for (const Id id : ids) {
            const std::size_t i = region_index(p, id);
            Region r = p.regions[i];
            erase_region(p, i);
            r.position += op.delta;
            insert_region(p, std::move(r));
        }
        drop_broken_crossfades(p);
        return true;
    });
}

}  // namespace sonora::model::apply_detail
