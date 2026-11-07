// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "edit_controller_detail.h"
#include "fader_taper.h"
#include "sonora/fade_curve.h"
#include "theme.h"

namespace sonora::ui {

using namespace model;

using namespace edit_detail;

const char* edit_mode_name(EditMode m) {
    switch (m) {
        case EditMode::Shuffle: return "Shuffle";
        case EditMode::Slip: return "Slip";
        case EditMode::Spot: return "Spot";
        case EditMode::Grid: return "Grid";
    }
    return "";
}

const char* tool_name(Tool t) {
    switch (t) {
        case Tool::Smart: return "Smart";
        case Tool::Trim: return "Trim";
        case Tool::Select: return "Select";
        case Tool::Grab: return "Grab";
        case Tool::Pencil: return "Pencil";
    }
    return "";
}

Frames ViewState::frame_at(double x) const {
    return static_cast<Frames>(std::llround(scroll + x * fpp));
}

double ViewState::y_of_track(int index) const {
    if (tops.empty() || index < 0 || static_cast<std::size_t>(index) >= tops.size()) {
        return index * track_height - scroll_y;
    }
    return tops[static_cast<std::size_t>(index)] - scroll_y;
}

int ViewState::track_at(double y) const {
    if (track_height <= 0) return -1;
    const double content = y + scroll_y;
    if (tops.size() < 2) return static_cast<int>(std::floor(content / track_height));
    if (content < 0) return -1;
    if (content >= tops.back()) {
        return static_cast<int>(tops.size() - 1) +
               static_cast<int>(std::floor((content - tops.back()) / track_height));
    }
    const auto it = std::upper_bound(tops.begin(), tops.end(), content);
    return static_cast<int>(it - tops.begin()) - 1;
}

int ViewState::autolane_at(int index, double y) const {
    if (!autolanes_open(index)) return -1;
    const double rel = y - y_of_track(index) - track_height;
    if (rel < 0 || rel >= kLaneCount * autolane_height) return -1;
    return std::min(kLaneCount - 1, static_cast<int>(rel / autolane_height));
}

double ViewState::content_height(std::size_t tracks) const {
    if (tops.size() == tracks + 1) return tops.back();
    return static_cast<double>(tracks) * track_height;
}

// --- fades -----------------------------------------------------------------

static_assert(static_cast<uint8_t>(FadeShape::Linear) ==
                      static_cast<uint8_t>(sonora::FadeCurve::Linear) &&
                  static_cast<uint8_t>(FadeShape::EqualPower) ==
                      static_cast<uint8_t>(sonora::FadeCurve::EqualPower) &&
                  static_cast<uint8_t>(FadeShape::SCurve) ==
                      static_cast<uint8_t>(sonora::FadeCurve::SCurve),
              "model::FadeShape and sonora::FadeCurve must share values");

float fade_gain(FadeShape shape, float x) {
    return sonora::fade_in_gain(static_cast<sonora::FadeCurve>(shape), x);
}

const char* fade_shape_name(FadeShape shape) {
    switch (shape) {
        case FadeShape::Linear: return "Linear";
        case FadeShape::EqualPower: return "Equal power";
        case FadeShape::SCurve: return "S-curve";
    }
    return "";
}

FadeShape next_fade_shape(FadeShape shape) {
    switch (shape) {
        case FadeShape::Linear: return FadeShape::EqualPower;
        case FadeShape::EqualPower: return FadeShape::SCurve;
        case FadeShape::SCurve: break;
    }
    return FadeShape::Linear;
}

RegionFades region_fades(const Playlist& p, const Region& r) {
    RegionFades f;
    f.in = r.fade_in.length;
    f.in_shape = r.fade_in.shape;
    f.out = r.fade_out.length;
    f.out_shape = r.fade_out.shape;
    for (const Crossfade& x : p.crossfades) {
        if (x.right == r.id) {
            f.in_crossfade = &x;
            f.in = 0;
        }
        if (x.left == r.id) {
            f.out_crossfade = &x;
            f.out = 0;
        }
    }
    return f;
}

// --- resolvers -------------------------------------------------------------

bool resolve_move(const Snapshot& s, Id region, Frames to, Id spare_id, MoveRegion& out) {
    const TrackEdit* t = nullptr;
    const Region* r = find_region(s, region, &t);
    if (r == nullptr) return false;
    to = std::max<Frames>(0, to);
    if (to == r->position) return false;
    const Frames end = to + r->length;
    out = MoveRegion{t->active_playlist, region, to, kNoId};
    for (const Region& o : active(t)->regions) {
        if (o.id != region && o.position < to && o.end() > end) out.split_right_id = spare_id;
    }
    return true;
}

bool resolve_trim(const Snapshot& s, Id region, Edge edge, Frames to, TrimRegion& out) {
    const TrackEdit* t = nullptr;
    const Region* r = find_region(s, region, &t);
    if (r == nullptr) return false;
    Frames room = INT64_MAX;
    for (const auto& c : r->channels) {
        const Source* src = s.sources->find(c.source);
        room = std::min(room, src ? src->length : 0);
    }
    if (room == INT64_MAX) room = 0;
    // Only the timeline start and the source's own material bound a trim. A
    // neighbour does not: trimming into it overwrites it (timeline-model.md,
    // "Overwrite"), as a Slip-mode trim does in Pro Tools.
    Frames lo, hi;
    if (edge == Edge::Start) {
        lo = std::max<Frames>(0, r->position - r->source_offset);
        hi = r->end() - 1;
    } else {
        lo = r->position + 1;
        hi = r->position + room - r->source_offset;
    }
    if (hi < lo) return false;
    to = std::clamp(to, lo, hi);
    if (to == (edge == Edge::Start ? r->position : r->end())) return false;
    out = TrimRegion{t->active_playlist, region, edge, to};
    return true;
}

// --- controller ------------------------------------------------------------

EditController::EditController(TimelineDocument& doc) : doc_(doc) {
    view_.fpp = snapshot().settings.sample_rate / 40.0;
    view_.track_height = theme::kTrackHeight;
    if (view_.fpp < kMinFpp) view_.fpp = kMinFpp;
    if (!snapshot().edit->tracks.empty()) sel_.track = 0;
}

void EditController::document_replaced() {
    drag_ = Drag{};
    follow_notice_.clear();
    tempo_undo_.clear();
    tempo_redo_.clear();
    sel_ = Selection{};
    autolanes_open_.clear();
    comp_lanes_open_.clear();
    lane_sel_ = LaneSelection{};
    if (!snapshot().edit->tracks.empty()) sel_.track = 0;
    has_saved_view_ = false;
    view_.scroll = 0;
    view_.scroll_y = 0;
    clamp_scroll();
}

const TrackEdit* EditController::track(int index) const {
    const auto& ts = snapshot().edit->tracks;
    if (index < 0 || index >= static_cast<int>(ts.size())) return nullptr;
    return ts[static_cast<std::size_t>(index)].get();
}

Frames EditController::session_end() const {
    Frames end = 0;
    for (const auto& t : snapshot().edit->tracks) {
        if (const Playlist* p = active(t.get())) {
            if (!p->regions.empty()) end = std::max(end, p->regions.back().end());
        }
    }
    return end > 0 ? end : static_cast<Frames>(snapshot().settings.sample_rate) * 60;
}

const Region* EditController::region_at(int index, Frames frame) const {
    const Playlist* p = active(track(index));
    if (p == nullptr) return nullptr;
    for (const Region& r : p->regions) {
        if (frame >= r.position && frame < r.end()) return &r;
    }
    return nullptr;
}

// --- view ---

void EditController::set_viewport(double width, double height) {
    view_.width = std::max(0.0, width);
    view_.height = std::max(0.0, height);
    clamp_scroll();
}

void EditController::set_track_height(double h) {
    view_.track_height = std::clamp(h, 24.0, 400.0);
    clamp_scroll();
}

double EditController::max_scroll_y() const {
    refresh_rows();
    const double lanes = view_.content_height(snapshot().edit->tracks.size());
    return std::max(0.0, lanes - view_.height);
}

void EditController::clamp_scroll() {
    view_.scroll = std::max(0.0, view_.scroll);
    view_.scroll_y = std::clamp(view_.scroll_y, 0.0, max_scroll_y());
}

void EditController::zoom_by(double factor, double anchor_x) {
    if (factor <= 0) return;
    const double max_fpp = std::max(kMinFpp, snapshot().settings.sample_rate * 60.0);
    const double anchor_frame = view_.scroll + anchor_x * view_.fpp;
    view_.fpp = std::clamp(view_.fpp * factor, kMinFpp, max_fpp);
    view_.scroll = anchor_frame - anchor_x * view_.fpp;
    clamp_scroll();
}

namespace {
double zoom_anchor(const ViewState& v, Frames insertion) {
    const double x = v.x_of(insertion);
    return x >= 0 && x <= v.width ? x : v.width / 2;
}
}  // namespace

void EditController::zoom_in() { zoom_by(0.5, zoom_anchor(view_, sel_.insertion)); }
void EditController::zoom_out() { zoom_by(2.0, zoom_anchor(view_, sel_.insertion)); }

bool EditController::zoom_fit_session(std::string& why) {
    if (view_.width <= 0) {
        why = "the timeline has no width yet";
        return false;
    }
    const double span = static_cast<double>(session_end()) * 1.05;
    view_.fpp = std::max(kMinFpp, span / view_.width);
    view_.scroll = 0;
    clamp_scroll();
    return true;
}

bool EditController::zoom_fit_selection(std::string& why) {
    Frames a = sel_.start, b = sel_.end;
    if (!sel_.has_range()) {
        why = "nothing is selected to fit";
        return false;
    }
    if (view_.width <= 0) {
        why = "the timeline has no width yet";
        return false;
    }
    const double span = static_cast<double>(b - a);
    view_.fpp = std::max(kMinFpp, span * 1.1 / view_.width);
    view_.scroll = static_cast<double>(a) - span * 0.05;
    clamp_scroll();
    return true;
}

bool EditController::zoom_toggle(std::string& why) {
    if (has_saved_view_) {
        view_.fpp = saved_view_.fpp;
        view_.scroll = saved_view_.scroll;
        has_saved_view_ = false;
        clamp_scroll();
        return true;
    }
    const ViewState before = view_;
    if (!(sel_.has_range() ? zoom_fit_selection(why) : zoom_fit_session(why))) return false;
    saved_view_ = before;
    has_saved_view_ = true;
    return true;
}

void EditController::vertical_zoom(bool in) {
    refresh_rows();
    // Every open lane above the selection grows too, so a kept scroll_y
    // would carry the lane being edited down and off screen: the selected
    // lane, else the selected track, stays where it is.
    const bool anchored = !view_.tops.empty() && track(sel_.track) != nullptr;
    const auto anchor_y = [&] {
        return sel_.autolane >= 0 && view_.autolanes_open(sel_.track)
                   ? view_.y_of_autolane(sel_.track, sel_.autolane)
                   : view_.y_of_track(sel_.track);
    };
    const double before = anchored ? anchor_y() : 0.0;
    view_.wave_scale = std::clamp(view_.wave_scale * (in ? 1.5f : 1.0f / 1.5f), 0.25f, 8.0f);
    // Lanes grow with the waveforms so a curve can be edited as finely as a
    // clip is read; the clamp keeps a lane usable at both ends.
    view_.autolane_height = std::clamp(ViewState::kAutolaneBaseHeight * view_.wave_scale, 32.0, 224.0);
    view_.comp_lane_height =
        std::clamp(ViewState::kCompLaneBaseHeight * view_.wave_scale, 32.0, 160.0);
    if (anchored) {
        refresh_rows();
        view_.scroll_y += anchor_y() - before;
    }
    clamp_scroll();
}

void EditController::scroll_by(double dx_px, double dy_px) {
    view_.scroll += dx_px * view_.fpp;
    view_.scroll_y += dy_px;
    clamp_scroll();
}

void EditController::scroll_to_frame(double left_frame) {
    view_.scroll = left_frame;
    clamp_scroll();
}

bool EditController::follow(Frames playhead) {
    if (view_.width <= 0) return false;
    const double x = view_.x_of(playhead);
    if (x >= 0 && x <= view_.width - 16) return false;
    // Page, as a console operator expects: the playhead jumps back to the
    // left a tenth in, and the view holds still until it reaches the edge.
    view_.scroll = static_cast<double>(playhead) - view_.width * 0.1 * view_.fpp;
    clamp_scroll();
    return true;
}

// --- modes, tools ---

void EditController::next_mode() {
    mode_ = static_cast<EditMode>((static_cast<int>(mode_) + 1) % 4);
}

// Every tool, the Pencil included (keymaps/README.md, "The tool cycle").
void EditController::next_tool() {
    tool_ = static_cast<Tool>((static_cast<int>(tool_) + 1) % (static_cast<int>(Tool::Pencil) + 1));
}

Frames EditController::snapped(Frames f) const {
    if (mode_ == EditMode::Grid) return snap_to_grid(f, grid_, snapshot().settings);
    return std::max<Frames>(0, f);
}

bool EditController::refuse_in_mode(const char* what, std::string& why) const {
    // Spot trims and nudges as Slip does: only its mouse move asks for a time.
    if (mode_ != EditMode::Shuffle) return false;
    why = std::string(what) + " in Shuffle mode is not built yet; use Slip, Spot or Grid";
    return true;
}

// --- selection ---

void EditController::select_track(int index) {
    if (track(index) == nullptr) return;
    if (index != sel_.track || sel_.tracks != 1) {
        sel_.region = kNoId;
        sel_.autolane = sel_.point = -1;
        sel_.start = sel_.end = sel_.insertion;
    }
    sel_.track = index;
    sel_.tracks = 1;
}

bool EditController::select_next_track(int delta, std::string& why) {
    refresh_rows();
    const int n = static_cast<int>(snapshot().edit->tracks.size());
    if (n == 0) {
        why = "the session has no tracks";
        return false;
    }
    const int next = sel_.track < 0 ? 0 : std::clamp(sel_.track + delta, 0, n - 1);
    select_track(next);
    // Keep the selected lane on screen.
    const double y = view_.y_of_track(next);
    if (y < 0) view_.scroll_y += y;
    else if (y + view_.track_height > view_.height) view_.scroll_y += y + view_.track_height - view_.height;
    clamp_scroll();
    return true;
}

void EditController::set_insertion(Frames f) {
    sel_.insertion = std::max<Frames>(0, snapped(f));
    sel_.start = sel_.end = sel_.insertion;
    sel_.region = kNoId;
    sel_.autolane = sel_.point = -1;
}

bool EditController::select_region(Id region) {
    const TrackEdit* t = nullptr;
    const Region* r = find_region(snapshot(), region, &t);
    if (r == nullptr) return false;
    sel_.track = track_index(snapshot(), t->id);
    sel_.tracks = 1;
    sel_.region = r->id;
    sel_.autolane = sel_.point = -1;
    sel_.start = sel_.insertion = r->position;
    sel_.end = r->end();
    return true;
}

void EditController::select_range(int index, Frames a, Frames b, int count) {
    if (track(index) == nullptr) return;
    sel_.track = index;
    sel_.tracks = std::clamp(count, 1, static_cast<int>(snapshot().edit->tracks.size()) - index);
    sel_.region = kNoId;
    sel_.autolane = sel_.point = -1;
    sel_.start = sel_.insertion = std::max<Frames>(0, std::min(a, b));
    sel_.end = std::max<Frames>(0, std::max(a, b));
}

bool EditController::select_all(std::string& why) {
    const auto& ts = snapshot().edit->tracks;
    if (ts.empty()) {
        why = "the session has no tracks";
        return false;
    }
    Frames end = 0;
    for (const auto& t : ts) {
        if (const Playlist* p = active(t.get())) {
            if (!p->regions.empty()) end = std::max(end, p->regions.back().end());
        }
    }
    if (end == 0) {
        why = "there is no region in the session to select";
        return false;
    }
    select_span(0, static_cast<int>(ts.size()), 0, end);
    return true;
}

void EditController::clear_selection() {
    sel_.region = kNoId;
    sel_.autolane = sel_.point = -1;
    sel_.start = sel_.end = sel_.insertion;
}

void EditController::clamp_selection() {
    if (const int n = span()) sel_.tracks = n;
}

// --- mouse ---

}  // namespace sonora::ui
