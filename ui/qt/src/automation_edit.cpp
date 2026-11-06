// SPDX-License-Identifier: GPL-3.0-or-later
#include "automation_edit.h"

#include <algorithm>
#include <cmath>

#include "fader_taper.h"

namespace sonora::ui {

using namespace model;

namespace {

// Volume and Volume Trim are both dB on the fader's taper: the trim fader is
// the volume fader in a trim mode (ADR-0027 §8 slice 5), so its lane is drawn
// as the fader shows it. Only their ranges differ (clamp_lane_value).
bool is_volume(AutomationParam p) {
    return p == AutomationParam::Volume || p == AutomationParam::VolumeTrim;
}

// True when `i` is one of a step's two points.
bool has_partner(const std::vector<AutomationPoint>& pts, std::size_t i) {
    return (i > 0 && pts[i - 1].at == pts[i].at) ||
           (i + 1 < pts.size() && pts[i + 1].at == pts[i].at);
}

}  // namespace

AutomationParam lane_param(int lane) {
    switch (lane) {
        case 1: return AutomationParam::Pan;
        case 2: return AutomationParam::VolumeTrim;
        default: return AutomationParam::Volume;
    }
}

const char* lane_name(AutomationParam p) {
    switch (p) {
        case AutomationParam::Volume: return "Volume";
        case AutomationParam::Pan: return "Pan";
        case AutomationParam::Plugin: return "Plugin";
        case AutomationParam::VolumeTrim: return "Volume Trim";
        case AutomationParam::Mute: return "Mute";
        case AutomationParam::SendLevel: return "Send Level";
        case AutomationParam::SendPan: return "Send Pan";
    }
    return "Volume";
}

float lane_position(AutomationParam p, float value) {
    if (is_volume(p)) return fader_position_from_db(value);
    return std::clamp((value + 1.0f) / 2.0f, 0.0f, 1.0f);
}

float lane_value(AutomationParam p, float position) {
    position = std::clamp(position, 0.0f, 1.0f);
    if (is_volume(p)) return clamp_lane_value(p, fader_db_from_position(position));
    return clamp_lane_value(p, position * 2.0f - 1.0f);
}

float clamp_lane_value(AutomationParam p, float value) {
    if (!(value == value)) return 0.0f;  // NaN
    if (p == AutomationParam::VolumeTrim) return std::clamp(value, kMinTrimDb, kMaxTrimDb);
    if (is_volume(p)) return std::clamp(value, kMinGainDb, kMaxGainDb);
    // A plugin value is format-native; its range is the plugin's, which the
    // lane does not carry, so apply() is the one to refuse it.
    if (p == AutomationParam::Plugin) return value;
    return std::clamp(value, -1.0f, 1.0f);
}

std::string lane_value_text(AutomationParam p, float value) {
    return is_volume(p) ? gain_text(value) + " dB" : pan_text(value);
}

namespace {

// lane_value_at on a lane known to have a point.
float value_on(const std::vector<AutomationPoint>& pts, Frames at) {
    const auto next = std::upper_bound(pts.begin(), pts.end(), at,
                                       [](Frames f, const AutomationPoint& p) { return f < p.at; });
    if (next == pts.begin()) return pts.front().value;
    if (next == pts.end()) return pts.back().value;
    const AutomationPoint& a = *(next - 1);
    const AutomationPoint& b = *next;
    if (a.at == at) return a.value;
    const double t = static_cast<double>(at - a.at) / static_cast<double>(b.at - a.at);
    // In double before the subtraction: a plugin lane is not clamped, and
    // two finite floats far enough apart differ by more than a float holds,
    // which drew the whole segment at infinity.
    const double from = a.value;
    return static_cast<float>(from + t * (static_cast<double>(b.value) - from));
}

// Where the line arriving at `at` ends: the first of a step's two points,
// which lane_value_at passes over.
float value_before(const std::vector<AutomationPoint>& pts, Frames at) {
    const auto first = std::lower_bound(pts.begin(), pts.end(), at,
                                        [](const AutomationPoint& p, Frames f) { return p.at < f; });
    if (first != pts.end() && first->at == at) return first->value;
    return value_on(pts, at);
}

// A step whose two values agree is one point.
void push_point(std::vector<AutomationPoint>& out, AutomationPoint p) {
    if (!out.empty() && out.back().at == p.at && out.back().value == p.value) return;
    out.push_back(p);
}

// `pts` with [a, b] replaced by `inside` (sorted, within [a, b]) between a
// point at `a` holding the value arriving there and one at `b` holding the
// value leaving it, so nothing outside [a, b] plays differently.
std::vector<AutomationPoint> overlaid(const std::vector<AutomationPoint>& pts, Frames a, Frames b,
                                      const std::vector<AutomationPoint>& inside) {
    std::vector<AutomationPoint> out;
    for (const AutomationPoint& p : pts) {
        if (p.at < a) out.push_back(p);
    }
    push_point(out, {a, value_before(pts, a)});
    for (const AutomationPoint& p : inside) push_point(out, p);
    push_point(out, {b, value_on(pts, b)});
    for (const AutomationPoint& p : pts) {
        if (p.at > b) out.push_back(p);
    }
    return out;
}

bool flat(const AutomationLane& lane) {
    for (const AutomationPoint& p : lane.points) {
        if (p.value != lane.points.front().value) return false;
    }
    return true;
}

// The lane an edit replaces points of: its param, a plugin lane's insert and
// key, a send lane's send, without which apply() would look for another lane.
void aim(EditAutomation& op, const AutomationLane& lane) {
    op.param = lane.param;
    op.insert = lane.insert;
    op.key = lane.key;
    op.send = lane.send;
}

EditAutomation window_op(Id track, const AutomationLane& lane, Frames lo, Frames hi,
                         const std::vector<AutomationPoint>& pts) {
    EditAutomation op;
    op.track = track;
    aim(op, lane);
    op.from = lo;
    op.to = hi + 1;
    for (const AutomationPoint& p : pts) {
        if (p.at >= lo && p.at <= hi) op.points.push_back(p);
    }
    return op;
}

// `pts` without the points in [lo, hi] that play nothing: those holding
// the value the lane already holds on both sides, inside a flat run. Only
// flat runs, because they are found in one pass with no tolerance (a full
// lane must not cost Thin's quadratic worst case on a gesture), and a
// step's two points always stay.
std::vector<AutomationPoint> without_redundant(const std::vector<AutomationPoint>& pts, Frames lo,
                                               Frames hi) {
    std::vector<AutomationPoint> out;
    out.reserve(pts.size());
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const AutomationPoint& p = pts[i];
        const bool flat_inside = p.at >= lo && p.at <= hi && !out.empty() &&
                                 i + 1 < pts.size() && !has_partner(pts, i) &&
                                 out.back().value == p.value && pts[i + 1].value == p.value;
        if (!flat_inside) out.push_back(p);
    }
    return out;
}

// The edit making `lane` into `after` over [lo, hi], appended to `out`. A
// lane the cap leaves no room in goes to `left_behind` instead: apply()
// would refuse its edit, and with it the region edit beside it.
void follow_lane(Id track, const AutomationLane& lane, Frames lo, Frames hi,
                 std::vector<AutomationPoint> after, std::vector<EditAutomation>& out,
                 std::vector<const AutomationLane*>* left_behind) {
    if (after.size() > kMaxAutomationPoints) after = without_redundant(after, lo, hi);
    if (after.size() > kMaxAutomationPoints) {
        if (left_behind != nullptr) left_behind->push_back(&lane);
        return;
    }
    out.push_back(window_op(track, lane, lo, hi, after));
}

}  // namespace

float lane_value_at(const AutomationLane* lane, Frames at, float fallback) {
    if (lane == nullptr || lane->points.empty()) return fallback;
    return value_on(lane->points, at);
}

int point_near(const AutomationLane* lane, AutomationParam p, Frames at, float value,
               double slop_frames, double slop_position) {
    if (lane == nullptr) return -1;
    int best = -1;
    double best_d = 3.0;
    const double pos = lane_position(p, value);
    for (std::size_t i = 0; i < lane->points.size(); ++i) {
        const AutomationPoint& q = lane->points[i];
        const double dx = std::abs(static_cast<double>(q.at - at)) / slop_frames;
        const double dy = std::abs(lane_position(p, q.value) - pos) / slop_position;
        if (dx > 1.0 || dy > 1.0) continue;
        const double d = dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

EditAutomation add_point_op(Id track, AutomationParam p, Frames at, float value) {
    at = std::clamp<Frames>(at, 0, kMaxInteger - 1);
    EditAutomation op;
    op.track = track;
    op.param = p;
    op.from = at;
    op.to = at + 1;
    op.points = {{at, clamp_lane_value(p, value)}};
    return op;
}

AutomationPoint moved_point(const AutomationLane& lane, std::size_t index, Frames at,
                            float value) {
    const auto& pts = lane.points;
    const Frames was = pts[index].at;
    Frames lo = was, hi = was;
    if (!has_partner(pts, index)) {
        lo = index > 0 ? pts[index - 1].at + 1 : 0;
        hi = index + 1 < pts.size() ? pts[index + 1].at - 1 : kMaxInteger - 1;
    }
    return {std::clamp(at, lo, hi), clamp_lane_value(lane.param, value)};
}

bool move_point_op(Id track, const AutomationLane& lane, std::size_t index, Frames at, float value,
                   EditAutomation& out) {
    const auto& pts = lane.points;
    if (index >= pts.size()) return false;
    const Frames was = pts[index].at;
    const AutomationPoint to = moved_point(lane, index, at, value);
    at = to.at;
    out = EditAutomation{};
    out.track = track;
    aim(out, lane);
    out.from = std::min(was, at);
    out.to = std::max(was, at) + 1;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        if (pts[i].at < out.from || pts[i].at >= out.to) continue;
        out.points.push_back(i == index ? to : pts[i]);
    }
    return true;
}

bool delete_point_op(Id track, const AutomationLane& lane, std::size_t index,
                     EditAutomation& out) {
    const auto& pts = lane.points;
    if (index >= pts.size()) return false;
    out = EditAutomation{};
    out.track = track;
    aim(out, lane);
    out.from = pts[index].at;
    out.to = pts[index].at + 1;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        if (i != index && pts[i].at == pts[index].at) out.points.push_back(pts[i]);
    }
    return true;
}

float thin_tolerance(AutomationParam p) { return is_volume(p) ? 0.25f : 0.005f; }

std::vector<AutomationPoint> thin_points(const std::vector<AutomationPoint>& points,
                                         float tolerance) {
    if (points.size() <= 2) return points;
    std::vector<bool> keep(points.size(), false);
    keep.front() = keep.back() = true;
    // Ramer-Douglas-Peucker on the value axis alone: the lane interpolates
    // linearly in value, so that is the error the engine would play.
    std::vector<std::pair<std::size_t, std::size_t>> todo{{0, points.size() - 1}};
    while (!todo.empty()) {
        const auto [lo, hi] = todo.back();
        todo.pop_back();
        if (hi <= lo + 1) continue;
        const AutomationPoint& a = points[lo];
        const AutomationPoint& b = points[hi];
        double worst = -1;
        std::size_t at = lo;
        for (std::size_t i = lo + 1; i < hi; ++i) {
            const double t = b.at == a.at ? 0.0
                                          : static_cast<double>(points[i].at - a.at) /
                                                static_cast<double>(b.at - a.at);
            // As value_on: a float difference overflows on a wide plugin lane,
            // and an infinite line would keep every point inside it.
            const double from = a.value;
            const double line = from + t * (static_cast<double>(b.value) - from);
            const double err = std::abs(static_cast<double>(points[i].value) - line);
            if (err > worst) {
                worst = err;
                at = i;
            }
        }
        if (worst <= tolerance) continue;
        keep[at] = true;
        todo.push_back({lo, at});
        todo.push_back({at, hi});
    }
    std::vector<AutomationPoint> out;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (keep[i]) out.push_back(points[i]);
    }
    return out;
}

bool stroke_op(Id track, AutomationParam p, const std::vector<AutomationPoint>& samples,
               EditAutomation& out) {
    if (samples.empty()) return false;
    std::vector<AutomationPoint> sorted;
    for (AutomationPoint s : samples) {
        s.at = std::clamp<Frames>(s.at, 0, kMaxInteger - 1);
        s.value = clamp_lane_value(p, s.value);
        const auto at = std::lower_bound(
            sorted.begin(), sorted.end(), s.at,
            [](const AutomationPoint& q, Frames f) { return q.at < f; });
        if (at != sorted.end() && at->at == s.at) *at = s;
        else sorted.insert(at, s);
    }
    out = EditAutomation{};
    out.track = track;
    out.param = p;
    out.from = sorted.front().at;
    out.to = sorted.back().at + 1;
    out.points = thin_points(sorted, thin_tolerance(p));
    return true;
}

bool copy_lane_range(const AutomationLane* lane, Frames a, Frames b, AutomationClip& out) {
    if (lane == nullptr || lane->points.empty() || b <= a) return false;
    AutomationClip clip;
    clip.param = lane->param;
    clip.length = b - a;
    clip.points.push_back({0, lane_value_at(lane, a, 0.0f)});
    for (const AutomationPoint& p : lane->points) {
        if (p.at > a && p.at < b - 1) clip.points.push_back({p.at - a, p.value});
    }
    if (b - 1 > a) clip.points.push_back({b - 1 - a, lane_value_at(lane, b - 1, 0.0f)});
    out = std::move(clip);
    return true;
}

bool clear_range_op(Id track, const AutomationLane& lane, Frames a, Frames b,
                    EditAutomation& out) {
    if (b <= a) return false;
    bool inside = false, outside = false;
    for (const AutomationPoint& p : lane.points) {
        if (p.at >= a && p.at < b) inside = true;
        else outside = true;
    }
    if (!inside) return false;
    out = EditAutomation{};
    out.track = track;
    aim(out, lane);
    out.from = a;
    out.to = b;
    if (outside) {
        out.points.push_back({a, lane_value_at(&lane, a, 0.0f)});
        if (b - 1 > a) out.points.push_back({b - 1, lane_value_at(&lane, b - 1, 0.0f)});
    }
    // What a clear leaves is what a second one would find: no edit, no undo
    // step.
    std::vector<AutomationPoint> had;
    for (const AutomationPoint& p : lane.points) {
        if (p.at >= a && p.at < b) had.push_back(p);
    }
    return had != out.points;
}

bool thin_range_op(Id track, const AutomationLane& lane, Frames a, Frames b,
                   EditAutomation& out) {
    if (b <= a) return false;
    std::vector<AutomationPoint> had;
    for (const AutomationPoint& p : lane.points) {
        if (p.at >= a && p.at < b) had.push_back(p);
    }
    if (had.size() < 3) return false;
    // A step (two points on one frame) is a deliberate jump: thin each run
    // between steps on its own so the jump survives.
    std::vector<AutomationPoint> kept, run;
    const float tol = thin_tolerance(lane.param);
    for (std::size_t i = 0; i < had.size(); ++i) {
        run.push_back(had[i]);
        if (i + 1 == had.size() || had[i + 1].at == had[i].at) {
            for (const AutomationPoint& p : thin_points(run, tol)) kept.push_back(p);
            run.clear();
        }
    }
    if (kept.size() == had.size()) return false;
    out = EditAutomation{};
    out.track = track;
    aim(out, lane);
    out.from = a;
    out.to = b;
    out.points = std::move(kept);
    return true;
}

bool paste_op(Id track, const AutomationClip& clip, Frames at, EditAutomation& out) {
    if (clip.empty() || clip.length <= 0 || at < 0 || at + clip.length > kMaxInteger) return false;
    out = EditAutomation{};
    out.track = track;
    out.param = clip.param;
    out.from = at;
    out.to = at + clip.length;
    for (const AutomationPoint& p : clip.points) out.points.push_back({p.at + at, p.value});
    return true;
}

std::vector<EditAutomation> follow_move_ops(const TrackEdit& track, Frames from, Frames to,
                                            Frames at,
                                            std::vector<const AutomationLane*>* left_behind) {
    std::vector<EditAutomation> out;
    if (to <= from || at == from || at < 0) return out;
    const Frames end = at + (to - from);
    const Frames lo = std::min(from, at), hi = std::max(to, end);
    // The op's range is [lo, hi + 1), which must stay a legal lane range.
    if (hi >= kMaxInteger - 1) return out;
    for (const auto& lane : track.automation) {
        if (lane == nullptr || lane->points.empty() || flat(*lane)) continue;
        const auto& pts = lane->points;
        std::vector<AutomationPoint> carried{{at, value_on(pts, from)}};
        for (const AutomationPoint& p : pts) {
            if (p.at > from && p.at < to) carried.push_back({p.at - from + at, p.value});
        }
        carried.push_back({end, value_before(pts, to)});
        follow_lane(track.id, *lane, lo, hi, overlaid(overlaid(pts, from, to, {}), at, end, carried),
                    out, left_behind);
    }
    return out;
}

std::vector<EditAutomation> follow_clear_ops(const TrackEdit& track, Frames from, Frames to,
                                             std::vector<const AutomationLane*>* left_behind) {
    std::vector<EditAutomation> out;
    if (to <= from || from < 0 || to >= kMaxInteger - 1) return out;
    for (const auto& lane : track.automation) {
        if (lane == nullptr || lane->points.empty() || flat(*lane)) continue;
        const auto& pts = lane->points;
        // No point in [from, to]: the lane already runs straight across.
        const bool inside = std::any_of(pts.begin(), pts.end(), [&](const AutomationPoint& p) {
            return p.at >= from && p.at <= to;
        });
        if (!inside) continue;
        follow_lane(track.id, *lane, from, to, overlaid(pts, from, to, {}), out, left_behind);
    }
    return out;
}

std::vector<EditAutomation> follow_time_ops(const TrackEdit& track, Frames at, Frames delta,
                                            std::vector<const AutomationLane*>* left_behind) {
    std::vector<EditAutomation> out;
    if (delta == 0 || at < 0 || at >= kMaxInteger - 1) return out;
    const Frames end = delta < 0 ? at - delta : at;   // where the surviving tail starts
    const Frames hi = kMaxInteger - 1;
    for (const auto& lane : track.automation) {
        if (lane == nullptr || lane->points.empty()) continue;
        const auto& pts = lane->points;
        // Nothing at or after `at`: the lane is held across the edit as it was.
        if (pts.back().at < at) continue;
        // The value the lane has going into `at` (the first of a step there)
        // and, for a delete, coming out of the removed stretch.
        const auto first = std::find_if(pts.begin(), pts.end(),
                                        [&](const AutomationPoint& p) { return p.at >= at; });
        const float v_in = first->at == at ? first->value : value_on(pts, at);
        std::vector<AutomationPoint> after;
        std::vector<AutomationPoint> moved;
        for (const AutomationPoint& p : pts) {
            if (p.at < end) continue;
            const Frames to = p.at + delta;
            if (to >= kMaxInteger - 1) continue;
            moved.push_back(AutomationPoint{to, p.value});
        }
        if (delta > 0) {
            after.push_back(AutomationPoint{at, v_in});
            // The stretch inserted holds the value it was cut at.
            if (moved.empty() || moved.front().at != at + delta) {
                after.push_back(AutomationPoint{at + delta, v_in});
            }
        } else {
            const float v_out = value_on(pts, end);
            const bool lands = !moved.empty() && moved.front().at == at;
            if (!lands) {
                after.push_back(AutomationPoint{at, v_in});
                if (v_out != v_in) after.push_back(AutomationPoint{at, v_out});
            }
        }
        for (const AutomationPoint& p : moved) after.push_back(p);
        follow_lane(track.id, *lane, at, hi, std::move(after), out, left_behind);
    }
    return out;
}

}  // namespace sonora::ui
