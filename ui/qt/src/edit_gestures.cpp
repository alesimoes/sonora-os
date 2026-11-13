// SPDX-License-Identifier: GPL-3.0-or-later
//
// The EditController's mouse: hit-testing, press, move and release.
#include "edit_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "edit_controller_detail.h"
#include "theme.h"

namespace sonora::ui {

using namespace model;
using namespace edit_detail;

Hit EditController::hit(double x, double y) const {
    refresh_rows();
    Hit h;
    const int i = view_.track_at(y);
    if (track(i) == nullptr || x < 0 || x > view_.width) return h;
    h.track = i;
    if (const int al = view_.autolane_at(i, y); al >= 0) {
        h.autolane = al;
        h.zone = HitZone::Automation;
        return h;
    }
    if (const int cl = view_.complane_at(i, y); cl >= 0) {
        h.lane = cl;
        h.zone = HitZone::TakeLane;
        const std::vector<Id> lanes = take_lane_ids(i);
        if (static_cast<std::size_t>(cl) < lanes.size()) {
            if (const Playlist* p = audio_playlist(*track(i), lanes[static_cast<std::size_t>(cl)])) {
                const Frames f = view_.frame_at(x);
                for (const Region& r : p->regions) {
                    if (f >= r.position && f < r.end()) h.region = r.id;
                }
            }
        }
        return h;
    }
    h.zone = HitZone::Lane;
    const Region* r = region_at(i, view_.frame_at(x));
    if (r == nullptr) return h;
    h.region = r->id;
    const double x0 = view_.x_of(r->position), x1 = view_.x_of(r->end());
    const double edge = std::min(kEdgePx, (x1 - x0) / 4);
    const double corner = std::min(kFadeZonePx, (x1 - x0) / 4);
    const bool top = y - view_.y_of_track(i) < view_.row_h(i) / 4;
    if (top && x - x0 <= corner) h.zone = HitZone::FadeIn;
    else if (top && x1 - x <= corner) h.zone = HitZone::FadeOut;
    else if (x - x0 <= edge) h.zone = HitZone::TrimStart;
    else if (x1 - x <= edge) h.zone = HitZone::TrimEnd;
    else if (y - view_.y_of_track(i) < view_.row_h(i) / 2) h.zone = HitZone::Select;
    else h.zone = HitZone::Body;
    return h;
}

bool EditController::press(double x, double y, std::string& why) {
    drag_ = Drag{};
    lane_sel_ = LaneSelection{};
    const Hit h = hit(x, y);
    if (h.track < 0) return true;
    select_track(h.track);
    sel_.autolane = sel_.point = -1;
    if (h.zone == HitZone::Automation) return press_autolane(h, x, y, why);
    if (h.zone == HitZone::TakeLane) return press_takelane(h, x, why);
    if (tool_ == Tool::Pencil && h.region != kNoId) return press_pencil(h, x, y, why);

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
        case Tool::Select:
        case Tool::Pencil: break;  // off a clip and off a lane the Pencil selects time
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
    if (kind == Drag::Kind::Move && mode_ == EditMode::Spot) {
        // Pro Tools' Spot mode: a click on a region asks where it starts,
        // rather than dragging it.
        const Region* r = find_region(snapshot(), h.region);
        sel_.region = r->id;
        sel_.start = r->position;
        sel_.end = r->end();
        sel_.insertion = r->position;
        spot_request_ = r->id;
        drag_ = Drag{};
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
    refresh_rows();
    if (drag_.kind == Drag::Kind::Pencil) {
        if (std::abs(x - drag_.press_x) >= kDragSlopPx || std::abs(y - drag_.press_y) >= kDragSlopPx) {
            drag_.moved = true;
        }
        drag_.pencil.push_back({x, pencil_level(drag_, y)});
        return;
    }
    const bool automation = drag_.kind == Drag::Kind::Point || drag_.kind == Drag::Kind::Stroke;
    if (std::abs(x - drag_.press_x) >= kDragSlopPx ||
        (automation && std::abs(y - drag_.press_y) >= kDragSlopPx)) {
        drag_.moved = true;
    }
    if (automation) {
        move_autolane(x, y);
        return;
    }
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
            // On an automation lane too: the range then covers the same
            // lane on each track it reaches.
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
        case Drag::Kind::Swipe: {
            const Frames f = snapped(view_.frame_at(x));
            drag_.start = std::min(drag_.press_frame, f);
            drag_.end = std::max(drag_.press_frame, f);
            break;
        }
        case Drag::Kind::Point:
        case Drag::Kind::Stroke:
        case Drag::Kind::Pencil:
        case Drag::Kind::None: break;
    }
}

bool EditController::release(double x, double y, std::string& why) {
    move(x, y);
    const Drag d = drag_;
    drag_ = Drag{};
    if (d.kind == Drag::Kind::Point || d.kind == Drag::Kind::Stroke) return release_autolane(d, why);
    if (d.kind == Drag::Kind::Swipe) return release_swipe(d, why);
    // A click without a move still draws: one sample, where it was put.
    if (d.kind == Drag::Kind::Pencil) return release_pencil(d, why);
    if (d.kind == Drag::Kind::None || !d.moved) return true;
    switch (d.kind) {
        case Drag::Kind::Range:
            select_span(std::min(d.track, d.track_end), std::abs(d.track_end - d.track) + 1,
                        d.start, d.end);
            sel_.autolane = d.autolane;
            return true;
        case Drag::Kind::Move: {
            MoveRegion op;
            if (!resolve_move(snapshot(), d.region, d.start, doc_.reserve_id(), op)) return true;
            if (!submit_move(op, why)) return false;
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
        case Drag::Kind::Swipe:
        case Drag::Kind::Point:
        case Drag::Kind::Stroke:
        case Drag::Kind::Pencil:
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

// --- the Pencil on a clip's waveform (ADR-0096) ---

SampleLane sample_lane(double row_top, double row_height, int count, int lane) {
    // As timeline_scene.cpp's draw_region lays a region out: a 2 px inset,
    // the name band, a pixel either side of the waveform.
    constexpr double kInset = 2.0;
    const double h = row_height - 2 * kInset;
    const double band = std::min<double>(theme::kRegionNameBand, h / 3);
    const double top = row_top + kInset + band + 1, height = std::max(0.0, h - band - 2);
    const int n = std::max(1, count);
    const double each = height / n;
    return SampleLane{top + each * std::clamp(lane, 0, n - 1), each};
}

bool EditController::sample_view_at(double x, double y) const {
    if (!sample_zoom()) return false;
    const Hit h = hit(x, y);
    return h.region != kNoId && h.zone != HitZone::TakeLane && h.zone != HitZone::Automation;
}

bool EditController::press_pencil(const Hit& h, double x, double y, std::string& why) {
    if (!sample_zoom()) {
        // The operator's next action, not a failure: the status line (§6).
        why = "zoom in to the samples to draw: the Pencil draws at 4 samples a pixel or closer";
        return false;
    }
    if (recording_) {
        why = "a take is recording; the Pencil draws once it stops. Nothing was drawn.";
        return false;
    }
    const Region* r = find_region(snapshot(), h.region);
    if (r == nullptr || r->channels.empty()) {
        why = "that clip has no audio to draw on";
        return false;
    }
    const int count = static_cast<int>(r->channels.size());
    const double row_top = view_.y_of_track(h.track);
    int lane = count - 1;
    for (int c = 0; c < count; ++c) {
        const SampleLane l = sample_lane(row_top, view_.row_h(h.track), count, c);
        if (y < l.top + l.height) {
            lane = c;
            break;
        }
    }
    if (samples_ != nullptr &&
        !samples_->ready(r->channels[static_cast<std::size_t>(lane)].source)) {
        // Zeros while loading are not the audio; a stroke over them would
        // replace samples the operator never saw.
        why = "the samples are still loading; draw once the waveform shows. Nothing was drawn.";
        return false;
    }
    drag_.kind = Drag::Kind::Pencil;
    drag_.track = drag_.track_end = h.track;
    drag_.region = r->id;
    drag_.channel = static_cast<uint16_t>(lane);
    drag_.press_x = x;
    drag_.press_y = y;
    drag_.press_frame = view_.frame_at(x);
    drag_.pencil.push_back({x, pencil_level(drag_, y)});
    return true;
}

float EditController::pencil_level(const Drag& d, double y) const {
    const Region* r = find_region(snapshot(), d.region);
    const int count = r != nullptr ? static_cast<int>(r->channels.size()) : 1;
    const SampleLane l =
        sample_lane(view_.y_of_track(d.track), view_.row_h(d.track), count, d.channel);
    const double half = l.height / 2;
    if (!(half > 0.0)) return 0.0f;
    // At the operator's vertical zoom, so the level under the pointer is the
    // level the line shows there; pencil_points clamps it to -1..1.
    const double scale = view_.wave_scale > 0.0f ? view_.wave_scale : 1.0;
    return static_cast<float>((l.mid() - y) / half / scale);
}

bool EditController::release_pencil(const Drag& d, std::string& why) {
    // A take may have started during the stroke: nothing goes to the engine.
    if (recording_) {
        why = "a take is recording; the Pencil draws once it stops. Nothing was drawn.";
        return false;
    }
    std::string argument;
    if (!pencil_argument(d, argument, why)) return false;
    if (!sample_sender_) {
        why = "no engine to edit the samples; nothing was drawn";
        return false;
    }
    // One stroke, one sample.edit, one undo step (ADR-0096 §4).
    sample_sender_(argument);
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

}  // namespace sonora::ui
