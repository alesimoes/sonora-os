// SPDX-License-Identifier: GPL-3.0-or-later
//
// Markers, as Pro Tools' Memory Locations: add, clear, recall, and the
// marker ruler's hit test. Part of EditController (edit_controller.h).
#include <algorithm>
#include <cmath>
#include <set>

#include "edit_controller.h"
#include "edit_controller_detail.h"
#include "marker_view_store.h"
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
    reveal(m->position);
    if (locate_) locate_(m->position);
    apply_marker_view(marker);
    return true;
}

void EditController::apply_marker_view(Id marker) {
    if (marker_views_ == nullptr) return;
    const MarkerView* v = marker_views_->find(snapshot().settings.session_id, marker);
    if (v == nullptr) return;
    if (v->recall_zoom && v->fpp > 0) {
        const double max_fpp = std::max(edit_detail::kMinFpp, snapshot().settings.sample_rate * 60.0);
        view_.fpp = std::clamp(v->fpp, edit_detail::kMinFpp, max_fpp);
        view_.scroll = v->start;
        clamp_scroll();
    }
    if (v->recall_tracks) {
        if (v->track_height > 0) set_track_height(v->track_height);
        if (hide_tracks_) {
            // A track deleted since the view was stored is skipped, not an error.
            std::vector<std::uint64_t> alive;
            for (const auto& t : snapshot().edit->tracks) {
                if (std::find(v->hidden.begin(), v->hidden.end(), t->id) != v->hidden.end()) {
                    alive.push_back(t->id);
                }
            }
            hide_tracks_(alive);
        }
    }
}

bool EditController::marker_has_view(Id marker) const {
    return marker_views_ != nullptr &&
           marker_views_->find(snapshot().settings.session_id, marker) != nullptr;
}

void EditController::prune_marker_views() {
    if (marker_views_ == nullptr) return;
    std::set<std::uint64_t> live;
    for (const Marker& m : markers().markers) live.insert(m.id);
    marker_views_->prune(snapshot().settings.session_id, live);
}

bool EditController::store_marker_view(Id marker, std::string& why) {
    if (find_marker(markers(), marker) == nullptr) {
        why = "that marker is no longer in the session";
        return false;
    }
    if (marker_views_ == nullptr) {
        why = "marker views are not available";
        return false;
    }
    MarkerView v;
    v.fpp = view_.fpp;
    v.start = view_.scroll;
    v.track_height = view_.track_height;
    const ViewState& rows = view();
    const auto& ts = snapshot().edit->tracks;
    for (std::size_t i = 0; i < ts.size(); ++i) {
        if (rows.row_hidden(static_cast<int>(i))) v.hidden.push_back(ts[i]->id);
    }
    marker_views_->store(snapshot().settings.session_id, marker, std::move(v));
    prune_marker_views();
    return true;
}

bool EditController::clear_marker_view(Id marker, std::string& why) {
    if (marker_views_ == nullptr ||
        !marker_views_->clear(snapshot().settings.session_id, marker)) {
        why = "that marker has no stored view";
        return false;
    }
    prune_marker_views();
    return true;
}

bool EditController::set_marker_view_part(Id marker, bool zoom, bool on, std::string& why) {
    const std::string& sid = snapshot().settings.session_id;
    const bool ok = marker_views_ != nullptr &&
                    (zoom ? marker_views_->setRecallZoom(sid, marker, on)
                          : marker_views_->setRecallTracks(sid, marker, on));
    if (!ok) why = "store the view first: this marker has none";
    return ok;
}

void EditController::locate_to(Frames frame) {
    sel_.region = kNoId;
    sel_.insertion = sel_.start = sel_.end = std::max<Frames>(0, frame);
    reveal(sel_.insertion);
    if (locate_) locate_(sel_.insertion);
}

void EditController::reveal(Frames frame) {
    if (view_.width <= 0) return;
    const double x = view_.x_of(frame);
    if (x < 0 || x > view_.width - 16) {
        view_.scroll = static_cast<double>(frame) - view_.width * 0.1 * view_.fpp;
        clamp_scroll();
    }
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
