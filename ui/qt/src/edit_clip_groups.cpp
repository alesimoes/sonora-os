// SPDX-License-Identifier: GPL-3.0-or-later
//
// Clip groups, clip looping and the sample edits' requests (ADR-0052). A group
// is the clips of one playlist that share a `clip_group`; they move together.
// A loop is a group whose members past the first run are repeats of the first.
// Sample edits are made by the engine, which owns the audio files: this file
// only turns the selection into the request it takes.
#include <algorithm>
#include <cmath>

#include "edit_clipboard.h"
#include "edit_controller.h"

namespace sonora::ui {

using namespace model;

namespace {

std::string quoted(const Region& r) {
    return r.name.empty() ? "region " + std::to_string(r.id) : "\"" + r.name + "\"";
}

// The regions of `p` in group `group`, in playlist order.
std::vector<const Region*> members_of(const Playlist& p, Id group) {
    std::vector<const Region*> out;
    for (const Region& r : p.regions) {
        if (group != 0 && r.clip_group == group) out.push_back(&r);
    }
    return out;
}

}  // namespace

bool EditController::group_clips(std::string& why) {
    if (sel_.track < 0 || !sel_.has_range()) {
        why = "drag the Selector across the clips to group, then group them; nothing was grouped";
        return false;
    }
    const TrackEdit* t = track(sel_.track);
    const Playlist* p = t != nullptr ? active_playlist(t) : nullptr;
    if (p == nullptr) {
        why = "that track has no audio clips to group";
        return false;
    }
    SetClipGroup op;
    op.playlist = p->id;
    for (const Region& r : p->regions) {
        if (r.position >= sel_.end || r.end() <= sel_.start) continue;
        if (r.clip_group != 0) {
            why = quoted(r) + " is already in a clip group; ungroup it first. Nothing was grouped.";
            return false;
        }
        op.members.push_back({r.id, 0});
    }
    if (op.members.size() < 2) {
        why = "a clip group needs two or more clips; the range holds " +
              std::string(op.members.empty() ? "none" : "one") + ". Nothing was grouped.";
        return false;
    }
    op.group = op.members.front().region;
    if (!submit(op, why)) return false;
    select_region(op.group);
    return true;
}

bool EditController::ungroup_clip(std::string& why) {
    const TrackEdit* t = nullptr;
    if (selected_region(why) == nullptr) return false;
    const Region* r = find_region(snapshot(), sel_.region, &t);
    if (r->clip_group == 0) {
        why = quoted(*r) + " is not in a clip group";
        return false;
    }
    const Playlist* p = active_playlist(t);
    SetClipGroup op;
    op.playlist = p->id;
    op.group = 0;
    for (const Region* m : members_of(*p, r->clip_group)) op.members.push_back({m->id, 0});
    const Id keep = r->id;
    if (!submit(op, why)) return false;
    select_region(keep);
    return true;
}

bool EditController::loop_clip(int count, std::string& why) {
    if (count < 1) {
        why = "Loop needs a number of repeats of 1 or more; nothing was looped";
        return false;
    }
    if (refuse_shuffle("Loop", "looped", why)) return false;
    const TrackEdit* t = nullptr;
    if (selected_region(why) == nullptr) return false;
    const Region* sel = find_region(snapshot(), sel_.region, &t);
    const Playlist* p = active_playlist(t);
    const Id group = sel->clip_group != 0 ? sel->clip_group : sel->id;

    // What repeats: the clips the operator made, never an earlier loop's
    // repeats, which are taken out and laid again.
    std::vector<const Region*> origin, repeats;
    if (sel->clip_group == 0) {
        origin.push_back(sel);
    } else {
        for (const Region* m : members_of(*p, group)) (m->loop == 0 ? origin : repeats).push_back(m);
    }
    if (origin.empty()) {
        why = "the group holds only repeats; unloop it first. Nothing was looped.";
        return false;
    }
    Frames first = origin.front()->position, last = origin.front()->end();
    for (const Region* r : origin) {
        first = std::min(first, r->position);
        last = std::max(last, r->end());
    }
    const Frames span = last - first;
    if (static_cast<double>(span) * (count + 1) + static_cast<double>(first) >
        static_cast<double>(kMaxInteger)) {
        why = "that many repeats would run past the end of the timeline; nothing was looped";
        return false;
    }
    const std::size_t total = repeats.size() + origin.size() * static_cast<std::size_t>(count) + 1;
    const std::string too_many = over_limit("Loop", "looped", total);
    if (!too_many.empty()) {
        why = too_many;
        return false;
    }

    std::vector<Member> members;
    for (const Region* r : repeats) members.push_back(RemoveRegion{p->id, r->id});
    const Id first_id = origin.front()->id;
    const Id group_id = sel->clip_group != 0 ? group : first_id;
    SetClipGroup tag;
    tag.playlist = p->id;
    tag.group = group_id;
    for (const Region* r : origin) tag.members.push_back({r->id, 0});
    // A group already tagged needs no second tag, and the model refuses the
    // group's name when the first region is not among the members.
    if (sel->clip_group == 0) members.push_back(tag);
    for (int k = 1; k <= count; ++k) {
        for (const Region* r : origin) {
            Region copy = *r;
            copy.id = doc_.reserve_id();
            copy.position = r->position + span * k;
            copy.clip_group = group_id;
            copy.loop = static_cast<std::uint16_t>(std::min(k, 65535));
            members.push_back(PlaceRegion{p->id, std::move(copy), kNoId});
        }
    }
    // The repeats overwrite what is beneath, as a paste does; none is split,
    // so no split id is spent.
    if (!submit_compound(std::move(members), "Loop Clip", "looped", why)) return false;
    loop_count_ = count;
    select_region(first_id);
    return true;
}

bool EditController::unloop_clip(std::string& why) {
    const TrackEdit* t = nullptr;
    if (selected_region(why) == nullptr) return false;
    const Region* sel = find_region(snapshot(), sel_.region, &t);
    if (sel->clip_group == 0) {
        why = quoted(*sel) + " is not looped";
        return false;
    }
    const Playlist* p = active_playlist(t);
    std::vector<const Region*> origin, repeats;
    for (const Region* m : members_of(*p, sel->clip_group)) (m->loop == 0 ? origin : repeats).push_back(m);
    if (repeats.empty()) {
        why = quoted(*sel) + " is not looped";
        return false;
    }
    std::vector<Member> members;
    for (const Region* r : repeats) members.push_back(RemoveRegion{p->id, r->id});
    // A loop of one clip is no group once it has no repeats.
    if (origin.size() == 1) {
        SetClipGroup untag;
        untag.playlist = p->id;
        untag.group = 0;
        untag.members.push_back({origin.front()->id, 0});
        members.push_back(untag);
    }
    const Id keep = origin.empty() ? kNoId : origin.front()->id;
    if (!submit_compound(std::move(members), "Unloop Clip", "unlooped", why)) return false;
    if (keep != kNoId) select_region(keep);
    return true;
}

bool EditController::group_move_ops(const MoveRegion& op, std::vector<Member>& out) const {
    const TrackEdit* t = nullptr;
    const Region* r = find_region(snapshot(), op.region, &t);
    if (r == nullptr || r->clip_group == 0 || op.timebase != Timebase::Frames) return false;
    const Playlist* p = active_playlist(t);
    std::vector<const Region*> group = members_of(*p, r->clip_group);
    if (group.size() < 2) return false;
    Frames delta = op.position - r->position;
    // The group stops at the session start rather than being refused.
    for (const Region* m : group) delta = std::max(delta, -m->position);
    if (delta == 0) return false;
    // The leading member goes first, so a member never lands on one that has
    // not yet moved away.
    std::sort(group.begin(), group.end(), [delta](const Region* a, const Region* b) {
        return delta > 0 ? a->position > b->position : a->position < b->position;
    });
    for (const Region* m : group) {
        MoveRegion step{op.playlist, m->id, m->position + delta, kNoId, Timebase::Frames};
        // A member that lands inside a clip outside the group splits it.
        for (const Region& o : p->regions) {
            if (o.clip_group != r->clip_group && o.position < step.position &&
                o.end() > step.position + m->length) {
                step.split_right_id = doc_.reserve_id();
            }
        }
        out.push_back(step);
    }
    return true;
}

bool EditController::sample_edit_argument(const std::string& verb, const std::string& param,
                                          std::string& argument, std::string& why) const {
    if (sel_.track < 0 || !sel_.has_range()) {
        why = "select the samples to edit with the Selector, zoomed in to the stretch, first; "
              "nothing was edited";
        return false;
    }
    const TrackEdit* t = track(sel_.track);
    const Playlist* p = t != nullptr ? active_playlist(t) : nullptr;
    const Region* in = nullptr;
    if (p != nullptr) {
        for (const Region& r : p->regions) {
            if (r.position <= sel_.start && sel_.end <= r.end()) in = &r;
        }
    }
    if (in == nullptr) {
        why = "the selection must lie inside one clip; a sample edit changes one clip's audio. "
              "Nothing was edited.";
        return false;
    }
    argument = verb + " " + std::to_string(in->id) + " " + std::to_string(sel_.start) + " " +
               std::to_string(sel_.end) + " all";
    if (!param.empty()) argument += " " + param;
    return true;
}

}  // namespace sonora::ui
