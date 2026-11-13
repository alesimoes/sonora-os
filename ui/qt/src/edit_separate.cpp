// SPDX-License-Identifier: GPL-3.0-or-later
//
// Separate, Heal Separation, trims to the insertion, Clear, time operations,
// Spot and Nudge.
#include "edit_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "edit_controller_detail.h"

namespace sonora::ui {

using namespace model;
using namespace edit_detail;

bool EditController::separate(std::string& why) {
    const TrackEdit* t = track(sel_.track);
    if (t == nullptr) {
        why = "select a track first";
        return false;
    }
    std::vector<Frames> at;
    if (sel_.has_range()) {
        at = {sel_.end, sel_.start};  // right first, so the left split keeps its region id
    } else {
        at = {sel_.insertion};
    }
    // Both ends of a range, on every selected track, are one separation, so
    // one undo step.
    const int tracks = sel_.region != kNoId ? 1 : span();
    std::vector<Member> members;
    for (int i = 0; i < tracks; ++i) {
        for (Frames f : at) {
            const Region* r = region_at(sel_.track + i, f);
            if (r == nullptr || f <= r->position) continue;
            members.push_back(SplitRegion{track(sel_.track + i)->active_playlist, r->id, f,
                                          doc_.reserve_id()});
        }
    }
    if (members.empty()) {
        why = tracks == 1 ? "no region to separate at the insertion point on " + t->name
                          : "no region to separate at the insertion point on any of the " +
                                std::to_string(tracks) + " selected tracks";
        why += "; click inside a region's upper half to place it";
        return false;
    }
    if (members.size() == 1) {
        if (!submit(to_operation(members.front()), why)) return false;
    } else if (!submit_compound(std::move(members), "Separate", "separated", why)) {
        return false;
    }
    sel_.region = kNoId;
    return true;
}

namespace {

// Why `l` and `r`, which butt, cannot be healed into one region; empty when
// they can. Healing must play exactly what the two parts played, so only a
// region separated and left alone since is joined.
std::string unhealable(const Playlist& p, const Region& l, const Region& r) {
    const std::string pair = l.name + " and " + r.name;
    if (l.channels != r.channels || l.source_offset + l.length != r.source_offset) {
        return pair + " are not one recording separated in two (moved, slipped or from "
                      "different takes); Heal Separation joins only parts that still line up";
    }
    for (const Crossfade& x : p.crossfades) {
        if (x.left == l.id && x.right == r.id) {
            return pair + " are crossfaded; select the crossfade and Clear it first";
        }
    }
    if (l.fade_out.length > 0) return l.name + " ends in a fade out; Clear the fade first";
    if (r.fade_in.length > 0) return r.name + " starts with a fade in; Clear the fade first";
    if (l.gain_db != r.gain_db) return pair + " have different clip gain";
    if (l.mute != r.mute) return "one of " + pair + " is muted and the other is not";
    return {};
}

}  // namespace

bool EditController::heal_separation(std::string& why) {
    int first = sel_.track;
    int tracks = span();
    Frames a = sel_.insertion, b = sel_.insertion;
    if (sel_.region != kNoId) {
        const TrackEdit* t = nullptr;
        const Region* r = find_region(snapshot(), sel_.region, &t);
        if (r == nullptr) {
            why = "the selected region is gone; nothing was healed";
            return false;
        }
        first = track_index(snapshot(), t->id);
        tracks = 1;
        a = r->position;
        b = r->end();
    } else if (sel_.has_range()) {
        a = sel_.start;
        b = sel_.end;
    }
    if (tracks == 0) {
        why = "select a track first";
        return false;
    }
    // Every separation the selection touches, on every selected track, is
    // one heal and one undo step; one that cannot be healed refuses them
    // all, so nothing is half done.
    std::vector<Member> members;
    for (int i = first; i < first + tracks; ++i) {
        const TrackEdit* t = track(i);
        const Playlist* p = active(t);
        if (p == nullptr) continue;
        const std::vector<Region>& rs = p->regions;
        std::size_t k = 0;
        while (k + 1 < rs.size()) {
            const auto joins = [&](std::size_t j) {
                const Frames cut = rs[j].end();
                return j + 1 < rs.size() && rs[j + 1].position == cut && cut >= a && cut <= b;
            };
            if (!joins(k)) {
                ++k;
                continue;
            }
            const std::size_t head = k;
            while (joins(k)) {
                const std::string no = unhealable(*p, rs[k], rs[k + 1]);
                if (!no.empty()) {
                    why = no + " (" + t->name + "); nothing was healed";
                    return false;
                }
                ++k;
            }
            const Region& h = rs[head];
            const Region& tail = rs[k];
            members.push_back(TrimRegion{p->id, h.id, Edge::End, tail.end()});
            // The healed region ends as its last part did: that part's fade
            // out and its crossfade into whatever follows.
            if (tail.fade_out.length > 0) {
                members.push_back(SetFade{p->id, h.id, FadeEnd::Out, tail.fade_out});
            }
            for (const Crossfade& x : p->crossfades) {
                if (x.left != tail.id) continue;
                Crossfade moved = x;
                moved.left = h.id;
                members.push_back(SetCrossfade{p->id, moved});
            }
        }
    }
    if (members.empty()) {
        why = sel_.has_range() || sel_.region != kNoId
                  ? "no separation inside the selection: Heal Separation joins two regions "
                    "that butt, with the cut between them selected"
                  : "no separation at the insertion point: place it on the cut between two "
                    "regions, or select across it";
        return false;
    }
    if (!submit_compound(std::move(members), "Heal Separation", "healed", why)) return false;
    sel_.region = kNoId;
    return true;
}

bool EditController::trim_start_to_insertion(std::string& why) {
    if (refuse_several("Trimming to the insertion point", why)) return false;
    int index = -1;
    const Region* r = target_region(index);
    TrimRegion op;
    if (r == nullptr || !resolve_trim(snapshot(), r->id, Edge::Start, sel_.insertion, op)) {
        why = "the insertion point is not inside a region on the selected track";
        return false;
    }
    return submit_trim(op, why);
}

bool EditController::trim_end_to_insertion(std::string& why) {
    if (refuse_several("Trimming to the insertion point", why)) return false;
    int index = -1;
    const Region* r = target_region(index);
    TrimRegion op;
    if (r == nullptr || !resolve_trim(snapshot(), r->id, Edge::End, sel_.insertion, op)) {
        why = "the insertion point is not inside a region on the selected track";
        return false;
    }
    return submit_trim(op, why);
}

bool EditController::clear(std::string& why) {
    if (autolane_selected()) return automation_clear(why);
    bool fade = false;
    if (!clear_fade_at_selection(fade, why)) return false;
    if (fade) return true;
    const bool shuffle = mode_ == EditMode::Shuffle;
    if (sel_.region != kNoId) {
        const TrackEdit* t = nullptr;
        const Region* r = find_region(snapshot(), sel_.region, &t);
        if (r == nullptr) {
            why = "the selected region is gone";
            return false;
        }
        if (shuffle) {
            // The gap closes: every later region moves up by the region's length.
            std::vector<Member> members{RemoveRegion{t->active_playlist, sel_.region}};
            shuffle_shift(*active(t), r->end(), -r->length, members);
            const int at = track_index(snapshot(), t->id);
            if (!submit_compound(std::move(members), "Clear", "cleared", why)) return false;
            note_shuffle_automation("Clear", at, 1);
            return true;
        }
        std::vector<EditAutomation> follow;
        std::vector<const AutomationLane*> left;
        if (automation_follows_edit_) follow = follow_clear_ops(*t, r->position, r->end(), &left);
        const std::vector<std::string> left_names = lane_labels(*t, left);
        if (!submit_following(RemoveRegion{t->active_playlist, sel_.region}, std::move(follow),
                              "Clear", "cleared", why)) {
            return false;
        }
        note_left_behind("Clear", left_names);
        return true;
    }
    if (!sel_.has_range()) {
        why = "nothing is selected";
        return false;
    }
    Span e;
    if (!edit_range(e, "cleared", why)) return false;
    std::vector<Member> members;
    for (int i = 0; i < e.count; ++i) {
        clear_range(*active(track(e.first + i)), e.a, e.b, [this]() { return doc_.reserve_id(); },
                    members);
        if (shuffle) shuffle_shift(*active(track(e.first + i)), e.b, -(e.b - e.a), members);
    }
    // After the regions' members, which reserve ids in order; a lane edit
    // reserves none.
    std::vector<std::string> left_names;
    if (automation_follows_edit_ && !shuffle) {
        for (int i = 0; i < e.count; ++i) {
            std::vector<const AutomationLane*> left;
            for (EditAutomation& f : follow_clear_ops(*track(e.first + i), e.a, e.b, &left)) {
                members.push_back(std::move(f));
            }
            for (std::string& n : lane_labels(*track(e.first + i), left)) {
                left_names.push_back(std::move(n));
            }
        }
    }
    if (members.empty()) {
        why = e.count == 1 ? "there is no region in the selection on " + track(e.first)->name
                           : "there is no region in the selection on any of its " +
                                 std::to_string(e.count) + " tracks";
        why += "; nothing was cleared";
        return false;
    }
    if (!submit_compound(std::move(members), "Clear", "cleared", why)) return false;
    if (shuffle) {
        note_shuffle_automation("Clear", e.first, e.count);
    } else {
        note_left_behind("Clear", left_names);
    }
    return true;
}

bool EditController::insert_time(std::string& why) { return shift_time(true, why); }
bool EditController::delete_time(std::string& why) { return shift_time(false, why); }

bool EditController::shift_time(bool insert, std::string& why) {
    const char* gesture = insert ? "Insert Time" : "Delete Time";
    const char* done = insert ? "inserted" : "deleted";
    if (autolane_selected()) {
        why = std::string(gesture) + " acts on tracks, not on an automation lane; select a track "
              "range. Nothing was " + done + ".";
        return false;
    }
    Span e;
    if (!edit_range(e, done, why)) return false;
    if (e.b <= e.a) {
        why = std::string("select a time range first; nothing was ") + done;
        return false;
    }
    const Frames length = e.b - e.a;
    const Frames delta = insert ? length : -length;
    std::vector<Member> members;
    std::vector<std::string> left_names;
    const auto next = [this]() { return doc_.reserve_id(); };
    for (int i = 0; i < e.count; ++i) {
        const Playlist& p = *active(track(e.first + i));
        if (insert) {
            shuffle_open(p, e.a, length, next, members);
        } else {
            clear_range(p, e.a, e.b, next, members);
            shuffle_shift(p, e.b, delta, members);
        }
    }
    for (int i = 0; i < e.count; ++i) {
        std::vector<const AutomationLane*> left;
        for (EditAutomation& f : follow_time_ops(*track(e.first + i), e.a, delta, &left)) {
            members.push_back(std::move(f));
        }
        for (std::string& n : lane_labels(*track(e.first + i), left)) {
            left_names.push_back(std::move(n));
        }
    }
    for (const Marker& m : markers().markers) {
        if (m.position < e.a) continue;
        const Frames to = insert ? m.position + delta : std::max(e.a, m.position + delta);
        if (to != m.position) members.push_back(MoveMarker{m.id, to});
    }
    if (members.empty()) {
        why = std::string("there is nothing after the range to move; nothing was ") + done;
        return false;
    }
    if (!submit_compound(std::move(members), gesture, done, why)) return false;
    select_span(e.first, e.count, e.a, insert ? e.b : e.a);
    note_left_behind(gesture, left_names);
    return true;
}

bool EditController::spot_region(Frames to, std::string& why) {
    const Region* r = sel_.region != kNoId ? find_region(snapshot(), sel_.region) : nullptr;
    if (r == nullptr) {
        why = "select a region to spot";
        return false;
    }
    if (to < 0) {
        why = "a region cannot start before the session start";
        return false;
    }
    const Id id = r->id;
    if (to == r->position) return true;
    MoveRegion op;
    if (!resolve_move(snapshot(), id, to, doc_.reserve_id(), op)) {
        why = "the region cannot be placed there";
        return false;
    }
    if (!submit_place(op, why)) return false;
    if (const Region* moved = find_region(snapshot(), id)) {
        sel_.start = moved->position;
        sel_.end = moved->end();
        sel_.insertion = moved->position;
    }
    return true;
}

bool EditController::nudge(int direction, std::string& why) {
    const Region* r = sel_.region != kNoId ? find_region(snapshot(), sel_.region) : nullptr;
    if (r == nullptr) {
        why = "select a region to nudge";
        return false;
    }
    MoveRegion op;
    const Id id = r->id;
    const Frames to = nudge_target(r->position, direction, grid_, snapshot().settings);
    if (to == r->position || !resolve_move(snapshot(), id, to, doc_.reserve_id(), op)) {
        why = "the region is already at the session start";
        return false;
    }
    if (!submit_place(op, why)) return false;
    if (const Region* moved = find_region(snapshot(), id)) {
        sel_.start = moved->position;
        sel_.end = moved->end();
        sel_.insertion = moved->position;
    }
    return true;
}

}  // namespace sonora::ui
