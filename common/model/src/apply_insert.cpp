// SPDX-License-Identifier: GPL-3.0-or-later
//
// apply() for the family InsertOp (ADR-0059):
// plug-in inserts and their values.
#include "apply_detail.h"

namespace sonora::model::apply_detail {

bool Applier::operator()(const AddInsert& op) {
    std::string where;
    std::string why = detail::plugin_problem(op.plugin, where);
    if (!why.empty()) return fail(where + ": " + why);
    InsertMix values{op.insert, false, op.params, op.state};
    why = detail::insert_mix_problem(values, op.plugin.format, where);
    if (!why.empty()) return fail(where + ": " + why);
    if (!fresh(op.insert)) return false;
    const bool ok = edit_track(op.track, [&](TrackEdit& t) {
        if (t.kind == TrackKind::Midi) return no_audio(t.id);
        if (!free_slot(t, op.slot, 0)) return false;
        insert_by_slot(t.inserts, Insert{op.insert, op.slot, op.plugin, true});
        next_id_ = op.insert + 1;
        return true;
    });
    if (ok) set_insert_mix(after_.mix, std::move(values));
    return ok;
}

bool Applier::operator()(const RemoveInsert& op) {
    return edit_insert_dropping_lanes(op.insert, [&](std::vector<Insert>& v, std::size_t i) {
        v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    });
}

bool Applier::operator()(const MoveInsert& op) {
    InsertAt at;
    if (!locate_insert(*before_.edit, op.insert, at)) return no_insert(op.insert);
    const std::size_t to = locate_track(*before_.edit, op.to_track);
    if (to == kNone) return fail("track " + str(op.to_track) + " does not exist");
    if (before_.edit->tracks[to]->kind == TrackKind::Midi) return no_audio(op.to_track);
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
    // A keyed insert takes its key along (ADR-0058): the chain it joins may
    // hold a key already, or feed the key bus.
    if (to != at.track) {
        const std::string refused =
            detail::key_refusal(*before_.edit, *to_track, op.insert, moving.key_bus);
        if (!refused.empty()) return fail(refused);
    }
    moving.slot = op.to_slot;
    insert_by_slot(to_track->inserts, std::move(moving));
    if (to != at.track) {
        // Its plugin lanes go with it: they key the insert, not the track.
        auto& from_lanes = from_track->automation;
        auto& to_lanes = to_track->automation;
        const auto first = std::stable_partition(
            from_lanes.begin(), from_lanes.end(),
            [&](const std::shared_ptr<const AutomationLane>& l) {
                return !(l->param == AutomationParam::Plugin && l->insert == op.insert);
            });
        if (first != from_lanes.end()) {
            const auto mid = to_lanes.insert(to_lanes.end(), first, from_lanes.end());
            from_lanes.erase(first, from_lanes.end());
            std::inplace_merge(to_lanes.begin(), mid, to_lanes.end(),
                               [](const std::shared_ptr<const AutomationLane>& a,
                                  const std::shared_ptr<const AutomationLane>& b) {
                                   return lane_before(*a, *b);
                               });
        }
    }
    edit->tracks[at.track] = std::move(from_track);
    if (to != at.track) edit->tracks[to] = std::move(to_track);
    after_ = before_;
    after_.edit = std::move(edit);
    return true;
}

bool Applier::operator()(const SetInsertActive& op) {
    return edit_insert(op.insert, [&](std::vector<Insert>& v, std::size_t i) {
        v[i].active = op.active;
        return true;
    });
}

bool Applier::operator()(const ReplaceInsert& op) {
    std::string where;
    std::string why = detail::plugin_problem(op.plugin, where);
    if (!why.empty()) return fail(where + ": " + why);
    InsertMix values{op.replacement, false, op.params, op.state};
    why = detail::insert_mix_problem(values, op.plugin.format, where);
    if (!why.empty()) return fail(where + ": " + why);
    if (!fresh(op.replacement)) return false;
    // The replacement's keys mean other things, even for another version
    // of the same plugin, so the old insert's lanes go with it.
    const bool ok = edit_insert_dropping_lanes(op.insert, [&](std::vector<Insert>& v,
                                                              std::size_t i) {
        v[i] = Insert{op.replacement, v[i].slot, op.plugin, v[i].active};
        return true;
    });
    if (ok) {
        after_.next_id = op.replacement + 1;
        set_insert_mix(after_.mix, std::move(values));
    }
    return ok;
}

bool Applier::operator()(const SetInsertBypass& op) {
    return edit_insert_mix(op.insert, [&](InsertMix& m) {
        m.bypass = op.bypass;
        return true;
    });
}

bool Applier::operator()(const SetInsertParams& op) {
    return edit_insert_mix(op.insert, [&](InsertMix& m) {
        m.params = op.params;
        return true;
    });
}

bool Applier::operator()(const SetInsertState& op) {
    return edit_insert_mix(op.insert, [&](InsertMix& m) {
        m.state = op.state;
        return true;
    });
}

}  // namespace sonora::model::apply_detail
