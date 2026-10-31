// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

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

// Why no crossfade fits the handles at a cut (ADR-0021), and where one would.
std::string no_handle_refusal(const Region& l, const Region& r, const Crossfade& x, Frames before,
                              Frames after) {
    const std::string none_before = named(r) + " starts where its audio does: no handle before the cut";
    const std::string none_after = named(l) + " ends where its audio does: no handle after the cut";
    switch (x.align) {
        case Align::Pre:
            return none_before + (after > 0 ? "; select from the cut onwards to crossfade after it" : "");
        case Align::Post:
            return none_after + (before > 0 ? "; select up to the cut to crossfade before it" : "");
        case Align::Centre:
            break;
    }
    return "no crossfade fits: " + none_before + ", and " + none_after;
}

// What a crossfade fitted to its handles became, and which handle held it
// back (ADR-0021 §4). A centred one that moved to one side at its full
// length was not shortened, and saying so would send the operator looking
// for frames that are all there.
std::string shortened_note(const Region& l, const Region& r, const Crossfade& asked,
                           const Crossfade& made, Frames before, Frames after) {
    Frames want_before = 0, want_after = 0;
    crossfade_extent(asked, want_before, want_after);
    std::string why;
    if (made.align != asked.align || before < want_before) {
        why = named(r) + " has " + std::to_string(before) + " frames of handle before its start";
    }
    if (made.align != asked.align || after < want_after) {
        why += (why.empty() ? "" : ", ") + named(l) + " has " + std::to_string(after) +
               " frames after its end";
    }
    const char* side = made.align == Align::Pre ? "before the cut" : "after the cut";
    if (made.align != asked.align && made.length == asked.length) {
        return std::string("Crossfade moved ") + side + ": " + why;
    }
    return "Crossfade shortened to " + std::to_string(made.length) + " frames" +
           (made.align == asked.align ? std::string() : std::string(", ") + side) + ": " + why;
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
    if (index != sel_.track || sel_.tracks != 1) {
        sel_.region = kNoId;
        sel_.start = sel_.end = sel_.insertion;
    }
    sel_.track = index;
    sel_.tracks = 1;
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
    sel_.tracks = 1;
    sel_.region = r->id;
    sel_.start = sel_.insertion = r->position;
    sel_.end = r->end();
    return true;
}

void EditController::select_range(int index, Frames a, Frames b, int count) {
    if (track(index) == nullptr) return;
    sel_.track = index;
    sel_.tracks = std::clamp(count, 1, static_cast<int>(snapshot().edit->tracks.size()) - index);
    sel_.region = kNoId;
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
    sel_.start = sel_.end = sel_.insertion;
}

void EditController::clamp_selection() {
    if (const int n = span()) sel_.tracks = n;
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
    drag_.track = drag_.track_end = h.track;
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

void EditController::move(double x, double y) {
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
            // Above or below the lanes is the first or the last track, so
            // a drag past the edge still reaches it.
            const int n = static_cast<int>(s.edit->tracks.size());
            drag_.track_end = std::clamp(view_.track_at(y), 0, std::max(0, n - 1));
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
            select_span(std::min(d.track, d.track_end), std::abs(d.track_end - d.track) + 1,
                        d.start, d.end);
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
    // Both ends of a range, on every selected track, are one separation, so
    // one undo step.
    const int tracks = sel_.region != kNoId ? 1 : span();
    std::vector<Member> members;
    for (int i = 0; i < tracks; ++i) {
        for (Frames f : at) {
            const Region* r = region_at(sel_.track + i, f);
            if (r == nullptr || f <= r->position) continue;
            members.push_back(SplitRegion{track(sel_.track + i)->active_playlist, r->id, f,
                                          doc_.reserve_id()});
        }
    }
    if (members.empty()) {
        why = tracks == 1 ? "no region to separate at the insertion point on " + t->name
                          : "no region to separate at the insertion point on any of the " +
                                std::to_string(tracks) + " selected tracks";
        why += "; click inside a region's upper half to place it";
        return false;
    }
    if (members.size() == 1) {
        if (!submit(to_operation(members.front()), why)) return false;
    } else if (!submit_compound(std::move(members), "Separate", "separated", why)) {
        return false;
    }
    sel_.region = kNoId;
    return true;
}

bool EditController::trim_start_to_insertion(std::string& why) {
    if (refuse_several("Trimming to the insertion point", why)) return false;
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
    if (refuse_several("Trimming to the insertion point", why)) return false;
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
    if (refuse_shuffle("Clear", "cleared", why)) return false;
    if (sel_.region != kNoId) {
        const TrackEdit* t = nullptr;
        if (find_region(snapshot(), sel_.region, &t) == nullptr) {
            why = "the selected region is gone";
            return false;
        }
        return submit(RemoveRegion{t->active_playlist, sel_.region}, why);
    }
    if (!sel_.has_range()) {
        why = "nothing is selected";
        return false;
    }
    Span e;
    if (!edit_range(e, "cleared", why)) return false;
    std::vector<Member> members;
    for (int i = 0; i < e.count; ++i) {
        clear_range(*active(track(e.first + i)), e.a, e.b, [this]() { return doc_.reserve_id(); },
                    members);
    }
    if (members.empty()) {
        why = e.count == 1 ? "there is no region in the selection on " + track(e.first)->name
                           : "there is no region in the selection on any of its " +
                                 std::to_string(e.count) + " tracks";
        why += "; nothing was cleared";
        return false;
    }
    return submit_compound(std::move(members), "Clear", "cleared", why);
}

bool EditController::nudge(int direction, std::string& why) {
    if (refuse_in_mode("Nudging", why)) return false;
    const Region* r = sel_.region != kNoId ? find_region(snapshot(), sel_.region) : nullptr;
    if (r == nullptr) {
        why = "select a region to nudge";
        return false;
    }
    MoveRegion op;
    const Id id = r->id;
    const Frames to = nudge_target(r->position, direction, grid_, snapshot().settings);
    if (to == r->position || !resolve_move(snapshot(), id, to, doc_.reserve_id(), op)) {
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

// --- the clipboard ---
//
// Pro Tools Reference Guide, "Editing" chapter: "Cut, Copy, Paste and Clear
// Commands" (Cut moves the selection to the Clipboard; Paste puts it at the
// insertion point or the selection's start, replacing what lies beneath in
// Slip, Spot and Grid), "Duplicate Command" (a copy placed straight after
// the selection) and "Repeat Command" (Duplicate a number of times, the
// number asked in a dialog). The workflow only (ADR-0004).

bool EditController::refuse_shuffle(const char* gesture, const char* nothing_was,
                                    std::string& why) const {
    if (mode_ != EditMode::Shuffle) return false;
    why = std::string(gesture) +
          " in Shuffle mode would move every later region to close or open the gap, and that "
          "ripple is not built yet (it waits for ShiftRegions); use Slip, Spot or Grid. "
          "Nothing was " + nothing_was + ".";
    return true;
}

int EditController::span() const {
    const int n = static_cast<int>(snapshot().edit->tracks.size());
    if (sel_.track < 0 || sel_.track >= n) return 0;
    return std::clamp(sel_.tracks, 1, n - sel_.track);
}

bool EditController::refuse_several(const char* gesture, std::string& why) const {
    if (sel_.region != kNoId || span() <= 1) return false;
    why = std::string(gesture) + " on several tracks is not built yet; select one track";
    return true;
}

bool EditController::edit_range(Span& out, const char* nothing_was, std::string& why) const {
    if (sel_.region != kNoId) {
        const TrackEdit* t = nullptr;
        const Region* r = find_region(snapshot(), sel_.region, &t);
        if (r == nullptr) {
            why = std::string("the selected region is gone; nothing was ") + nothing_was;
            return false;
        }
        out = Span{track_index(snapshot(), t->id), 1, r->position, r->end()};
        return true;
    }
    if (track(sel_.track) == nullptr) {
        why = std::string("select a track first; nothing was ") + nothing_was;
        return false;
    }
    if (!sel_.has_range()) {
        why = std::string("select a region or a time range first; nothing was ") + nothing_was;
        return false;
    }
    out = Span{sel_.track, span(), sel_.start, sel_.end};
    return true;
}

bool EditController::copy_lanes(const Span& e, Clipboard& out, const char* nothing_was,
                                std::string& why) const {
    Clipboard clip;
    bool any = false;
    for (int i = 0; i < e.count; ++i) {
        Clip lane;
        any = copy_range(snapshot(), *track(e.first + i), e.a, e.b, lane) || any;
        clip.lanes.push_back(std::move(lane));
    }
    if (!any) {
        why = e.count == 1 ? "there is no region in the selection on " + track(e.first)->name
                           : "there is no region in the selection on any of its " +
                                 std::to_string(e.count) + " tracks";
        why += std::string("; nothing was ") + nothing_was;
        return false;
    }
    out = std::move(clip);
    return true;
}

void EditController::select_span(int first, int count, Frames a, Frames b) {
    sel_.track = first;
    sel_.tracks = count;
    sel_.region = kNoId;
    sel_.start = sel_.insertion = a;
    sel_.end = b;
}

bool EditController::submit_compound(std::vector<Member> members, const char* gesture,
                                     const char* nothing_was, std::string& why) {
    const std::string too_many = over_limit(gesture, nothing_was, members.size());
    if (!too_many.empty()) {
        why = too_many;
        return false;
    }
    return submit(Compound{std::move(members), gesture}, why);
}

bool EditController::copy(std::string& why) {
    Span e;
    Clipboard clip;
    if (!edit_range(e, "copied", why) || !copy_lanes(e, clip, "copied", why)) return false;
    clipboard_ = std::move(clip);
    ++clipboard_serial_;
    return true;
}

bool EditController::cut(std::string& why) {
    if (refuse_shuffle("Cut", "cut", why)) return false;
    Span e;
    Clipboard clip;
    if (!edit_range(e, "cut", why) || !copy_lanes(e, clip, "cut", why)) return false;
    std::vector<Member> members;
    for (int i = 0; i < e.count; ++i) {
        clear_range(*active(track(e.first + i)), e.a, e.b, [this]() { return doc_.reserve_id(); },
                    members);
    }
    // The clipboard changes only with the edit: a refused Cut leaves both.
    if (!submit_compound(std::move(members), "Cut", "cut", why)) return false;
    clipboard_ = std::move(clip);
    ++clipboard_serial_;
    select_span(e.first, e.count, e.a, e.b);
    return true;
}

namespace {

// "Gtr", "Gtr and Bass", "Gtr, Bass and Keys".
std::string spoken_list(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out += i + 1 == names.size() ? " and " : ", ";
        out += names[i];
    }
    return out;
}

}  // namespace

bool EditController::paste(std::string& why) {
    if (refuse_shuffle("Paste", "pasted", why)) return false;
    if (track(sel_.track) == nullptr) {
        why = "select a track first; nothing was pasted";
        return false;
    }
    if (clipboard_.empty()) {
        why = "the clipboard is empty; cut or copy a selection first";
        return false;
    }
    // Pro Tools pastes each lane as many tracks below the selected one as it
    // was copied from below the first, and leaves out lanes that run past
    // the last track. What is left out is said, never silent.
    const int n = static_cast<int>(snapshot().edit->tracks.size());
    const int lanes = static_cast<int>(clipboard_.lanes.size());
    // One track's material pasted into a selection on several tracks fills
    // every one of them, as Pro Tools does; a clipboard of several lanes
    // keeps their layout, so it never repeats onto a selection's extra tracks.
    const int targets = lanes == 1 && sel_.region == kNoId ? span() : 1;
    const auto lane_for = [this, targets](int i) -> const Clip& {
        return clipboard_.lanes[targets > 1 ? 0 : static_cast<std::size_t>(i)];
    };
    const int fit = targets > 1 ? targets : std::min(lanes, n - sel_.track);
    bool any = false;
    for (int i = 0; i < fit; ++i) {
        const Clip& lane = lane_for(i);
        if (lane.empty()) continue;
        any = true;
        const std::string problem = paste_problem(snapshot(), *track(sel_.track + i), lane);
        if (!problem.empty()) {
            why = problem;
            return false;
        }
    }
    // Only lanes that hold regions are lost by being left out: an empty one
    // would have cleared a track that is not there.
    std::vector<std::string> left_out;
    for (int i = fit; i < lanes; ++i) {
        const Clip& lane = clipboard_.lanes[static_cast<std::size_t>(i)];
        if (!lane.empty()) left_out.push_back(lane.track);
    }
    const std::string last = track(n - 1)->name;
    if (!any) {
        // Only the lanes' gaps would land: a paste that clears and places
        // nothing is not what the operator asked for.
        why = "the clipboard's regions were copied from " + spoken_list(left_out) +
              ", and there is no track below " + last + " to paste them on; nothing was pasted";
        return false;
    }
    const Frames at = sel_.has_range() ? sel_.start : sel_.insertion;
    std::vector<Member> members;
    for (int i = 0; i < fit; ++i) {
        paste_clip(*active(track(sel_.track + i)), at, lane_for(i),
                   [this]() { return doc_.reserve_id(); }, members);
    }
    if (!submit_compound(std::move(members), "Paste", "pasted", why)) return false;
    select_span(sel_.track, fit, at, at + clipboard_.length());
    why.clear();
    if (!left_out.empty()) {
        why = "Pasted " + std::to_string(fit) + " of the clipboard's " + std::to_string(lanes) +
              " tracks: there is no track below " + last + " for what was copied from " +
              spoken_list(left_out) + ".";
    }
    return true;
}

bool EditController::duplicate(std::string& why) {
    if (refuse_shuffle("Duplicate", "duplicated", why)) return false;
    Span e;
    Clipboard clip;
    if (!edit_range(e, "duplicated", why) || !copy_lanes(e, clip, "duplicated", why)) {
        return false;
    }
    std::vector<Member> members;
    for (int i = 0; i < e.count; ++i) {
        paste_clip(*active(track(e.first + i)), e.b, clip.lanes[static_cast<std::size_t>(i)],
                   [this]() { return doc_.reserve_id(); }, members);
    }
    if (!submit_compound(std::move(members), "Duplicate", "duplicated", why)) return false;
    select_span(e.first, e.count, e.b, e.b + clip.length());
    return true;
}

bool EditController::can_repeat(std::string& why, std::string* track_out, Frames* length) const {
    if (refuse_shuffle("Repeat", "repeated", why)) return false;
    Span e;
    Clipboard clip;
    if (!edit_range(e, "repeated", why) || !copy_lanes(e, clip, "repeated", why)) return false;
    if (track_out != nullptr) {
        std::vector<std::string> names;
        for (int i = 0; i < e.count; ++i) names.push_back(track(e.first + i)->name);
        *track_out = spoken_list(names);
    }
    if (length != nullptr) *length = e.b - e.a;
    return true;
}

bool EditController::repeat(int count, std::string& why) {
    if (!can_repeat(why)) return false;
    if (count < 1) {
        why = "Repeat needs a number of repeats of 1 or more; nothing was repeated";
        return false;
    }
    Span e;
    Clipboard clip;
    edit_range(e, "repeated", why);
    copy_lanes(e, clip, "repeated", why);
    // Counted before it is built: a typed 100000 is refused by its size,
    // not by allocating it. A lane with no region costs only its clear, but
    // one lane at least holds a region, so a count that passes is at most
    // the member limit.
    std::size_t total = 0;
    for (int i = 0; i < e.count; ++i) {
        total += repeat_members(*active(track(e.first + i)), e.b,
                                clip.lanes[static_cast<std::size_t>(i)],
                                static_cast<std::size_t>(count));
    }
    const std::string too_many = over_limit("Repeat", "repeated", total);
    if (!too_many.empty()) {
        why = too_many;
        return false;
    }
    std::vector<Member> members;
    for (int i = 0; i < e.count; ++i) {
        repeat_clip(*active(track(e.first + i)), e.b, clip.lanes[static_cast<std::size_t>(i)],
                    count, [this]() { return doc_.reserve_id(); }, members);
    }
    if (!submit_compound(std::move(members), "Repeat", "repeated", why)) return false;
    repeat_count_ = count;
    select_span(e.first, e.count, e.b, e.b + clip.length() * count);
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
    if (refuse_several("Fade to Start", why)) return false;
    int index = -1;
    const Region* r = target_region(index);
    if (r == nullptr || sel_.insertion <= r->position) {
        why = "the insertion point is not inside a region on the selected track";
        return false;
    }
    return set_region_fade(r->id, FadeEnd::In, sel_.insertion - r->position, why);
}

bool EditController::fade_to_end(std::string& why) {
    if (refuse_several("Fade to End", why)) return false;
    int index = -1;
    const Region* r = target_region(index);
    if (r == nullptr || sel_.insertion <= r->position) {
        why = "the insertion point is not inside a region on the selected track";
        return false;
    }
    return set_region_fade(r->id, FadeEnd::Out, r->end() - sel_.insertion, why);
}

bool EditController::fade_selection(std::string& why) {
    if (refuse_several("Fading a selection", why)) return false;
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
        // Short handles shorten it to the audio there is, keeping that
        // placement, as Pro Tools' Adjust Bounds does; the selection below
        // shows what was made, and the note says why (ADR-0021).
        Frames handle_before = 0, handle_after = 0;
        crossfade_handles(l, r, *snapshot().sources, handle_before, handle_after);
        const Crossfade asked = x;
        if (!fit_crossfade(x, handle_before, handle_after)) {
            why = no_handle_refusal(l, r, x, handle_before, handle_after);
            return false;
        }
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        // Not into the fade at either region's other end. The model refuses
        // two crossfades that meet inside a region but not a crossfade over
        // the region's own fade, which the engine would multiply: two fades
        // at once over the overlap, heard as a dip nobody drew. Refused, not
        // shortened, as the model refuses a missing handle. A fade kept
        // under a crossfade at the other end counts too: clearing that
        // crossfade brings it back, over this one.
        const RegionFades lf = region_fades(p, l), rf = region_fades(p, r);
        const bool left = before > l.length - taken_at_start(l, lf);
        if (left || after > r.length - taken_at_end(r, rf)) {
            const Region& n = left ? l : r;
            const Crossfade* other = left ? lf.in_crossfade : rf.out_crossfade;
            const Frames heard = left ? reach_in(lf) : reach_out(rf);
            const Frames stored = left ? l.fade_in.length : r.fade_out.length;
            const std::string fade = named(n) + "'s fade-" + (left ? "in" : "out");
            const char* end = left ? "start" : "end";
            if (other != nullptr && stored > heard &&
                (left ? before <= l.length - heard : after <= r.length - heard)) {
                // The limit is a fade nobody can see: say where it is.
                const Region* m = region_on(p, left ? other->left : other->right);
                why = "the crossfade would overlap " + fade + ", kept under its crossfade with " +
                      (m ? named(*m) : std::string("its neighbour")) +
                      "; clear that crossfade, then that fade-" + (left ? "in" : "out") +
                      ", or select less of " + named(n);
            } else {
                why = "the crossfade would overlap " +
                      (other != nullptr ? "the crossfade at " + named(n) + "'s " + end : fade) +
                      "; shorten that first, or select less of " + named(n);
            }
            return false;
        }
        // Before the submit: l and r live in the snapshot it replaces.
        std::string note;
        if (!(x == asked)) note = shortened_note(l, r, asked, x, handle_before, handle_after);
        // The fades at the cut go with the crossfade, as in Pro Tools: kept
        // under it, nobody can see them, yet they come back on Clear and
        // still take room from a fade at the regions' other ends. One
        // compound, so the gesture stays one undo step that brings them back.
        std::vector<Member> members;
        if (l.fade_out.length > 0) members.push_back(SetFade{p.id, l.id, FadeEnd::Out, Fade{}});
        if (r.fade_in.length > 0) members.push_back(SetFade{p.id, r.id, FadeEnd::In, Fade{}});
        if (members.empty()) {
            if (!submit(SetCrossfade{p.id, x}, why)) return false;
        } else {
            members.push_back(SetCrossfade{p.id, x});
            if (!submit_compound(std::move(members), "Crossfade", "crossfaded", why)) return false;
        }
        sel_.region = kNoId;
        sel_.start = sel_.insertion = cut - before;
        sel_.end = cut + after;
        why = note;
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
    // A fade is on one track; on several, Clear clears the range.
    if (t == nullptr || sel_.region != kNoId || !sel_.has_range() || span() > 1) return true;
    const Playlist& p = *active(t);
    for (const Crossfade& x : p.crossfades) {
        const Region* r = region_on(p, x.right);
        if (r == nullptr) continue;
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        if (sel_.start == r->position - before && sel_.end == r->position + after) {
            done = true;
            // Removing it brings back any fade a region keeps under it: none
            // for a crossfade made here, which drops them, but a document
            // from elsewhere or an older build can have them. One that would run into what is at its region's other end
            // comes back shortened to meet it: the model does that
            // (RemoveCrossfade, PR #69), because the operator cannot see
            // or edit that fade while the other end is crossfaded.
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

bool EditController::undo_to(std::size_t depth, std::string& why) {
    const model::History& h = doc_.history();
    if (depth >= h.step_count()) {
        why = "that step is no longer in the undo history";
        return false;
    }
    const std::size_t from = h.undo_depth();
    const bool back = depth < from;
    const std::size_t total = back ? from - depth : depth - from;
    for (std::size_t done = 0; done < total; ++done) {
        if (!(back ? undo(why) : redo(why))) return false;
    }
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

bool EditController::toggle_solo_safe(int index, std::string& why) {
    TrackMix m;
    if (!mix_target(snapshot(), index, m, why)) return false;
    m.solo_safe = !m.solo_safe;
    return submit(SetMix{m}, why);
}

bool EditController::toggle_input_monitor(int index, std::string& why) {
    TrackMix m;
    if (!mix_target(snapshot(), index, m, why)) return false;
    const TrackEdit& t = *snapshot().edit->tracks[static_cast<std::size_t>(index)];
    if (t.kind != TrackKind::Audio) {
        why = t.name + (t.kind == TrackKind::Midi ? " is a MIDI track" : " is an instrument track") +
              ": it has no audio input to monitor";
        return false;
    }
    m.monitor = m.monitor == MonitorMode::Input ? MonitorMode::Auto : MonitorMode::Input;
    return submit(SetMix{m}, why);
}

namespace {

std::string no_track(int index) {
    return index < 0 ? "no track is selected" : "there is no track " + std::to_string(index + 1);
}

}  // namespace

bool EditController::new_tracks(int count, std::string& why) {
    // The tracks and the reorder that places them, in one compound.
    constexpr int kMost = static_cast<int>(kMaxCompoundMembers) - 1;
    if (count < 1 || count > kMost) {
        why = "make 1 to " + std::to_string(kMost) + " tracks at a time";
        return false;
    }
    const Snapshot& s = snapshot();
    const auto& tracks = s.edit->tracks;
    std::vector<std::string> names;
    std::vector<std::uint16_t> inputs;
    for (const auto& t : tracks) {
        names.push_back(t->name);
        const TrackMix* m = s.mix.find(t->id);
        if (t->kind == TrackKind::Audio && m != nullptr && !m->input.empty()) {
            inputs.push_back(m->input.front());
        }
    }
    const auto has = [](const auto& list, const auto& v) {
        return std::find(list.begin(), list.end(), v) != list.end();
    };
    std::vector<Member> members;
    std::vector<Id> added;
    int number = 1;
    std::uint16_t input = 0;
    for (int i = 0; i < count; ++i) {
        std::string name;
        do {
            name = "Audio " + std::to_string(number++);
        } while (has(names, name));
        names.push_back(name);
        while (has(inputs, input)) ++input;
        inputs.push_back(input);
        AddTrack a;
        a.track = doc_.reserve_id();
        a.playlist = doc_.reserve_id();
        a.name = name;
        a.playlist_name = name + ".01";
        a.mix.input = {input};
        added.push_back(a.track);
        members.push_back(std::move(a));
    }
    const int n = static_cast<int>(tracks.size());
    const int at = sel_.track >= 0 && sel_.track < n ? sel_.track + 1 : n;
    if (at < n) {
        std::vector<Id> order = track_order(*s.edit);
        order.insert(order.begin() + at, added.begin(), added.end());
        members.push_back(ReorderTracks{std::move(order)});
    }
    if (!submit(Compound{std::move(members), count == 1 ? "New Track" : "New Tracks"}, why)) {
        return false;
    }
    select_track(at);
    return true;
}

bool EditController::duplicate_track(int index, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    DuplicateTrack d;
    d.source = t->id;
    d.name = duplicate_track_name(snapshot(), t->id);
    const std::size_t ids = duplicate_track_ids(snapshot(), d);
    // The copy's objects take the ids after `track`, one by one.
    d.track = doc_.reserve_id();
    for (std::size_t i = 1; i < ids; ++i) doc_.reserve_id();
    if (!submit(d, why)) return false;
    select_track(index + 1);
    return true;
}

bool EditController::delete_track(int index, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    if (!submit(RemoveTrack{t->id}, why)) return false;
    // The track that took its row, or the one above at the bottom; the
    // insertion point stays where it was.
    const int n = static_cast<int>(snapshot().edit->tracks.size());
    sel_.region = kNoId;
    sel_.start = sel_.end = sel_.insertion;
    sel_.tracks = 1;
    sel_.track = n > 0 ? std::min(index, n - 1) : -1;
    return true;
}

bool EditController::move_track(int index, int delta, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    const int to = index + delta;
    if (track(to) == nullptr) {
        why = "\"" + t->name + "\" is already the " + (delta < 0 ? "top" : "bottom") + " track";
        return false;
    }
    std::vector<Id> order = track_order(*snapshot().edit);
    std::swap(order[static_cast<std::size_t>(index)], order[static_cast<std::size_t>(to)]);
    const bool selected = sel_.track == index;
    if (!submit(ReorderTracks{std::move(order)}, why)) return false;
    if (selected) sel_.track = to;
    return true;
}

namespace {

// A track's playlists of its kind, as (id, name), in their order.
std::vector<std::pair<Id, std::string>> playlists_of(const TrackEdit& t) {
    std::vector<std::pair<Id, std::string>> out;
    for (const auto& p : t.playlists) out.emplace_back(p->id, p->name);
    for (const auto& p : t.midi_playlists) out.emplace_back(p->id, p->name);
    return out;
}

}  // namespace

std::vector<std::string> EditController::playlist_names(int index) const {
    std::vector<std::string> out;
    if (const TrackEdit* t = track(index)) {
        for (auto& [id, name] : playlists_of(*t)) out.push_back(std::move(name));
    }
    return out;
}

std::string EditController::active_playlist_name(int index) const {
    const TrackEdit* t = track(index);
    if (t == nullptr) return {};
    for (auto& [id, name] : playlists_of(*t))
        if (id == t->active_playlist) return name;
    return {};
}

bool EditController::new_playlist(int index, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    const Id id = doc_.reserve_id();
    const Id on = t->id;
    std::vector<Member> members;
    members.push_back(AddPlaylist{on, id, next_playlist_name(snapshot(), on)});
    members.push_back(SetActivePlaylist{on, id});
    if (!submit(Compound{std::move(members), "New Playlist"}, why)) return false;
    if (sel_.track == index) sel_.region = kNoId;
    return true;
}

bool EditController::duplicate_playlist(int index, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    DuplicatePlaylist d;
    d.track = t->id;
    d.source = t->active_playlist;
    d.name = next_playlist_name(snapshot(), t->id);
    const std::size_t ids = duplicate_playlist_ids(snapshot(), d);
    // The copy's regions take the ids after the copy's, one by one.
    d.playlist = doc_.reserve_id();
    for (std::size_t i = 1; i < ids; ++i) doc_.reserve_id();
    const Id copy = d.playlist;
    const Id on = d.track;
    std::vector<Member> members;
    members.push_back(std::move(d));
    members.push_back(SetActivePlaylist{on, copy});
    if (!submit(Compound{std::move(members), "Duplicate Playlist"}, why)) return false;
    if (sel_.track == index) sel_.region = kNoId;
    return true;
}

bool EditController::choose_playlist(int index, const std::string& name, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    for (const auto& [id, n] : playlists_of(*t)) {
        if (n != name) continue;
        if (id == t->active_playlist) return true;
        if (!submit(SetActivePlaylist{t->id, id}, why)) return false;
        if (sel_.track == index) sel_.region = kNoId;
        return true;
    }
    why = "\"" + t->name + "\" has no playlist named \"" + name + "\"";
    return false;
}

bool EditController::rename_playlist(int index, const std::string& name, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    if (name.empty()) {
        why = "a playlist needs a name";
        return false;
    }
    if (name == active_playlist_name(index)) return true;
    if (playlist_name_taken(snapshot(), name, t->active_playlist)) {
        why = "another playlist is already named \"" + name + "\"";
        return false;
    }
    return submit(SetPlaylistName{t->id, t->active_playlist, name}, why);
}

bool EditController::delete_unused_playlists(int index, std::string& why) {
    const TrackEdit* t = track(index);
    if (t == nullptr) {
        why = no_track(index);
        return false;
    }
    std::vector<Member> members;
    for (const auto& [id, n] : playlists_of(*t)) {
        if (id != t->active_playlist) members.push_back(RemovePlaylist{t->id, id});
    }
    if (members.empty()) {
        why = "\"" + t->name + "\" has no unused playlists";
        return false;
    }
    // Past the compound limit, the rest go on the next press.
    if (members.size() > kMaxCompoundMembers) members.resize(kMaxCompoundMembers);
    return submit(Compound{std::move(members), "Delete Unused Playlists"}, why);
}

bool EditController::set_bus_gain(float db, std::string& why) {
    MonitorBus bus = snapshot().mix.bus;
    if (!fader_gain(db, bus.gain_db, why)) return false;
    return submit(SetMonitorBus{bus}, why);
}

}  // namespace sonora::ui
