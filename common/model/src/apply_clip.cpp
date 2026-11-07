// SPDX-License-Identifier: GPL-3.0-or-later
//
// apply() for the family ClipOp (ADR-0059):
// a region's own attributes and the fades between regions.
#include "apply_detail.h"

namespace sonora::model::apply_detail {

bool Applier::operator()(const SetFade& op) {
    if (!not_midi(op.playlist, "a MIDI region has no fades")) return false;
    return edit_playlist(op.playlist, [&](Playlist& p, std::uint16_t) {
        const std::size_t i = region_index(p, op.region);
        if (i == kNone) return no_region(op.region, op.playlist);
        (op.end == FadeEnd::In ? p.regions[i].fade_in : p.regions[i].fade_out) =
            op.fade;
        mark(i, i);
        return true;
    });
}

bool Applier::operator()(const SetCrossfade& op) {
    if (!not_midi(op.playlist, "MIDI regions have no crossfades")) return false;
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

bool Applier::operator()(const RemoveCrossfade& op) {
    if (!not_midi(op.playlist, "MIDI regions have no crossfades")) return false;
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

bool Applier::operator()(const SetRegionName& op) {
    if (!detail::valid_text(op.name)) return fail("name is not valid UTF-8");
    return edit_region(op.playlist, op.region,
                       [&](auto& r) { r.name = op.name; });
}

bool Applier::operator()(const SetRegionColour& op) {
    if (!detail::region_colour_ok(op.colour)) return fail(detail::region_colour_problem());
    return edit_region(op.playlist, op.region,
                       [&](auto& r) { r.colour = op.colour; });
}

bool Applier::operator()(const SetRegionGain& op) {
    if (!(op.gain_db >= kMinGainDb && op.gain_db <= kMaxGainDb)) {
        return fail("gain_db must be from -144 to 24 dB");
    }
    const std::string bad = gain_line_problem(op.gain_line);
    if (!bad.empty()) return fail(bad);
    return edit_playlist(op.playlist, [&](Playlist& p, std::uint16_t) {
        const std::size_t i = region_index(p, op.region);
        if (i == kNone) return no_region(op.region, op.playlist);
        p.regions[i].gain_db = op.gain_db;
        p.regions[i].gain_line = op.gain_line;
        mark(i, i);
        return true;
    });
}

bool Applier::operator()(const SetClipGroup& op) {
    if (op.members.empty()) return fail("a clip group needs at least one region");
    if (op.members.size() > kMaxCompoundMembers) {
        return fail("a clip group holds at most " +
                    str(static_cast<std::uint64_t>(kMaxCompoundMembers)) + " regions");
    }
    return edit_playlist(op.playlist, [&](Playlist& p, std::uint16_t) {
        std::vector<std::size_t> at;
        bool names_group = op.group == 0;
        for (const ClipGroupMember& m : op.members) {
            const std::size_t i = region_index(p, m.region);
            if (i == kNone) return no_region(m.region, op.playlist);
            if (std::find(at.begin(), at.end(), i) != at.end()) {
                return fail("region " + str(m.region) + " is named twice");
            }
            if (op.group == 0 && m.loop != 0) {
                return fail("an ungrouped region has no loop number");
            }
            if (op.group != 0 && m.loop != 0 && m.region == op.group) {
                return fail("the region that names a group is not a repeat");
            }
            if (m.region == op.group) names_group = true;
            at.push_back(i);
        }
        if (!names_group) {
            return fail("a clip group is named by one of its regions; " + str(op.group) +
                        " is not among them");
        }
        if (op.group != 0) {
            for (std::size_t i = 0; i < p.regions.size(); ++i) {
                if (p.regions[i].clip_group == op.group &&
                    std::find(at.begin(), at.end(), i) == at.end()) {
                    return fail("clip group " + str(op.group) +
                                " already holds a region the operation does not name");
                }
            }
        }
        for (std::size_t k = 0; k < at.size(); ++k) {
            Region& r = p.regions[at[k]];
            r.clip_group = op.group;
            r.loop = op.group == 0 ? std::uint16_t{0} : op.members[k].loop;
            mark(at[k], at[k]);
        }
        return true;
    });
}

bool Applier::operator()(const SetRegionSources& op) {
    const std::string bad = gain_line_problem(op.gain_line);
    if (!bad.empty()) return fail(bad);
    return edit_playlist(op.playlist, [&](Playlist& p, std::uint16_t track_channels) {
        const std::size_t i = region_index(p, op.region);
        if (i == kNone) return no_region(op.region, op.playlist);
        Region r = p.regions[i];
        r.channels = op.channels;
        r.source_offset = op.source_offset;
        r.gain_line = op.gain_line;
        const std::string why = detail::region_problem(r, *before_.sources, track_channels);
        if (!why.empty()) return fail("region " + str(op.region) + ": " + why);
        p.regions[i] = std::move(r);
        mark(i, i);
        drop_broken_crossfades(p);
        return true;
    });
}

}  // namespace sonora::model::apply_detail
