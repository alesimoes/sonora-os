// SPDX-License-Identifier: GPL-3.0-or-later
#include "automation_edit.h"

#include <algorithm>
#include <cmath>

#include "fader_taper.h"

namespace sonora::ui {

using namespace model;

namespace {

bool is_volume(AutomationParam p) { return p == AutomationParam::Volume; }

// True when `i` is one of a step's two points.
bool has_partner(const std::vector<AutomationPoint>& pts, std::size_t i) {
    return (i > 0 && pts[i - 1].at == pts[i].at) ||
           (i + 1 < pts.size() && pts[i + 1].at == pts[i].at);
}

}  // namespace

AutomationParam lane_param(int lane) {
    return lane == 1 ? AutomationParam::Pan : AutomationParam::Volume;
}

const char* lane_name(AutomationParam p) { return is_volume(p) ? "Volume" : "Pan"; }

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
    if (is_volume(p)) return std::clamp(value, kMinGainDb, kMaxGainDb);
    return std::clamp(value, -1.0f, 1.0f);
}

std::string lane_value_text(AutomationParam p, float value) {
    return is_volume(p) ? gain_text(value) + " dB" : pan_text(value);
}

float lane_value_at(const AutomationLane* lane, Frames at, float fallback) {
    if (lane == nullptr || lane->points.empty()) return fallback;
    const auto& pts = lane->points;
    const auto next = std::upper_bound(pts.begin(), pts.end(), at,
                                       [](Frames f, const AutomationPoint& p) { return f < p.at; });
    if (next == pts.begin()) return pts.front().value;
    if (next == pts.end()) return pts.back().value;
    const AutomationPoint& a = *(next - 1);
    const AutomationPoint& b = *next;
    if (a.at == at) return a.value;
    const double t = static_cast<double>(at - a.at) / static_cast<double>(b.at - a.at);
    return static_cast<float>(a.value + t * (b.value - a.value));
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
    out.param = lane.param;
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
    out.param = lane.param;
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
            const double line = a.value + t * (b.value - a.value);
            const double err = std::abs(points[i].value - line);
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
    out.param = lane.param;
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
    out.param = lane.param;
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

}  // namespace sonora::ui
