// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit_controller.h"

#include <algorithm>
#include <cmath>

#include "fader_taper.h"
#include "sonora/fade_curve.h"
#include "theme.h"

namespace sonora::ui {

using namespace model;

namespace {

constexpr double kMinFpp = 1.0;  // one sample a pixel
constexpr double kEdgePx = 8.0;  // the Smart Tool's trim zone at each end
constexpr double kDragSlopPx = 3.0;
constexpr double kFadeZonePx = 12.0;  // the Smart Tool's fade corners, in the lane's top quarter

const Playlist* active(const TrackEdit* t) { return active_playlist(t); }

}  // namespace

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
    }
    return "";
}

Frames ViewState::frame_at(double x) const {
    return static_cast<Frames>(std::llround(scroll + x * fpp));
}

int ViewState::track_at(double y) const {
    if (track_height <= 0) return -1;
    return static_cast<int>(std::floor((y + scroll_y) / track_height));
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

namespace {

// How far into a region the fades at each end reach, crossfades included:
// what a new fade at the other end may not run into.
Frames reach_in(const RegionFades& f) {
    if (f.in_crossfade == nullptr) return f.in;
    Frames before = 0, after = 0;
    crossfade_extent(*f.in_crossfade, before, after);
    return after;
}

Frames reach_out(const RegionFades& f) {
    if (f.out_crossfade == nullptr) return f.out;
    Frames before = 0, after = 0;
    crossfade_extent(*f.out_crossfade, before, after);
    return before;
}

// What a new fade at one end may not run into: the other end's reach as it
// is heard, or that end's own stored fade if a crossfade hides it, whichever
// is longer. The model keeps the hidden fade and still counts it
// (region_problem: fade_in + fade_out <= length), so a limit from the heard
// reach alone would pass a drag the model then refuses.
Frames taken_at_end(const Region& r, const RegionFades& f) {
    return std::max(reach_out(f), r.fade_out.length);
}

Frames taken_at_start(const Region& r, const RegionFades& f) {
    return std::max(reach_in(f), r.fade_in.length);
}

const Region* region_on(const Playlist& p, Id id) {
    for (const Region& r : p.regions) {
        if (r.id == id) return &r;
    }
    return nullptr;
}

std::string named(const Region& r) {
    return r.name.empty() ? "region " + std::to_string(r.id) : "\"" + r.name + "\"";
}

std::string crossfaded_refusal(const Playlist& p, const Region& r, const Crossfade& x, bool in) {
    const Region* other = region_on(p, in ? x.left : x.right);
    return named(r) + "'s " + (in ? "start" : "end") + " is crossfaded with " +
           (other ? named(*other) : std::string("its neighbour")) +
           "; select across the cut and fade the selection to change it";
}

}  // namespace

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
    sel_ = Selection{};
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
    const double lanes = static_cast<double>(snapshot().edit->tracks.size()) * view_.track_height;
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
    view_.wave_scale = std::clamp(view_.wave_scale * (in ? 1.5f : 1.0f / 1.5f), 0.25f, 8.0f);
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

void EditController::next_tool() { tool_ = static_cast<Tool>((static_cast<int>(tool_) + 1) % 4); }

Frames EditController::snapped(Frames f) const {
    if (mode_ == EditMode::Grid) return snap_to_grid(f, grid_, snapshot().settings);
    return std::max<Frames>(0, f);
}

bool EditController::refuse_in_mode(const char* what, std::string& why) const {
    if (mode_ == EditMode::Slip || mode_ == EditMode::Grid) return false;
    why = std::string(what) + " in " + edit_mode_name(mode_) +
          " mode is not built yet; use Slip or Grid";
    return true;
}

// --- selection ---

void EditController::select_track(int index) {
    if (track(index) == nullptr) return;
    if (index != sel_.track) {
        sel_.region = kNoId;
        sel_.start = sel_.end = sel_.insertion;
    }
    sel_.track = index;
}

bool EditController::select_next_track(int delta, std::string& why) {
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
}

bool EditController::select_region(Id region) {
    const TrackEdit* t = nullptr;
    const Region* r = find_region(snapshot(), region, &t);
    if (r == nullptr) return false;
    sel_.track = track_index(snapshot(), t->id);
    sel_.region = r->id;
    sel_.start = sel_.insertion = r->position;
    sel_.end = r->end();
    return true;
}

void EditController::clear_selection() {
    sel_.region = kNoId;
    sel_.start = sel_.end = sel_.insertion;
}

// --- mouse ---

Hit EditController::hit(double x, double y) const {
    Hit h;
    const int i = view_.track_at(y);
    if (track(i) == nullptr || x < 0 || x > view_.width) return h;
    h.track = i;
    h.zone = HitZone::Lane;
    const Region* r = region_at(i, view_.frame_at(x));
    if (r == nullptr) return h;
    h.region = r->id;
    const double x0 = view_.x_of(r->position), x1 = view_.x_of(r->end());
    const double edge = std::min(kEdgePx, (x1 - x0) / 4);
    const double corner = std::min(kFadeZonePx, (x1 - x0) / 4);
    const bool top = y - view_.y_of_track(i) < view_.track_height / 4;
    if (top && x - x0 <= corner) h.zone = HitZone::FadeIn;
    else if (top && x1 - x <= corner) h.zone = HitZone::FadeOut;
    else if (x - x0 <= edge) h.zone = HitZone::TrimStart;
    else if (x1 - x <= edge) h.zone = HitZone::TrimEnd;
    else if (y - view_.y_of_track(i) < view_.track_height / 2) h.zone = HitZone::Select;
    else h.zone = HitZone::Body;
    return h;
}

bool EditController::press(double x, double y, std::string& why) {
    drag_ = Drag{};
    const Hit h = hit(x, y);
    if (h.track < 0) return true;
    select_track(h.track);

    Drag::Kind kind = Drag::Kind::Range;
    const bool in_region = h.region != kNoId;
    switch (tool_) {
        case Tool::Smart:
            if (h.zone == HitZone::TrimStart) kind = Drag::Kind::TrimStart;
            else if (h.zone == HitZone::TrimEnd) kind = Drag::Kind::TrimEnd;
            else if (h.zone == HitZone::Body) kind = Drag::Kind::Move;
            else if (h.zone == HitZone::FadeIn) kind = Drag::Kind::FadeIn;
            else if (h.zone == HitZone::FadeOut) kind = Drag::Kind::FadeOut;
            break;
        case Tool::Grab:
            if (in_region) kind = Drag::Kind::Move;
            break;
        case Tool::Trim:
            if (in_region) {
                const Region* r = find_region(snapshot(), h.region);
                const Frames f = view_.frame_at(x);
                kind = f - r->position < r->end() - f ? Drag::Kind::TrimStart : Drag::Kind::TrimEnd;
            }
            break;
        case Tool::Select: break;
    }

    drag_.kind = kind;
    drag_.track = h.track;
    drag_.press_x = x;
    drag_.press_frame = view_.frame_at(x);
    if (kind == Drag::Kind::Range) {
        set_insertion(drag_.press_frame);
        drag_.press_frame = sel_.insertion;
        drag_.start = drag_.end = sel_.insertion;
        return true;
    }
    if (kind == Drag::Kind::FadeIn || kind == Drag::Kind::FadeOut) {
        // No mode refusal: a fade moves no region, so Shuffle and Spot allow
        // it as Slip does.
        const TrackEdit* t = nullptr;
        const Region* r = find_region(snapshot(), h.region, &t);
        const RegionFades f = region_fades(*active(t), *r);
        const bool in = kind == Drag::Kind::FadeIn;
        if (const Crossfade* x = in ? f.in_crossfade : f.out_crossfade) {
            why = crossfaded_refusal(*active(t), *r, *x, in);
            drag_ = Drag{};
            return false;
        }
        drag_.region = r->id;
        drag_.orig_start = drag_.start = r->position;
        drag_.orig_end = drag_.end = r->end();
        drag_.fade = in ? f.in : f.out;
        sel_.region = r->id;
        sel_.start = r->position;
        sel_.end = r->end();
        sel_.insertion = r->position;
        return true;
    }
    if (refuse_in_mode(kind == Drag::Kind::Move ? "Moving a region" : "Trimming a region", why)) {
        drag_ = Drag{};
        return false;
    }
    const Region* r = find_region(snapshot(), h.region);
    drag_.region = h.region;
    drag_.orig_start = drag_.start = r->position;
    drag_.orig_end = drag_.end = r->end();
    sel_.region = r->id;
    sel_.start = r->position;
    sel_.end = r->end();
    sel_.insertion = r->position;
    return true;
}

void EditController::move(double x, double /*y*/) {
    if (drag_.kind == Drag::Kind::None) return;
    if (std::abs(x - drag_.press_x) >= kDragSlopPx) drag_.moved = true;
    if (!drag_.moved) return;
    const Frames delta = view_.frame_at(x) - drag_.press_frame;
    const Snapshot& s = snapshot();
    switch (drag_.kind) {
        case Drag::Kind::Move: {
            const Frames to = snapped(drag_.orig_start + delta);
            drag_.start = to;
            drag_.end = to + (drag_.orig_end - drag_.orig_start);
            break;
        }
        case Drag::Kind::TrimStart:
        case Drag::Kind::TrimEnd: {
            const bool start = drag_.kind == Drag::Kind::TrimStart;
            TrimRegion t;
            const Frames want = snapped((start ? drag_.orig_start : drag_.orig_end) + delta);
            drag_.start = drag_.orig_start;
            drag_.end = drag_.orig_end;
            drag_.preview.reset();
            if (resolve_trim(s, drag_.region, start ? Edge::Start : Edge::End, want, t)) {
                (start ? drag_.start : drag_.end) = t.to;
                // What the drop will do to the neighbours, drawn while the
                // edge is still in the hand: apply() is pure, so the preview
                // is the edit itself, applied to a copy nobody else sees.
                Snapshot after;
                std::string ignored;
                const TrackEdit* track = nullptr;
                if (apply(s, t, after, ignored) && find_region(after, drag_.region, &track)) {
                    for (const auto& p : track->playlists) {
                        if (p->id == track->active_playlist) drag_.preview = p;
                    }
                }
            }
            break;
        }
        case Drag::Kind::Range: {
            const Frames f = snapped(view_.frame_at(x));
            drag_.start = std::min(drag_.press_frame, f);
            drag_.end = std::max(drag_.press_frame, f);
            break;
        }
        case Drag::Kind::FadeIn:
        case Drag::Kind::FadeOut: {
            const TrackEdit* t = nullptr;
            const Region* r = find_region(s, drag_.region, &t);
            if (r == nullptr) break;
            const RegionFades f = region_fades(*active(t), *r);
            const Frames at = snapped(view_.frame_at(x));
            // Up to the other end's fade, never through it: the model refuses
            // fades that overlap, and a drag stops at the limit rather than
            // ending in a refusal.
            if (drag_.kind == Drag::Kind::FadeIn) {
                drag_.fade = std::clamp<Frames>(at - r->position, 0, r->length - taken_at_end(*r, f));
            } else {
                drag_.fade = std::clamp<Frames>(r->end() - at, 0, r->length - taken_at_start(*r, f));
            }
            break;
        }
        case Drag::Kind::None: break;
    }
}

bool EditController::release(double x, double y, std::string& why) {
    move(x, y);
    const Drag d = drag_;
    drag_ = Drag{};
    if (d.kind == Drag::Kind::None || !d.moved) return true;
    switch (d.kind) {
        case Drag::Kind::Range:
            sel_.start = d.start;
            sel_.end = d.end;
            sel_.insertion = d.start;
            return true;
        case Drag::Kind::Move: {
            MoveRegion op;
            if (!resolve_move(snapshot(), d.region, d.start, doc_.reserve_id(), op)) return true;
            if (!submit(op, why)) return false;
            break;
        }
        case Drag::Kind::TrimStart:
        case Drag::Kind::TrimEnd: {
            const bool start = d.kind == Drag::Kind::TrimStart;
            TrimRegion op;
            if (!resolve_trim(snapshot(), d.region, start ? Edge::Start : Edge::End,
                              start ? d.start : d.end, op)) {
                return true;
            }
            if (!submit(op, why)) return false;
            break;
        }
        case Drag::Kind::FadeIn:
        case Drag::Kind::FadeOut:
            if (!set_region_fade(d.region,
                                 d.kind == Drag::Kind::FadeIn ? FadeEnd::In : FadeEnd::Out,
                                 d.fade, why)) {
                return false;
            }
            break;
        case Drag::Kind::None: break;
    }
    if (const Region* r = find_region(snapshot(), d.region)) {
        sel_.region = r->id;
        sel_.start = r->position;
        sel_.end = r->end();
        sel_.insertion = r->position;
    }
    return true;
}

// --- edit commands ---

bool EditController::submit(const Operation& op, std::string& why) {
    if (!doc_.submit(op, why)) return false;
    if (sel_.region != kNoId && find_region(snapshot(), sel_.region) == nullptr) {
        sel_.region = kNoId;
        sel_.start = sel_.end = sel_.insertion;
    }
    return true;
}

const Region* EditController::target_region(int& index) const {
    index = sel_.track;
    if (sel_.region != kNoId) {
        const TrackEdit* t = nullptr;
        const Region* r = find_region(snapshot(), sel_.region, &t);
        if (r) {
            index = track_index(snapshot(), t->id);
            if (sel_.insertion > r->position && sel_.insertion < r->end()) return r;
        }
    }
    return region_at(index, sel_.insertion);
}

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
    int done = 0;
    for (Frames f : at) {
        const Region* r = region_at(sel_.track, f);
        if (r == nullptr || f <= r->position) continue;
        if (!submit(SplitRegion{t->active_playlist, r->id, f, doc_.reserve_id()}, why)) return false;
        ++done;
    }
    if (done == 0) {
        why = "no region to separate at the insertion point on " + t->name +
              "; click inside a region's upper half to place it";
        return false;
    }
    sel_.region = kNoId;
    return true;
}

bool EditController::trim_start_to_insertion(std::string& why) {
    int index = -1;
    const Region* r = target_region(index);
    if (refuse_in_mode("Trimming", why)) return false;
    TrimRegion op;
    if (r == nullptr || !resolve_trim(snapshot(), r->id, Edge::Start, sel_.insertion, op)) {
        why = "the insertion point is not inside a region on the selected track";
        return false;
    }
    return submit(op, why);
}

bool EditController::trim_end_to_insertion(std::string& why) {
    int index = -1;
    const Region* r = target_region(index);
    if (refuse_in_mode("Trimming", why)) return false;
    TrimRegion op;
    if (r == nullptr || !resolve_trim(snapshot(), r->id, Edge::End, sel_.insertion, op)) {
        why = "the insertion point is not inside a region on the selected track";
        return false;
    }
    return submit(op, why);
}

bool EditController::clear(std::string& why) {
    bool fade = false;
    if (!clear_fade_at_selection(fade, why)) return false;
    if (fade) return true;
    if (refuse_in_mode("Clearing", why)) return false;
    if (sel_.region == kNoId) {
        why = sel_.has_range()
                  ? "clearing part of a region is not built yet; select a whole region (lower half)"
                  : "nothing is selected";
        return false;
    }
    const TrackEdit* t = nullptr;
    if (find_region(snapshot(), sel_.region, &t) == nullptr) {
        why = "the selected region is gone";
        return false;
    }
    return submit(RemoveRegion{t->active_playlist, sel_.region}, why);
}

bool EditController::nudge(int direction, std::string& why) {
    if (refuse_in_mode("Nudging", why)) return false;
    const Region* r = sel_.region != kNoId ? find_region(snapshot(), sel_.region) : nullptr;
    if (r == nullptr) {
        why = "select a region to nudge";
        return false;
    }
    const auto step = static_cast<Frames>(std::llround(grid_frames(grid_, snapshot().settings)));
    MoveRegion op;
    const Id id = r->id;
    if (!resolve_move(snapshot(), id, r->position + direction * step, doc_.reserve_id(), op)) {
        why = "the region is already at the session start";
        return false;
    }
    if (!submit(op, why)) return false;
    if (const Region* moved = find_region(snapshot(), id)) {
        sel_.start = moved->position;
        sel_.end = moved->end();
        sel_.insertion = moved->position;
    }
    return true;
}

// --- fades ---

bool EditController::set_region_fade(Id region, FadeEnd end, Frames length, std::string& why) {
    const TrackEdit* t = nullptr;
    const Region* r = find_region(snapshot(), region, &t);
    if (r == nullptr) {
        why = "the region is gone";
        return false;
    }
    const Playlist& p = *active(t);
    const RegionFades f = region_fades(p, *r);
    const bool in = end == FadeEnd::In;
    if (const Crossfade* x = in ? f.in_crossfade : f.out_crossfade) {
        why = crossfaded_refusal(p, *r, *x, in);
        return false;
    }
    const Frames room = r->length - (in ? taken_at_end(*r, f) : taken_at_start(*r, f));
    if (length < 0 || length > room) {
        const char* self = in ? "in" : "out";
        const char* other = in ? "out" : "in";
        why = std::string("a fade-") + self + " that long would run into " + named(*r) +
              "'s fade-" + other;
        const Crossfade* hiding = in ? f.out_crossfade : f.in_crossfade;
        const Frames stored = in ? r->fade_out.length : r->fade_in.length;
        const Frames heard = in ? reach_out(f) : reach_in(f);
        if (hiding != nullptr && length >= 0 && length <= r->length - heard && stored > heard) {
            // The limit is a fade nobody can see: say where it is and how to
            // reach it, rather than name a fade that is not drawn.
            const Region* n = region_on(p, in ? hiding->right : hiding->left);
            why += std::string(", kept under its crossfade with ") +
                   (n ? named(*n) : std::string("its neighbour")) +
                   "; clear the crossfade, then that fade-" + other + ", to make room";
        }
        return false;
    }
    const Fade& now = in ? r->fade_in : r->fade_out;
    if (now.length == length) return true;
    // A fade being reshaped in length keeps its curve; a new one takes the
    // last one used.
    const Fade fade{length, now.length > 0 ? now.shape : fade_shape_};
    return submit(SetFade{p.id, r->id, end, fade}, why);
}

bool EditController::fade_to_start(std::string& why) {
    int index = -1;
    const Region* r = target_region(index);
    if (r == nullptr || sel_.insertion <= r->position) {
        why = "the insertion point is not inside a region on the selected track";
        return false;
    }
    return set_region_fade(r->id, FadeEnd::In, sel_.insertion - r->position, why);
}

bool EditController::fade_to_end(std::string& why) {
    int index = -1;
    const Region* r = target_region(index);
    if (r == nullptr || sel_.insertion <= r->position) {
        why = "the insertion point is not inside a region on the selected track";
        return false;
    }
    return set_region_fade(r->id, FadeEnd::Out, r->end() - sel_.insertion, why);
}

bool EditController::fade_selection(std::string& why) {
    const TrackEdit* t = track(sel_.track);
    if (t == nullptr) {
        why = "select a track first";
        return false;
    }
    if (!sel_.has_range()) {
        why = "select a time range across a region's start, its end, or the cut between two "
              "regions";
        return false;
    }
    const Playlist& p = *active(t);
    const Frames a = sel_.start, b = sel_.end;

    // Cuts: two regions that butt, inside the range or at either end of it.
    std::vector<std::size_t> cuts;  // the left region's index
    for (std::size_t i = 0; i + 1 < p.regions.size(); ++i) {
        const Frames cut = p.regions[i].end();
        if (cut == p.regions[i + 1].position && cut >= a && cut <= b) cuts.push_back(i);
    }
    if (cuts.size() > 1) {
        why = "the selection crosses " + std::to_string(cuts.size()) +
              " cuts; batch fades are not built yet, so select across one";
        return false;
    }
    if (cuts.size() == 1) {
        const Region& l = p.regions[cuts[0]];
        const Region& r = p.regions[cuts[0] + 1];
        const Frames cut = l.end();
        Crossfade x;
        x.left = l.id;
        x.right = r.id;
        x.length = b - a;
        x.shape = crossfade_shape_;
        // Pre and post when the range stops at the cut, as Pro Tools does.
        // Otherwise centred on the cut: the model has no other alignment, so
        // a lopsided range becomes a centred crossfade of its length, and
        // the selection is redrawn to show exactly what was made.
        x.align = b == cut ? Align::Pre : a == cut ? Align::Post : Align::Centre;
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        // Not into the fade at either region's other end. The model refuses
        // two crossfades that meet inside a region but not a crossfade over
        // the region's own fade, which the engine would multiply: two fades
        // at once over the overlap, heard as a dip nobody drew. Refused, not
        // shortened, as the model refuses a missing handle.
        const RegionFades lf = region_fades(p, l), rf = region_fades(p, r);
        if (before > l.length - reach_in(lf) || after > r.length - reach_out(rf)) {
            const bool left = before > l.length - reach_in(lf);
            const Region& n = left ? l : r;
            const bool crossfaded = (left ? lf.in_crossfade : rf.out_crossfade) != nullptr;
            const char* end = left ? "start" : "end";
            why = "the crossfade would overlap " +
                  (crossfaded ? "the crossfade at " + named(n) + "'s " + end
                              : named(n) + "'s fade-" + (left ? "in" : "out")) +
                  "; shorten that first, or select less of " + named(n);
            return false;
        }
        if (!submit(SetCrossfade{p.id, x}, why)) return false;
        sel_.region = kNoId;
        sel_.start = sel_.insertion = cut - before;
        sel_.end = cut + after;
        return true;
    }

    std::vector<const Region*> under;
    for (const Region& r : p.regions) {
        if (r.position < b && r.end() > a) under.push_back(&r);
    }
    if (under.empty()) {
        why = "there is no region in the selection on " + t->name;
        return false;
    }
    if (under.size() > 1) {
        why = "the selection covers " + std::to_string(under.size()) +
              " regions; batch fades are not built yet, so select across one region's start "
              "or end";
        return false;
    }
    const Region& r = *under[0];
    const bool has_start = a <= r.position, has_end = b >= r.end();
    if (has_start && has_end) {
        why = "the selection covers the whole of " + named(r) +
              "; select across its start for a fade-in or its end for a fade-out";
        return false;
    }
    if (!has_start && !has_end) {
        why = "a fade needs the selection to include " + named(r) +
              "'s start or end, or a cut between two regions";
        return false;
    }
    const Frames length = has_start ? b - r.position : r.end() - a;
    const Frames from = has_start ? r.position : a;
    if (!set_region_fade(r.id, has_start ? FadeEnd::In : FadeEnd::Out, length, why)) return false;
    sel_.region = kNoId;
    sel_.start = sel_.insertion = from;
    sel_.end = from + length;
    return true;
}

bool EditController::clear_fade_at_selection(bool& done, std::string& why) {
    done = false;
    const TrackEdit* t = track(sel_.track);
    if (t == nullptr || sel_.region != kNoId || !sel_.has_range()) return true;
    const Playlist& p = *active(t);
    for (const Crossfade& x : p.crossfades) {
        const Region* r = region_on(p, x.right);
        if (r == nullptr) continue;
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        if (sel_.start == r->position - before && sel_.end == r->position + after) {
            done = true;
            return submit(RemoveCrossfade{p.id, x.left, x.right}, why);
        }
    }
    for (const Region& r : p.regions) {
        const RegionFades f = region_fades(p, r);
        const bool in = f.in > 0 && sel_.start == r.position && sel_.end == r.position + f.in;
        const bool out = f.out > 0 && sel_.start == r.end() - f.out && sel_.end == r.end();
        if (!in && !out) continue;
        done = true;
        Fade gone = in ? r.fade_in : r.fade_out;
        gone.length = 0;
        return submit(SetFade{p.id, r.id, in ? FadeEnd::In : FadeEnd::Out, gone}, why);
    }
    return true;
}

bool EditController::undo(std::string& why) {
    drag_ = Drag{};
    if (!doc_.undo(why)) return false;
    if (sel_.region != kNoId && find_region(snapshot(), sel_.region) == nullptr) clear_selection();
    return true;
}

bool EditController::redo(std::string& why) {
    drag_ = Drag{};
    if (!doc_.redo(why)) return false;
    if (sel_.region != kNoId && find_region(snapshot(), sel_.region) == nullptr) clear_selection();
    return true;
}

// --- mix ---
//
// A mix operation replaces a track's whole TrackMix (operations.h, SetMix),
// so each of these is a small resolver: the current mix with one field
// changed. A gain is clamped to the fader's travel here, because the model
// accepts any finite number and the fader cannot show one beyond its ends.
// A gain already beyond an end (a file saved at +18 dB) widens the range to
// include it, so touching the fader does not quietly take 6 dB off: it stays
// until the operator moves it back onto the travel.

namespace {
bool mix_target(const Snapshot& s, int index, TrackMix& mix, std::string& why) {
    if (index < 0 || index >= static_cast<int>(s.edit->tracks.size())) {
        why = index < 0 ? "no track is selected" : "there is no track " + std::to_string(index + 1);
        return false;
    }
    const TrackEdit& t = *s.edit->tracks[static_cast<std::size_t>(index)];
    const TrackMix* m = s.mix.find(t.id);
    if (m == nullptr) {
        why = t.name + " has no mix settings";
        return false;
    }
    mix = *m;
    return true;
}

// `gain` is the current value on entry and the new one on success.
bool fader_gain(float db, float& gain, std::string& why) {
    if (!std::isfinite(db)) {
        why = "a gain must be a number of dB";
        return false;
    }
    gain = std::clamp(db, std::min(kFaderSilenceDb, gain), std::max(kFaderMaxDb, gain));
    return true;
}
}  // namespace

bool EditController::set_gain(int index, float db, std::string& why) {
    TrackMix m;
    if (!mix_target(snapshot(), index, m, why) || !fader_gain(db, m.gain_db, why)) return false;
    return submit(SetMix{m}, why);
}

bool EditController::set_pan(int index, float pan, std::string& why) {
    TrackMix m;
    if (!mix_target(snapshot(), index, m, why)) return false;
    if (!std::isfinite(pan)) {
        why = "a pan position must be a number";
        return false;
    }
    m.pan = std::clamp(pan, -1.0f, 1.0f);
    return submit(SetMix{m}, why);
}

bool EditController::toggle_mute(int index, std::string& why) {
    TrackMix m;
    if (!mix_target(snapshot(), index, m, why)) return false;
    m.mute = !m.mute;
    return submit(SetMix{m}, why);
}

bool EditController::toggle_solo(int index, std::string& why) {
    TrackMix m;
    if (!mix_target(snapshot(), index, m, why)) return false;
    m.solo = !m.solo;
    return submit(SetMix{m}, why);
}

bool EditController::set_bus_gain(float db, std::string& why) {
    MonitorBus bus = snapshot().mix.bus;
    if (!fader_gain(db, bus.gain_db, why)) return false;
    return submit(SetMonitorBus{bus}, why);
}

}  // namespace sonora::ui
