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

// --- Cut, Copy, Paste and Clear on a lane's range ---

bool EditController::automation_copy(bool cut, std::string& why) {
    const char* verb = cut ? "cut" : "copied";
    const TrackEdit* t = track(sel_.track);
    if (t == nullptr) {
        why = std::string("select a track first; nothing was ") + verb;
        return false;
    }
    const AutomationParam p = lane_param(sel_.autolane);
    if (!sel_.has_range()) {
        why = "select a range of " + lane_label(*t, p) + " first; nothing was " + verb;
        return false;
    }
    AutomationClip clip;
    if (!copy_lane_range(t->lane(p), sel_.start, sel_.end, clip)) {
        why = "there is no automation on " + lane_label(*t, p) + "; nothing was " + verb;
        return false;
    }
    if (cut) {
        EditAutomation op;
        if (!clear_range_op(t->id, *t->lane(p), sel_.start, sel_.end, op)) {
            why = "there is no breakpoint in the selection on " + lane_label(*t, p) +
                  "; nothing was cut";
            return false;
        }
        if (!submit(op, why)) return false;
    }
    auto_clip_ = std::move(clip);
    sel_.point = -1;
    return true;
}

bool EditController::automation_paste(std::string& why) {
    const TrackEdit* t = track(sel_.track);
    if (t == nullptr) {
        why = "select a track first; nothing was pasted";
        return false;
    }
    if (auto_clip_.empty()) {
        why = "the automation clipboard is empty; copy a range of a lane first";
        return false;
    }
    const AutomationParam p = lane_param(sel_.autolane);
    if (auto_clip_.param != p) {
        why = std::string("the clipboard holds ") + lane_name(auto_clip_.param) +
              " automation; select a " + lane_name(auto_clip_.param) +
              " lane to paste it. Nothing was pasted.";
        return false;
    }
    const Frames at = sel_.has_range() ? sel_.start : sel_.insertion;
    EditAutomation op;
    if (!paste_op(t->id, auto_clip_, at, op)) {
        why = "the automation does not fit there; nothing was pasted";
        return false;
    }
    if (!submit(op, why)) return false;
    sel_.start = sel_.insertion = op.from;
    sel_.end = op.to;
    sel_.point = -1;
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
        why = "select a breakpoint or a range of " + lane_label(*t, p) +
              " first; nothing was cleared";
        return false;
    }
    if (lane == nullptr) {
        why = "there is no automation on " + lane_label(*t, p) + "; nothing was cleared";
        return false;
    }
    if (!clear_range_op(t->id, *lane, sel_.start, sel_.end, op)) {
        why = "there is no breakpoint in the selection on " + lane_label(*t, p) +
              "; nothing was cleared";
        return false;
    }
    return submit(op, why);
}

}  // namespace sonora::ui
