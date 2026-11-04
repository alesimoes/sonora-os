// SPDX-License-Identifier: GPL-3.0-or-later
//
// Take lanes and comping in the Edit window (ADR-0031 builds the edit,
// ADR-0049 the gestures): which tracks show their other playlists as lanes,
// the range dragged across a lane, Copy to Main Playlist and Quick Swipe, a
// new comp playlist, and the request Consolidate sends the engine.
#include <algorithm>
#include <cstdio>

#include "edit_controller.h"
#include "sonora/model/comping.h"

namespace sonora::ui {

using namespace model;

const Playlist* audio_playlist(const TrackEdit& t, Id id) {
    for (const auto& p : t.playlists) {
        if (p->id == id) return p.get();
    }
    return nullptr;
}

std::vector<Id> take_lanes_of(const TrackEdit& t) {
    std::vector<Id> out;
    if (t.kind == TrackKind::Midi) return out;
    for (const auto& p : t.playlists) {
        if (p->id != t.active_playlist && out.size() < kMaxTakeLanes) out.push_back(p->id);
    }
    return out;
}

namespace {

// Quick Swipe's join, 10 ms: long enough to hide a click, short enough to
// stay inside a breath between words.
constexpr double kQuickSwipeCrossfadeSeconds = 0.010;

}  // namespace

std::vector<Id> EditController::take_lane_ids(int index) const {
    const TrackEdit* t = track(index);
    return t == nullptr ? std::vector<Id>{} : take_lanes_of(*t);
}

std::vector<std::string> EditController::take_lane_names(int index) const {
    std::vector<std::string> out;
    const TrackEdit* t = track(index);
    if (t == nullptr) return out;
    for (const Id id : take_lanes_of(*t)) {
        if (const Playlist* p = audio_playlist(*t, id)) out.push_back(p->name);
    }
    return out;
}

bool EditController::comp_lanes_shown(int index) const {
    const TrackEdit* t = track(index);
    return t != nullptr && comp_lanes_open_.count(t->id) != 0 && t->kind != TrackKind::Midi;
}

bool EditController::toggle_comp_lanes(int index, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = "that track does not exist";
        return false;
    }
    if (t->kind == TrackKind::Midi) {
        why = t->name + " is a MIDI track; MIDI comping is not built yet";
        return false;
    }
    if (comp_lanes_open_.erase(t->id) == 0) {
        // Opening onto nothing would show an empty gap the operator has to
        // explain to themselves; say what makes a lane instead.
        if (take_lanes_of(*t).empty()) {
            why = "\"" + t->name +
                  "\" has one playlist, so there are no takes to show. Record a loop take, or "
                  "make a New Playlist and record onto it; the others show here as lanes.";
            return false;
        }
        comp_lanes_open_.insert(t->id);
    } else {
        lane_sel_ = LaneSelection{};
    }
    drag_ = Drag{};
    refresh_rows();
    clamp_scroll();
    return true;
}

int EditController::comp_origin_lane(int index, const Region& r) const {
    const TrackEdit* t = track(index);
    if (t == nullptr) return -1;
    const Id from = comp_origin(*t, r, t->active_playlist);
    if (from == 0) return -1;
    const std::vector<Id> lanes = take_lanes_of(*t);
    const auto it = std::find(lanes.begin(), lanes.end(), from);
    return it == lanes.end() ? -1 : static_cast<int>(it - lanes.begin());
}

EditController::LaneSelection EditController::lane_selection() const {
    if (!lane_sel_.valid()) return {};
    const TrackEdit* t = track(lane_sel_.track);
    if (t == nullptr) return {};
    const std::vector<Id> lanes = take_lanes_of(*t);
    if (std::find(lanes.begin(), lanes.end(), lane_sel_.lane) == lanes.end()) return {};
    return lane_sel_;
}

bool EditController::select_lane_range(int index, Id lane, Frames a, Frames b, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = "that track does not exist";
        return false;
    }
    const std::vector<Id> lanes = take_lanes_of(*t);
    if (std::find(lanes.begin(), lanes.end(), lane) == lanes.end()) {
        why = "\"" + t->name + "\" has no such take lane";
        return false;
    }
    const Frames start = std::max<Frames>(0, std::min(a, b));
    const Frames end = std::max<Frames>(0, std::max(a, b));
    if (end <= start) {
        why = "the range is empty; drag across the lane to select the part to keep";
        return false;
    }
    select_track(index);
    // The selection every other edit acts on is the insertion point at the
    // range's start: a stray Clear cannot reach the comp through the lane.
    sel_.region = kNoId;
    sel_.autolane = sel_.point = -1;
    sel_.insertion = start;
    sel_.start = sel_.end = start;
    lane_sel_ = LaneSelection{index, lane, start, end};
    return true;
}

bool EditController::select_lane_region(int index, Id lane, Id region, std::string& why) {
    const TrackEdit* t = track(index);
    const Playlist* p = t == nullptr ? nullptr : audio_playlist(*t, lane);
    if (p == nullptr) {
        why = "that take lane is gone";
        return false;
    }
    for (const Region& r : p->regions) {
        if (r.id == region) return select_lane_range(index, lane, r.position, r.end(), why);
    }
    why = "that region is not on the lane";
    return false;
}

bool EditController::comp_copy_to_main(std::string& why) {
    const LaneSelection ls = lane_selection();
    if (!ls.valid()) {
        why = "select a range on a take lane first: drag across the lane, or click a region on it. "
              "Nothing was copied.";
        return false;
    }
    const TrackEdit* t = track(ls.track);
    CompRange c;
    c.track = t->id;
    c.from = ls.lane;
    c.to = t->active_playlist;
    c.start = ls.start;
    c.end = ls.end;
    if (quick_swipe_) {
        c.seam_crossfade = static_cast<Frames>(snapshot().settings.sample_rate *
                                               kQuickSwipeCrossfadeSeconds);
    }
    Comp comp;
    if (!comp_range(snapshot(), c, [this]() { return doc_.reserve_id(); }, comp, why)) {
        why += " Nothing was copied.";
        return false;
    }
    if (comp.members.empty()) {
        why = "the lane and the main playlist are both silent there, so there is nothing to copy";
        return false;
    }
    if (!submit_compound(std::move(comp.members), "Copy to Main Playlist", "copied", why)) {
        return false;
    }
    const Frames start = ls.start, end = ls.end;
    lane_sel_ = LaneSelection{};
    select_span(ls.track, 1, start, end);
    // Quick Swipe asked for crossfades; the ones the handles could not take
    // stay cuts, and the operator is told rather than left to find the click.
    if (c.seam_crossfade > 0 && comp.seams_faded < comp.seams) {
        char text[200];
        std::snprintf(text, sizeof text,
                      "%zu of %zu joins kept a hard cut: the takes have no audio to spare beyond "
                      "the swipe for a crossfade there",
                      comp.seams - comp.seams_faded, comp.seams);
        why = text;
    } else {
        why.clear();
    }
    return true;
}

bool EditController::comp_new(int index, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = "that track does not exist";
        return false;
    }
    if (t->kind == TrackKind::Midi) {
        why = t->name + " is a MIDI track; MIDI comping is not built yet";
        return false;
    }
    std::string active;
    if (const Playlist* p = audio_playlist(*t, t->active_playlist)) active = p->name;
    std::string name = active + " comp";
    for (int n = 2; playlist_name_taken(snapshot(), name); ++n) {
        name = active + " comp " + std::to_string(n);
    }
    const Id id = doc_.reserve_id();
    const Id on = t->id;
    std::vector<Member> members;
    members.push_back(AddPlaylist{on, id, name});
    members.push_back(SetActivePlaylist{on, id});
    if (!submit(Compound{std::move(members), "New Comp"}, why)) return false;
    if (sel_.track == index) sel_.region = kNoId;
    lane_sel_ = LaneSelection{};
    comp_lanes_open_.insert(on);
    refresh_rows();
    clamp_scroll();
    return true;
}

bool EditController::consolidate_request(ConsolidateRequest& out, std::string& why) const {
    const TrackEdit* t = track(sel_.track);
    if (t == nullptr) {
        why = "select a track, and a region or a range on it, to consolidate";
        return false;
    }
    if (t->kind == TrackKind::Midi) {
        why = t->name + " is a MIDI track; there is no audio to consolidate";
        return false;
    }
    Frames start = 0, end = 0;
    std::string name;
    if (sel_.region != kNoId) {
        const Region* r = find_region(snapshot(), sel_.region);
        if (r == nullptr) {
            why = "the selected region is gone";
            return false;
        }
        start = r->position;
        end = r->end();
        name = r->name;
    } else if (sel_.has_range()) {
        if (refuse_several("Consolidate", why)) return false;
        start = sel_.start;
        end = sel_.end;
    } else {
        why = "select a region, or drag a range on \"" + t->name + "\", to consolidate. Nothing was "
              "consolidated.";
        return false;
    }
    out.track = t->id;
    out.playlist = t->active_playlist;
    out.start = start;
    out.end = end;
    out.name = name;
    return true;
}

bool EditController::press_takelane(const Hit& h, double x, std::string& why) {
    const std::vector<Id> lanes = take_lane_ids(h.track);
    if (h.lane < 0 || static_cast<std::size_t>(h.lane) >= lanes.size()) {
        why = "that take lane is gone";
        return false;
    }
    drag_.kind = Drag::Kind::Swipe;
    drag_.track = drag_.track_end = h.track;
    drag_.lane = lanes[static_cast<std::size_t>(h.lane)];
    drag_.region = h.region;
    drag_.press_x = x;
    drag_.press_frame = snapped(view_.frame_at(x));
    drag_.start = drag_.end = drag_.press_frame;
    return true;
}

bool EditController::release_swipe(const Drag& d, std::string& why) {
    if (!d.moved) {
        // A click picks the region under it whole, as Logic's click on a take
        // segment does; on empty lane it picks nothing.
        if (d.region == kNoId) return true;
        if (!select_lane_region(d.track, d.lane, d.region, why)) return false;
    } else if (!select_lane_range(d.track, d.lane, d.start, d.end, why)) {
        return false;
    }
    return quick_swipe_ ? comp_copy_to_main(why) : true;
}

}  // namespace sonora::ui
