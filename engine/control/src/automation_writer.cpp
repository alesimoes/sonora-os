// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/automation_writer.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>

namespace sonora {

namespace {

using model::AutomationLane;
using model::AutomationParam;
using model::AutomationPoint;
using model::Frames;

// A span of the pass: a value held, or a release's return, a line from
// `value` at `origin` to `to` at `origin + len`. A return cut by a later
// write keeps its line.
struct Painted {
    Frames end = 0;
    bool ret = false;
    float value = 0.0f;
    float to = 0.0f;
    Frames origin = 0;
    Frames len = 1;
    float at(Frames t) const {
        if (!ret) return value;
        const double x = static_cast<double>(t - origin) / static_cast<double>(len);
        return value + static_cast<float>(x * static_cast<double>(to - value));
    }
};
using Paint = std::map<Frames, Painted>;

// [a, p.end) takes p, over whatever was painted there.
void paint(Paint& m, Frames a, const Painted& p) {
    const Frames b = p.end;
    auto it = m.lower_bound(a);
    if (it != m.begin()) {
        const auto before = std::prev(it);
        if (before->second.end > a) {
            const Painted tail = before->second;
            before->second.end = a;
            if (tail.end > b) m.emplace(b, tail);
        }
    }
    it = m.lower_bound(a);
    while (it != m.end() && it->first < b) {
        if (it->second.end > b) {
            const Painted tail = it->second;
            m.erase(it);
            m.emplace(b, tail);
            break;
        }
        it = m.erase(it);
    }
    m[a] = p;
}

// The lane's value at t as the engine plays it (playback_plan.h's
// auto_lane_value): linear, held at the ends, a step's second from its frame.
float lane_value(const std::vector<AutomationPoint>& p, Frames t) {
    if (t < p.front().at) return p.front().value;
    if (t >= p.back().at) return p.back().value;
    const auto hi = std::upper_bound(p.begin(), p.end(), t,
                                     [](Frames f, const AutomationPoint& q) { return f < q.at; });
    const AutomationPoint& b = *hi;
    const AutomationPoint& a = *std::prev(hi);
    const double x = static_cast<double>(t - a.at) / static_cast<double>(b.at - a.at);
    return a.value + static_cast<float>(x * static_cast<double>(b.value - a.value));
}

struct Original {
    const AutomationLane* lane = nullptr;
    float fixed = 0.0f;
    float at(Frames t) const { return lane ? lane_value(lane->points, t) : fixed; }
};

void put(std::vector<AutomationPoint>& out, Frames at, float value) {
    // A step to the same value is no step.
    if (!out.empty() && out.back().at == at && out.back().value == value) return;
    out.push_back({at, value});
}

struct LaneWrites {
    model::Id track = 0;
    AutomationParam param = AutomationParam::Volume;
    Paint paint;
};

// About what a fader or a pan knob can be set to by hand.
float tolerance_of(AutomationParam p) { return p == AutomationParam::Volume ? 0.1f : 0.005f; }

}  // namespace

void thin_automation(std::vector<AutomationPoint>& points, float tolerance) {
    const std::size_t n = points.size();
    if (n <= 2) return;
    // Bounds the work per kept point on a long, smooth ride.
    constexpr std::size_t kMaxRun = 512;
    std::vector<AutomationPoint> kept;
    kept.reserve(n);
    kept.push_back(points[0]);
    std::size_t anchor = 0;
    for (std::size_t i = 1; i + 1 < n; ++i) {
        const bool step = points[i].at == points[i - 1].at || points[i].at == points[i + 1].at;
        bool keep = step || i - anchor >= kMaxRun;
        const AutomationPoint& a = points[anchor];
        const AutomationPoint& b = points[i + 1];
        for (std::size_t j = anchor + 1; !keep && j <= i; ++j) {
            const double x = static_cast<double>(points[j].at - a.at) /
                             static_cast<double>(b.at - a.at);
            const float line = a.value + static_cast<float>(x * static_cast<double>(b.value - a.value));
            keep = std::fabs(points[j].value - line) > tolerance;
        }
        if (keep) {
            kept.push_back(points[i]);
            anchor = i;
        }
    }
    kept.push_back(points[n - 1]);
    points.swap(kept);
}

std::vector<model::EditAutomation> automation_pass_edits(const model::Snapshot& doc,
                                                         const std::vector<PassWrite>& writes,
                                                         Frames return_frames) {
    std::vector<LaneWrites> lanes;
    const auto original_of = [&](model::Id id, AutomationParam param, bool& found) {
        Original o;
        found = false;
        for (const auto& t : doc.edit->tracks) {
            if (t->id != id) continue;
            if (t->kind == model::TrackKind::Midi) return o;
            found = true;
            o.lane = t->lane(param);
            const model::TrackMix* mix = doc.mix.find(id);
            if (mix) o.fixed = param == AutomationParam::Volume ? mix->gain_db : mix->pan;
            return o;
        }
        return o;
    };
    for (const PassWrite& w : writes) {
        LaneWrites* lane = nullptr;
        for (LaneWrites& l : lanes) {
            if (l.track == w.track && l.param == w.param) lane = &l;
        }
        if (lane == nullptr) {
            lanes.push_back({w.track, w.param, {}});
            lane = &lanes.back();
        }
        Painted p;
        p.value = w.value;
        if (w.frames > 0) {
            p.end = w.frame + static_cast<Frames>(w.frames);
        } else {
            if (return_frames <= 0) continue;
            bool found = false;
            const Original o = original_of(w.track, w.param, found);
            p.ret = true;
            p.end = w.frame + return_frames;
            p.origin = w.frame;
            p.len = return_frames;
            p.to = o.at(p.end);
        }
        paint(lane->paint, w.frame, p);
    }

    std::vector<model::EditAutomation> edits;
    for (const LaneWrites& l : lanes) {
        bool found = false;
        const Original o = original_of(l.track, l.param, found);
        if (!found || l.paint.empty()) continue;
        model::EditAutomation e;
        e.track = l.track;
        e.param = l.param;
        e.from = l.paint.begin()->first;
        std::vector<AutomationPoint> run;
        Frames run_end = 0;
        float last = 0.0f;
        const auto close_run = [&]() {
            put(run, run_end, last);
            put(run, run_end, o.at(run_end));
            thin_automation(run, tolerance_of(l.param));
            e.points.insert(e.points.end(), run.begin(), run.end());
            run.clear();
        };
        bool first = true;
        for (const auto& [start, p] : l.paint) {
            if (!first && start != run_end) {
                close_run();
                // The gap keeps the points it had.
                if (o.lane) {
                    for (const AutomationPoint& q : o.lane->points) {
                        if (q.at > run_end && q.at < start) e.points.push_back(q);
                    }
                }
            }
            if (first || start != run_end) put(run, start, o.at(start));
            first = false;
            put(run, start, p.at(start));
            run_end = p.end;
            last = p.at(p.end);
        }
        close_run();
        e.to = run_end + 1;
        edits.push_back(std::move(e));
    }
    return edits;
}

}  // namespace sonora
