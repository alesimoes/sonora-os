// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Edit window's automation lanes (ADR-0027): which tracks disclose them,
// the mouse gestures on them and Cut, Copy, Paste and Clear on a lane's
// range. The operations themselves are built in automation_edit.cpp.
#include <algorithm>
#include <cmath>

#include "edit_controller.h"

namespace sonora::ui {

using namespace model;

namespace {

constexpr double kPointHitPx = 7.0;  // how near a press must be to take a breakpoint

std::string lane_label(const TrackEdit& t, AutomationParam p) {
    return t.name + "'s " + lane_name(p) + " lane";
}

}  // namespace

void EditController::refresh_rows() const {
    if (autolanes_open_.empty()) {
        view_.tops.clear();
        view_.open.clear();
        return;
    }
    const auto& ts = doc_.snapshot().edit->tracks;
    view_.tops.assign(ts.size() + 1, 0.0);
    view_.open.assign(ts.size(), 0);
    double y = 0;
    for (std::size_t i = 0; i < ts.size(); ++i) {
        view_.tops[i] = y;
        const bool open = autolanes_open_.count(ts[i]->id) != 0 && ts[i]->kind != TrackKind::Midi;
        view_.open[i] = open ? 1 : 0;
        y += view_.track_height + (open ? kLaneCount * view_.autolane_height : 0.0);
    }
    view_.tops[ts.size()] = y;
}

bool EditController::autolanes_shown(int index) const {
    const TrackEdit* t = track(index);
    return t != nullptr && autolanes_open_.count(t->id) != 0 && t->kind != TrackKind::Midi;
}

bool EditController::toggle_autolanes(int index, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = "that track does not exist";
        return false;
    }
    if (t->kind == TrackKind::Midi) {
        why = t->name + " is a MIDI track; it has no audio automation to show";
        return false;
    }
    if (autolanes_open_.erase(t->id) == 0) {
        autolanes_open_.insert(t->id);
    } else if (sel_.track == index) {
        sel_.autolane = sel_.point = -1;
    }
    drag_ = Drag{};
    refresh_rows();
    clamp_scroll();
    return true;
}

const AutomationLane* EditController::selected_lane() const {
    const TrackEdit* t = track(sel_.track);
    if (t == nullptr || sel_.autolane < 0) return nullptr;
    return t->lane(lane_param(sel_.autolane));
}

// --- the mouse ---

bool EditController::press_autolane(const Hit& h, double x, double y, std::string&) {
    const TrackEdit* t = track(h.track);
    if (t == nullptr) return true;
    const AutomationParam p = lane_param(h.autolane);
    const AutomationLane* lane = t->lane(p);
    const Frames f = std::max<Frames>(0, view_.frame_at(x));
    const float value = lane_value(p, view_.autolane_position(h.track, h.autolane, y));
    drag_.track = drag_.track_end = h.track;
    drag_.autolane = h.autolane;
    drag_.press_x = x;
    drag_.press_y = y;
    drag_.press_frame = f;
    sel_.region = kNoId;

    switch (tool_) {
        case Tool::Select:
            drag_.kind = Drag::Kind::Range;
            set_insertion(f);
            sel_.autolane = h.autolane;
            drag_.press_frame = sel_.insertion;
            drag_.start = drag_.end = sel_.insertion;
            return true;
        case Tool::Pencil:
            drag_.kind = Drag::Kind::Stroke;
            drag_.stroke = {{f, value}};
            sel_.insertion = f;
            sel_.start = sel_.end = f;
            sel_.autolane = h.autolane;
            return true;
        case Tool::Smart:
        case Tool::Grab: {
            drag_.kind = Drag::Kind::Point;
            const double slop_position = kPointHitPx / (view_.autolane_height - 2 * kAutolaneInset);
            drag_.point = point_near(lane, p, f, value, kPointHitPx * view_.fpp, slop_position);
            if (drag_.point >= 0) {
                drag_.at_point = lane->points[static_cast<std::size_t>(drag_.point)];
                sel_.insertion = drag_.at_point.at;
            } else {
                drag_.at_point = {snapped(f), value};
                sel_.insertion = drag_.at_point.at;
            }
            sel_.start = sel_.end = sel_.insertion;
            sel_.autolane = h.autolane;
            sel_.point = drag_.point;
            return true;
        }
        case Tool::Trim:
            drag_ = Drag{};
            return true;
    }
    return true;
}

void EditController::move_autolane(double x, double y) {
    const TrackEdit* t = track(drag_.track);
    if (t == nullptr) return;
    const AutomationParam p = lane_param(drag_.autolane);
    const Frames f = std::max<Frames>(0, view_.frame_at(x));
    const float value = lane_value(p, view_.autolane_position(drag_.track, drag_.autolane, y));
    if (drag_.kind == Drag::Kind::Stroke) {
        // Free: the stroke follows the pointer, not the grid.
        if (drag_.moved && drag_.stroke.size() < kMaxAutomationPoints) {
            drag_.stroke.push_back({f, value});
        }
        return;
    }
    if (drag_.point >= 0) {
        if (!drag_.moved) return;
        const AutomationLane* lane = t->lane(p);
        if (lane == nullptr || static_cast<std::size_t>(drag_.point) >= lane->points.size()) return;
        drag_.at_point = moved_point(*lane, static_cast<std::size_t>(drag_.point), snapped(f), value);
        return;
    }
    drag_.at_point = {snapped(f), value};
}

bool EditController::release_autolane(const Drag& d, std::string& why) {
    const TrackEdit* t = track(d.track);
    if (t == nullptr) return true;
    const Id id = t->id;
    const AutomationParam p = lane_param(d.autolane);
    sel_.track = d.track;
    sel_.tracks = 1;
    sel_.autolane = d.autolane;

    EditAutomation op;
    AutomationPoint left = d.at_point;
    if (d.kind == Drag::Kind::Stroke) {
        if (!stroke_op(id, p, d.stroke, op)) return true;
        if (!submit(op, why)) return false;
        sel_.insertion = sel_.start = op.from;
        sel_.end = op.to - op.from > 1 ? op.to : op.from;
        sel_.point = -1;
        return true;
    }
    const AutomationLane* lane = t->lane(p);
    if (d.point >= 0) {
        sel_.point = d.point;
        if (!d.moved) return true;
        if (lane == nullptr || static_cast<std::size_t>(d.point) >= lane->points.size()) return true;
        const std::size_t i = static_cast<std::size_t>(d.point);
        left = moved_point(*lane, i, d.at_point.at, d.at_point.value);
        if (left == lane->points[i]) return true;
        if (!move_point_op(id, *lane, i, left.at, left.value, op)) return true;
    } else {
        op = add_point_op(id, p, d.at_point.at, d.at_point.value);
        left = op.points.front();
    }
    if (!submit(op, why)) return false;
    sel_.point = -1;
    if (const TrackEdit* now = track(d.track)) {
        if (const AutomationLane* l = now->lane(p)) {
            for (std::size_t i = 0; i < l->points.size(); ++i) {
                if (l->points[i] == left) {
                    sel_.point = static_cast<int>(i);
                    break;
                }
            }
        }
    }
    sel_.insertion = sel_.start = sel_.end = left.at;
    return true;
}

// --- Cut, Copy, Paste, Clear and Thin on a lane's range ---
//
// A range dragged across tracks covers the same lane (the same parameter)
// on each of them, as Pro Tools' Edit selection covers each track's shown
// view. A track in the range whose lanes are hidden shows no curve, so no
// edit touches it: what the operator did not see is not what changes.

namespace {

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out += i + 1 == names.size() ? " and " : ", ";
        out += names[i];
    }
    return out;
}

}  // namespace

std::vector<int> EditController::lane_range_tracks(std::vector<std::string>& hidden) const {
    std::vector<int> out;
    const int n = span();
    for (int i = sel_.track; i < sel_.track + n; ++i) {
        if (autolanes_shown(i)) out.push_back(i);
        else if (const TrackEdit* t = track(i)) hidden.push_back(t->name);
    }
    return out;
}

std::string EditController::lane_range_label() const {
    const AutomationParam p = lane_param(sel_.autolane);
    const TrackEdit* t = track(sel_.track);
    if (span() <= 1 && t != nullptr) return lane_label(*t, p);
    return std::string("the selected ") + lane_name(p) + " lanes";
}

bool EditController::submit_lane_ops(std::vector<EditAutomation> ops, const char* gesture,
                                     const char* nothing_was, std::string& why) {
    if (ops.size() == 1) return submit(ops.front(), why);
    std::vector<Member> members;
    members.reserve(ops.size());
    for (EditAutomation& op : ops) members.emplace_back(std::move(op));
    return submit_compound(std::move(members), gesture, nothing_was, why);
}

void EditController::note_lanes_hidden(const char* gesture,
                                       const std::vector<std::string>& tracks) {
    if (tracks.empty()) return;
    const bool one = tracks.size() == 1;
    follow_notice_ = joined(tracks) + (one ? " shows" : " show") +
                     " no automation lanes, so " + gesture + " left " + (one ? "it" : "them") +
                     " alone; show its lanes to include it";
}

bool EditController::automation_copy(bool cut, std::string& why) {
    const char* verb = cut ? "cut" : "copied";
    const TrackEdit* t = track(sel_.track);
    if (t == nullptr) {
        why = std::string("select a track first; nothing was ") + verb;
        return false;
    }
    const AutomationParam p = lane_param(sel_.autolane);
    if (!sel_.has_range()) {
        why = "select a range of " + lane_range_label() + " first; nothing was " + verb;
        return false;
    }
    std::vector<std::string> hidden;
    const std::vector<int> rows = lane_range_tracks(hidden);
    // One slot a covered track, so each clip keeps its place below the first.
    std::vector<AutomationClip> clips(static_cast<std::size_t>(span()));
    std::vector<EditAutomation> ops;
    bool any = false;
    for (int i : rows) {
        const TrackEdit* ti = track(i);
        AutomationClip& clip = clips[static_cast<std::size_t>(i - sel_.track)];
        if (!copy_lane_range(ti->lane(p), sel_.start, sel_.end, clip)) continue;
        any = true;
        EditAutomation op;
        if (cut && clear_range_op(ti->id, *ti->lane(p), sel_.start, sel_.end, op)) {
            ops.push_back(std::move(op));
        }
    }
    if (!any) {
        why = "there is no automation on " + lane_range_label() + "; nothing was " + verb;
        return false;
    }
    if (cut) {
        if (ops.empty()) {
            why = "there is no breakpoint in the selection on " + lane_range_label() +
                  "; nothing was cut";
            return false;
        }
        if (!submit_lane_ops(std::move(ops), "Cut Automation", "cut", why)) return false;
    }
    for (AutomationClip& c : clips) {
        // An empty slot still says what it would have held, for Paste's check.
        c.param = p;
        c.length = sel_.end - sel_.start;
    }
    auto_clips_ = std::move(clips);
    sel_.point = -1;
    note_lanes_hidden(cut ? "Cut" : "Copy", hidden);
    return true;
}

bool EditController::automation_paste(std::string& why) {
    const TrackEdit* t = track(sel_.track);
    if (t == nullptr) {
        why = "select a track first; nothing was pasted";
        return false;
    }
    const bool held = std::any_of(auto_clips_.begin(), auto_clips_.end(),
                                  [](const AutomationClip& c) { return !c.empty(); });
    if (!held) {
        why = "the automation clipboard is empty; copy a range of a lane first";
        return false;
    }
    const AutomationParam p = lane_param(sel_.autolane);
    const AutomationParam had = auto_clips_.front().param;
    if (had != p) {
        why = std::string("the clipboard holds ") + lane_name(had) + " automation; select a " +
              lane_name(had) + " lane to paste it. Nothing was pasted.";
        return false;
    }
    // Pro Tools' order: the first clip on the selected track, each next one
    // on the track below.
    const Frames at = sel_.has_range() ? sel_.start : sel_.insertion;
    const int n = static_cast<int>(snapshot().edit->tracks.size());
    std::vector<EditAutomation> ops;
    std::vector<std::string> hidden;
    std::size_t beyond = 0;
    for (std::size_t k = 0; k < auto_clips_.size(); ++k) {
        if (auto_clips_[k].empty()) continue;
        const int i = sel_.track + static_cast<int>(k);
        if (i >= n) {
            ++beyond;
            continue;
        }
        if (!autolanes_shown(i)) {
            hidden.push_back(track(i)->name);
            continue;
        }
        EditAutomation op;
        if (!paste_op(track(i)->id, auto_clips_[k], at, op)) {
            why = "the automation does not fit there; nothing was pasted";
            return false;
        }
        ops.push_back(std::move(op));
    }
    if (ops.empty()) {
        why = "no track below shows its " + std::string(lane_name(p)) +
              " lane to paste onto; nothing was pasted";
        return false;
    }
    const Frames length = auto_clips_.front().length;
    if (!submit_lane_ops(std::move(ops), "Paste Automation", "pasted", why)) return false;
    sel_.start = sel_.insertion = at;
    sel_.end = at + length;
    sel_.tracks = std::clamp(static_cast<int>(auto_clips_.size()), 1, n - sel_.track);
    sel_.point = -1;
    follow_notice_.clear();
    note_lanes_hidden("Paste", hidden);
    if (beyond > 0) {
        const bool one = beyond == 1;
        follow_notice_ += (follow_notice_.empty() ? "" : ". ") + std::to_string(beyond) +
                          (one ? " lane" : " lanes") + " of the clipboard had no track to land on and " +
                          (one ? "was" : "were") + " not pasted";
    }
    return true;
}

bool EditController::automation_clear(std::string& why) {
    const TrackEdit* t = track(sel_.track);
    if (t == nullptr) {
        why = "select a track first; nothing was cleared";
        return false;
    }
    const AutomationParam p = lane_param(sel_.autolane);
    const AutomationLane* lane = t->lane(p);
    EditAutomation op;
    if (sel_.point >= 0 && lane != nullptr &&
        static_cast<std::size_t>(sel_.point) < lane->points.size()) {
        delete_point_op(t->id, *lane, static_cast<std::size_t>(sel_.point), op);
        if (!submit(op, why)) return false;
        sel_.point = -1;
        return true;
    }
    if (!sel_.has_range()) {
        why = "select a breakpoint or a range of " + lane_range_label() +
              " first; nothing was cleared";
        return false;
    }
    std::vector<std::string> hidden;
    std::vector<EditAutomation> ops;
    bool any_lane = false;
    for (int i : lane_range_tracks(hidden)) {
        const TrackEdit* ti = track(i);
        const AutomationLane* l = ti->lane(p);
        if (l == nullptr) continue;
        any_lane = true;
        if (clear_range_op(ti->id, *l, sel_.start, sel_.end, op)) ops.push_back(std::move(op));
    }
    if (!any_lane) {
        why = "there is no automation on " + lane_range_label() + "; nothing was cleared";
        return false;
    }
    if (ops.empty()) {
        why = "there is no breakpoint in the selection on " + lane_range_label() +
              "; nothing was cleared";
        return false;
    }
    if (!submit_lane_ops(std::move(ops), "Clear Automation", "cleared", why)) return false;
    note_lanes_hidden("Clear", hidden);
    return true;
}

bool EditController::thin_automation(std::string& why) {
    const TrackEdit* t = track(sel_.track);
    if (!autolane_selected() || t == nullptr || !sel_.has_range()) {
        why = "select a range of an automation lane first; nothing was thinned";
        return false;
    }
    const AutomationParam p = lane_param(sel_.autolane);
    std::vector<std::string> hidden;
    std::vector<EditAutomation> ops;
    for (int i : lane_range_tracks(hidden)) {
        const TrackEdit* ti = track(i);
        const AutomationLane* l = ti->lane(p);
        EditAutomation op;
        if (l != nullptr && thin_range_op(ti->id, *l, sel_.start, sel_.end, op)) {
            ops.push_back(std::move(op));
        }
    }
    if (ops.empty()) {
        why = "there is nothing to thin in the selection on " + lane_range_label() +
              "; nothing was thinned";
        return false;
    }
    if (!submit_lane_ops(std::move(ops), "Thin Automation", "thinned", why)) return false;
    note_lanes_hidden("Thin", hidden);
    return true;
}

}  // namespace sonora::ui
