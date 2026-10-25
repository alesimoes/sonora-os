// SPDX-License-Identifier: GPL-3.0-or-later
//
// Markers, as Pro Tools' Memory Locations: add, clear, recall, and the
// marker ruler's hit test. Part of EditController (edit_controller.h).
#include <algorithm>
#include <cmath>
#include <set>

#include "edit_controller.h"
#include "theme.h"

namespace sonora::ui {

using namespace model;

namespace {

std::string trimmed(const std::string& s) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    std::size_t a = 0, b = s.size();
    while (a < b && space(s[a])) ++a;
    while (b > a && space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

const Marker* find_marker(const MarkerList& list, Id id) {
    for (const Marker& m : list.markers) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

}  // namespace

EditController::MarkerPlace EditController::marker_place(bool rolling, Frames playhead) const {
    if (rolling) return {std::max<Frames>(0, playhead), 0};
    if (sel_.has_range()) return {sel_.start, sel_.end - sel_.start};
    return {sel_.insertion, 0};
}

std::string EditController::next_marker_name() const {
    std::set<std::string> taken;
    for (const Marker& m : markers().markers) taken.insert(m.name);
    for (std::size_t n = 1;; ++n) {
        std::string name = "Marker " + std::to_string(n);
        if (taken.count(name) == 0) return name;
    }
}

bool EditController::add_marker(const std::string& name, MarkerPlace at, std::string& why) {
    std::string n = trimmed(name);
    if (n.empty()) n = next_marker_name();
    AddMarker op;
    op.marker.id = doc_.reserve_id();
    op.marker.position = at.position;
    op.marker.length = at.length;
    op.marker.name = std::move(n);
    return submit(op, why);
}

bool EditController::remove_marker(Id marker, std::string& why) {
    if (find_marker(markers(), marker) == nullptr) {
        why = "that marker is no longer in the session";
        return false;
    }
    return submit(RemoveMarker{marker}, why);
}

bool EditController::recall_marker(Id marker, std::string& why) {
    const Marker* m = find_marker(markers(), marker);
    if (m == nullptr) {
        why = "that marker is no longer in the session";
        return false;
    }
    if (sel_.track < 0 && !snapshot().edit->tracks.empty()) {
        sel_.track = 0;
        sel_.tracks = 1;
    }
    sel_.region = kNoId;
    sel_.insertion = sel_.start = m->position;
    sel_.end = m->position + m->length;
    if (view_.width > 0) {
        const double x = view_.x_of(m->position);
        if (x < 0 || x > view_.width - 16) {
            // As follow() pages: the marker a tenth in from the left edge.
            view_.scroll = static_cast<double>(m->position) - view_.width * 0.1 * view_.fpp;
            clamp_scroll();
        }
    }
    return true;
}

Id EditController::marker_at(double x) const {
    Id best = kNoId;
    double best_d = 0;
    for (const Marker& m : markers().markers) {
        const double mx = view_.x_of(m.position);
        if (x < mx - theme::kMarkerHitSlop || x >= mx + theme::kMarkerFlagWidth + theme::kMarkerHitSlop) {
            continue;
        }
        const double d = std::abs(x - mx);
        if (best == kNoId || d < best_d) {
            best = m.id;
            best_d = d;
        }
    }
    if (best != kNoId) return best;
    // Over no flag: the shortest selection marker whose bar is under x.
    Frames shortest = 0;
    for (const Marker& m : markers().markers) {
        if (m.length <= 0) continue;
        if (x < view_.x_of(m.position) || x >= view_.x_of(m.position + m.length)) continue;
        if (best == kNoId || m.length < shortest) {
            best = m.id;
            shortest = m.length;
        }
    }
    return best;
}

}  // namespace sonora::ui
