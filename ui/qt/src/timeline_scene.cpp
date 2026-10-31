// SPDX-License-Identifier: GPL-3.0-or-later
#include "timeline_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "theme.h"

namespace sonora::ui {

using namespace model;

namespace {

constexpr float kLaneInset = 2.0f;  // between a region and its lane's edges

void add(std::vector<SceneRect>& out, double x, double y, double w, double h, uint32_t argb) {
    if (w <= 0 || h <= 0) return;
    out.push_back({static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                   static_cast<float>(h), argb});
}

void outline(std::vector<SceneRect>& out, double x, double y, double w, double h, double t,
             uint32_t argb) {
    add(out, x, y, w, t, argb);
    add(out, x, y + h - t, w, t, argb);
    add(out, x, y, t, h, argb);
    add(out, x + w - t, y, t, h, argb);
}

uint32_t with_alpha(uint32_t argb, uint8_t a) { return (argb & 0x00ffffffu) | (uint32_t(a) << 24); }

// The area a region's waveform is drawn in, under its name band.
void wave_area(double lane_y, const ViewState& v, double& wy, double& wh) {
    const double y = lane_y + kLaneInset;
    const double h = v.track_height - 2 * kLaneInset;
    const double band = std::min<double>(theme::kRegionNameBand, h / 3);
    wy = y + band + 1;
    wh = h - band - 2;
}

// A fade's gain curve over pixels [fx0, fx1), clipped to [cx0, cx1): the
// curve the engine plays (fade_gain), one column a pixel, as a line, with
// `shade` (when non-zero) over what the fade takes away above it.
void draw_fade_curve(double fx0, double fx1, bool rising, FadeShape shape, double cx0,
                     double cx1, double wy, double wh, uint32_t line, uint32_t shade,
                     std::vector<SceneRect>& out) {
    if (fx1 - fx0 < 1.0 || wh <= 2) return;
    const int first = static_cast<int>(std::floor(std::max(fx0, cx0)));
    const int last = static_cast<int>(std::ceil(std::min(fx1, cx1)));
    double prev = -1;
    for (int px = first; px < last; ++px) {
        const double x = (px + 0.5 - fx0) / (fx1 - fx0);
        const float g = fade_gain(shape, static_cast<float>(rising ? x : 1.0 - x));
        const double yc = wy + (1.0 - g) * wh;
        if (shade != 0) add(out, px, wy, 1.0, yc - wy, shade);
        const double from = prev < 0 ? yc : prev;
        add(out, px, std::min(from, yc) - 0.75, 1.0, std::max(1.5, std::abs(yc - from) + 1.5),
            line);
        prev = yc;
    }
}

// One region at [pos, pos + len) reading its source from `offset`, drawn on
// lane `lane_y`, with the fades `fades` says are heard. `ghost` draws it
// translucent (a drag's origin).
void draw_region(const Region& r, Frames pos, Frames len, Frames offset, const RegionFades& fades,
                 double lane_y, const ViewState& v, uint32_t palette, bool selected, bool ghost,
                 const PeakSource& peaks, Scene& out) {
    const double x0 = v.x_of(pos), x1 = v.x_of(pos + len);
    if (x1 < 0 || x0 > v.width) return;
    const double cx0 = std::max(0.0, x0), cx1 = std::min(v.width, x1);
    const double y = lane_y + kLaneInset;
    const double h = v.track_height - 2 * kLaneInset;
    const uint8_t alpha = ghost ? 90 : 255;

    const uint32_t body = r.mute       ? theme::region_muted(palette)
                          : selected   ? theme::region_body_selected(palette)
                                       : theme::region_body(palette);
    add(out.rects, cx0, y, cx1 - cx0, h, with_alpha(body, alpha));
    const double band = std::min<double>(theme::kRegionNameBand, h / 3);
    add(out.rects, cx0, y, cx1 - cx0, band, with_alpha(theme::region_name_band(palette), alpha));

    // The overview: one min/max column per pixel, centred in the area under
    // the name band, at the operator's vertical zoom.
    const double wy = y + band + 1, wh = h - band - 2;
    const int first_px = static_cast<int>(std::floor(cx0));
    const int count = static_cast<int>(std::ceil(cx1)) - first_px;
    if (count > 0 && wh > 2 && !r.channels.empty()) {
        static thread_local std::vector<PeakPair> buf;
        buf.resize(static_cast<std::size_t>(count));
        const double src_first = static_cast<double>(offset) + (first_px - x0) * v.fpp;
        peaks.peaks(r.channels[0].source, r.channels[0].channel,
                    static_cast<Frames>(std::llround(src_first)), v.fpp, count, buf.data());
        const double mid = wy + wh / 2, half = wh / 2;
        const uint32_t wave = with_alpha(r.mute ? theme::kTextTertiary : theme::region_wave(palette), alpha);
        for (int i = 0; i < count; ++i) {
            const double px = first_px + i;
            if (px < cx0 - 0.5 || px >= cx1) continue;
            const double top = std::max(wy, mid - std::min(1.0, static_cast<double>(buf[i].max) * v.wave_scale) * half);
            const double bot = std::min(wy + wh, mid - std::max(-1.0, static_cast<double>(buf[i].min) * v.wave_scale) * half);
            add(out.rects, px, top, 1.0, std::max(1.0, bot - top), wave);
        }
    }
    // A trim in the hand can leave fades longer than the region; they are
    // drawn as they would fit.
    const Frames in = std::clamp<Frames>(fades.in, 0, len);
    const Frames fade_out = std::clamp<Frames>(fades.out, 0, len - in);
    if (!r.mute && wh > 2) {
        const uint32_t line = with_alpha(theme::kFadeLine, alpha);
        const uint32_t shade = ghost ? 0u : theme::kFadeShade;
        if (in > 0) {
            draw_fade_curve(x0, v.x_of(pos + in), true, fades.in_shape, cx0, cx1, wy, wh, line,
                            shade, out.rects);
        }
        if (fade_out > 0) {
            draw_fade_curve(v.x_of(pos + len - fade_out), x1, false, fades.out_shape, cx0, cx1, wy,
                            wh, line, shade, out.rects);
        }
    }
    if (selected && !ghost) outline(out.rects, cx0, y, cx1 - cx0, h, 1.5, theme::kRegionSelectedOutline);
    if (!ghost && x1 - x0 > 24) {
        out.labels.push_back({static_cast<float>(cx0 + 4), static_cast<float>(y),
                              static_cast<float>(cx1 - cx0 - 8), r.name, theme::kText,
                              theme::kTextCaption});
    }
}

// Each crossfade on `p` as the engine plays it: over [cut - before, cut +
// after), the left region falling and the right one rising, in a box that
// marks the span both sound in.
void draw_crossfades(const Playlist& p, double lane_y, const ViewState& v, Scene& out) {
    double wy = 0, wh = 0;
    wave_area(lane_y, v, wy, wh);
    const double y = lane_y + kLaneInset;
    const double h = v.track_height - 2 * kLaneInset;
    for (const Crossfade& x : p.crossfades) {
        Frames cut = -1;
        for (const Region& r : p.regions) {
            if (r.id == x.right) cut = r.position;
        }
        if (cut < 0) continue;
        Frames before = 0, after = 0;
        crossfade_extent(x, before, after);
        const double fx0 = v.x_of(cut - before), fx1 = v.x_of(cut + after);
        if (fx1 < 0 || fx0 > v.width) continue;
        const double cx0 = std::max(0.0, fx0), cx1 = std::min(v.width, fx1);
        add(out.rects, cx0, y, cx1 - cx0, h, theme::kCrossfadeFill);
        outline(out.rects, cx0, y, cx1 - cx0, h, 1.0, theme::kCrossfadeEdge);
        draw_fade_curve(fx0, fx1, false, x.shape, cx0, cx1, wy, wh, theme::kFadeLine, 0, out.rects);
        draw_fade_curve(fx0, fx1, true, x.shape, cx0, cx1, wy, wh, theme::kFadeLine, 0, out.rects);
    }
}

// The points lane `l` of track `i` is drawn from: the lane's own, with the
// gesture in the hand applied, so a breakpoint is seen where the release will
// leave it before the button comes up. `dragged` is the index of the point
// in the hand, -1 for none.
std::vector<AutomationPoint> lane_points_as_drawn(const TrackEdit& t, int i, int l,
                                                  const Drag& drag, int& dragged) {
    const AutomationParam p = lane_param(l);
    const AutomationLane* lane = t.lane(p);
    std::vector<AutomationPoint> pts = lane ? lane->points : std::vector<AutomationPoint>{};
    dragged = -1;
    if (drag.track != i || drag.autolane != l) return pts;
    if (drag.kind == Drag::Kind::Point) {
        if (drag.point >= 0) {
            if (static_cast<std::size_t>(drag.point) < pts.size()) {
                pts[static_cast<std::size_t>(drag.point)] = drag.at_point;
                dragged = drag.point;
            }
        } else {
            pts.erase(std::remove_if(pts.begin(), pts.end(),
                                     [&](const AutomationPoint& q) { return q.at == drag.at_point.at; }),
                      pts.end());
            const auto at = std::lower_bound(
                pts.begin(), pts.end(), drag.at_point.at,
                [](const AutomationPoint& q, Frames f) { return q.at < f; });
            dragged = static_cast<int>(at - pts.begin());
            pts.insert(at, drag.at_point);
        }
    } else if (drag.kind == Drag::Kind::Stroke) {
        EditAutomation op;
        if (stroke_op(t.id, p, drag.stroke, op)) {
            std::vector<AutomationPoint> merged;
            for (const AutomationPoint& q : pts) {
                if (q.at < op.from) merged.push_back(q);
            }
            merged.insert(merged.end(), op.points.begin(), op.points.end());
            for (const AutomationPoint& q : pts) {
                if (q.at >= op.to) merged.push_back(q);
            }
            pts = std::move(merged);
        }
    }
    return pts;
}

// One lane of automation: the curve the engine plays (lane_value_at, one
// column a pixel), its breakpoints, the range selected on it and the value
// of the breakpoint in the hand. A lane with no breakpoint draws the static
// fader or pan as a dashed line, because that is what plays: a flat line
// drawn solid would look like automation that is not there.
void draw_autolane(const Snapshot& s, const TrackEdit& t, int i, int l, const ViewState& v,
                   const Selection& sel, const Drag& drag, Scene& out) {
    const AutomationParam p = lane_param(l);
    const double top = v.y_of_autolane(i, l);
    const TrackMix* mix = s.mix.find(t.id);
    const bool off = mix != nullptr && mix->automation == AutomationMode::Off;
    const uint32_t colour =
        off ? theme::kTextTertiary
            : (p == AutomationParam::Volume ? theme::kAutomationVolume : theme::kAutomationPan);
    const double h = v.autolane_height;

    const float unity = p == AutomationParam::Volume ? lane_position(p, 0.0f) : 0.5f;
    add(out.rects, 0, std::floor(v.autolane_y(i, l, unity)), v.width, 1, theme::kAutomationGuide);

    Frames ra = sel.start, rb = sel.end;
    bool ranged = sel.autolane == l && sel.track == i && sel.has_range();
    if (drag.kind == Drag::Kind::Range && drag.autolane == l && drag.track == i && drag.moved) {
        ra = drag.start;
        rb = drag.end;
        ranged = rb > ra;
    }
    if (ranged) {
        const double x0 = std::max(0.0, v.x_of(ra));
        const double x1 = std::min(v.width, v.x_of(rb));
        add(out.rects, x0, top, x1 - x0, h - 1, theme::kSelectionFill);
    }

    int dragged = -1;
    const std::vector<AutomationPoint> pts = lane_points_as_drawn(t, i, l, drag, dragged);
    const float fallback = mix == nullptr                    ? 0.0f
                           : p == AutomationParam::Volume ? mix->gain_db
                                                          : mix->pan;
    if (pts.empty()) {
        const double y = v.autolane_y(i, l, lane_position(p, fallback));
        for (double px = 0; px < v.width; px += 9) add(out.rects, px, y - 0.75, 5, 1.5, colour);
        return;
    }
    AutomationLane drawn;
    drawn.param = p;
    drawn.points = pts;
    double prev = -1;
    for (int px = 0; px < static_cast<int>(v.width); ++px) {
        const Frames f = static_cast<Frames>(std::llround(v.scroll + (px + 0.5) * v.fpp));
        const double y = v.autolane_y(i, l, lane_position(p, lane_value_at(&drawn, f, fallback)));
        const double from = prev < 0 ? y : prev;
        add(out.rects, px, std::min(from, y) - 0.75, 1.0, std::max(1.5, std::abs(y - from) + 1.5),
            colour);
        prev = y;
    }
    const bool lane_selected = sel.autolane == l && sel.track == i;
    for (std::size_t k = 0; k < pts.size(); ++k) {
        const double x = v.x_of(pts[k].at);
        if (x < -6 || x > v.width + 6) continue;
        const double y = v.autolane_y(i, l, lane_position(p, pts[k].value));
        const bool picked = (drag.track == i && drag.autolane == l && dragged == static_cast<int>(k)) ||
                            (lane_selected && dragged < 0 && sel.point == static_cast<int>(k));
        const double r = picked ? 4.5 : 3.5;
        if (picked) add(out.rects, x - r - 1.5, y - r - 1.5, 2 * r + 3, 2 * r + 3, theme::kAccent);
        add(out.rects, x - r, y - r, 2 * r, 2 * r, picked ? theme::kPlayhead : colour);
        if (picked) {
            out.labels.push_back({static_cast<float>(x + 9), static_cast<float>(y - 16), 90.0f,
                                  lane_value_text(p, pts[k].value), theme::kText,
                                  theme::kTextCaption});
        }
    }
}

}  // namespace

void build_timeline_scene(const Snapshot& s, const ViewState& v, const Selection& sel,
                          const Drag& drag, bool grid_lines, GridValue grid,
                          const PeakSource& peaks, Scene& out) {
    out.clear();
    if (v.width <= 0 || v.height <= 0) return;
    const auto& tracks = s.edit->tracks;

    // Lanes.
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const double y = v.y_of_track(static_cast<int>(i));
        if (v.y_of_track(static_cast<int>(i) + 1) < 0 || y > v.height) continue;
        add(out.rects, 0, y, v.width, v.track_height, i % 2 ? theme::kLaneB : theme::kLaneA);
        add(out.rects, 0, y + v.track_height - 1, v.width, 1, theme::kBorderSubtle);
        if (!v.autolanes_open(static_cast<int>(i))) continue;
        for (int l = 0; l < kLaneCount; ++l) {
            const double top = v.y_of_autolane(static_cast<int>(i), l);
            add(out.rects, 0, top, v.width, v.autolane_height, theme::kSunken);
            add(out.rects, 0, top + v.autolane_height - 1, v.width, 1, theme::kBorderSubtle);
        }
    }

    // Grid: bar lines always, faintly; the grid value's lines in Grid mode.
    // Both follow the tempo map, as the Bars|Beats ruler above them does.
    {
        static thread_local std::vector<GridLine> lines;
        const double a = v.scroll, b = v.scroll + v.width * v.fpp;
        sonora::ui::grid_lines(a, b, grid, !grid_lines, s.settings, lines);
        // Lines closer than 4 px are noise, not a grid. Where the grid
        // itself is that dense (every gap around a line is under 4 px), none
        // of it is drawn. A single short gap, such as a bar a meter change
        // cut to a sliver, drops only the line that crowds the one before it,
        // as the ruler does with its labels.
        const auto gap = [&](std::size_t i) {
            return (lines[i].frame - lines[i - 1].frame) / v.fpp;
        };
        const bool capped = lines.size() >= 4096;
        double drawn = -HUGE_VAL;
        for (std::size_t i = 0; !capped && i < lines.size(); ++i) {
            const bool tight_before = i > 0 && gap(i) < 4.0;
            const bool tight_after = i + 1 < lines.size() && gap(i + 1) < 4.0;
            const bool in_dense_run =
                (tight_before || i == 0) && (tight_after || i + 1 == lines.size()) &&
                (tight_before || tight_after);
            const double px = (lines[i].frame - v.scroll) / v.fpp;
            if (in_dense_run || px - drawn < 4.0) continue;
            drawn = px;
            const GridLine& l = lines[i];
            const uint32_t c = grid_lines ? (l.bar ? theme::kGridBar : theme::kGridBeat)
                                          : theme::kGridBeat;
            add(out.rects, std::floor((l.frame - v.scroll) / v.fpp), 0, 1, v.height, c);
        }
    }

    // Regions.
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const double y = v.y_of_track(static_cast<int>(i));
        if (y + v.track_height < 0 || y > v.height) continue;
        const Playlist* p = active_playlist(tracks[i].get());
        if (p == nullptr) continue;
        const uint32_t palette = theme::region_colour(static_cast<int>(i));
        // A trim in the hand: the playlist as the drop will leave it, so a
        // neighbour the edge runs into is seen to be overwritten before the
        // button comes up.
        if (drag.moved && drag.preview && drag.preview->id == p->id) {
            for (const Region& r : drag.preview->regions) {
                draw_region(r, r.position, r.length, r.source_offset,
                            region_fades(*drag.preview, r), y, v, palette, r.id == drag.region,
                            false, peaks, out);
            }
            draw_crossfades(*drag.preview, y, v, out);
            continue;
        }
        for (const Region& r : p->regions) {
            const bool dragged = drag.moved && drag.region == r.id;
            const bool selected = sel.region == r.id;
            RegionFades fades = region_fades(*p, r);
            if (!dragged) {
                draw_region(r, r.position, r.length, r.source_offset, fades, y, v, palette,
                            selected, false, peaks, out);
                continue;
            }
            switch (drag.kind) {
                case Drag::Kind::Move:
                    draw_region(r, r.position, r.length, r.source_offset, fades, y, v, palette,
                                false, true, peaks, out);
                    draw_region(r, drag.start, r.length, r.source_offset, fades, y, v, palette,
                                true, false, peaks, out);
                    break;
                case Drag::Kind::FadeIn:
                case Drag::Kind::FadeOut:
                    // The fade in the hand, as the drop will set it.
                    (drag.kind == Drag::Kind::FadeIn ? fades.in : fades.out) = drag.fade;
                    draw_region(r, r.position, r.length, r.source_offset, fades, y, v, palette,
                                true, false, peaks, out);
                    break;
                default:
                    draw_region(r, drag.start, drag.end - drag.start,
                                r.source_offset + (drag.start - r.position), fades, y, v, palette,
                                true, false, peaks, out);
                    break;
            }
        }
        draw_crossfades(*p, y, v, out);
    }

    // Automation lanes, over the grid and under the range.
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        if (!v.autolanes_open(static_cast<int>(i))) continue;
        const double y = v.y_of_track(static_cast<int>(i));
        if (v.y_of_track(static_cast<int>(i) + 1) < 0 || y > v.height) continue;
        for (int l = 0; l < kLaneCount; ++l) {
            draw_autolane(s, *tracks[i], static_cast<int>(i), l, v, sel, drag, out);
        }
    }

    // A time range on the selected tracks, or the one being dragged out.
    Frames ra = sel.start, rb = sel.end;
    int rt = sel.track, rn = sel.tracks;
    if (drag.kind == Drag::Kind::Range && drag.moved) {
        ra = drag.start;
        rb = drag.end;
        rt = std::min(drag.track, drag.track_end);
        rn = std::abs(drag.track_end - drag.track) + 1;
    }
    // A range on an automation lane is drawn on the lane, not across the row.
    const bool on_lane = drag.kind == Drag::Kind::Range && drag.moved ? drag.autolane >= 0
                                                                     : sel.autolane >= 0;
    if (rb > ra && rt >= 0 && sel.region == kNoId && !on_lane) {
        const double x0 = std::max(0.0, v.x_of(ra)), x1 = std::min(v.width, v.x_of(rb));
        // One band over all of them: the range is one selection, not one per
        // track.
        add(out.rects, x0, v.y_of_track(rt), x1 - x0, v.y_of_track(rt + rn) - v.y_of_track(rt),
            theme::kSelectionFill);
    }
}

void build_timeline_overlay(const ViewState& v, const Selection& sel, const TransportOverlay& t,
                            std::vector<SceneRect>& out) {
    out.clear();
    if (v.width <= 0 || v.height <= 0) return;
    if (t.rolling) {
        const double x = std::floor(v.x_of(t.playhead));
        if (x >= -1 && x <= v.width) {
            add(out, x - 0.5, 0, 2, v.height, t.recording ? theme::kRecord : theme::kPlayhead);
        }
        return;
    }
    const double x = std::floor(v.x_of(sel.insertion));
    if (x >= 0 && x <= v.width) add(out, x, 0, 1, v.height, theme::kAccent);
}

void build_ruler_scene(TimeFormat fmt, const model::SessionSettings& s, const ViewState& v,
                       double height, Scene& out) {
    out.clear();
    if (v.width <= 0 || height <= 0) return;
    add(out.rects, 0, 0, v.width, height, theme::kPanel);
    add(out.rects, 0, height - 1, v.width, 1, theme::kBorderSubtle);
    static thread_local std::vector<RulerTick> ticks;
    ruler_ticks(fmt, v.scroll, v.fpp, v.width, s, ticks);
    for (const RulerTick& t : ticks) {
        const double x = std::floor(t.x);
        if (t.major) {
            add(out.rects, x, height * 0.35, 1, height * 0.65, theme::kRulerTick);
            out.labels.push_back({static_cast<float>(x + 4), 2.0f, 120.0f, t.label,
                                  theme::kTextSecondary, theme::kTextCaption});
        } else {
            add(out.rects, x, height * 0.7, 1, height * 0.3, theme::kRulerTick);
        }
    }
}

void build_marker_ruler_scene(const model::MarkerList& markers, const ViewState& v, double height,
                              Scene& out) {
    out.clear();
    if (v.width <= 0 || height <= 0) return;
    add(out.rects, 0, 0, v.width, height, theme::kPanel);
    add(out.rects, 0, height - 1, v.width, 1, theme::kBorderSubtle);
    // Selection markers' bars first, under every flag.
    for (const Marker& m : markers.markers) {
        if (m.length <= 0) continue;
        const double x0 = std::max(0.0, std::floor(v.x_of(m.position)));
        const double x1 = std::min(v.width, std::floor(v.x_of(m.position + m.length)));
        add(out.rects, x0, height * 0.55, x1 - x0, height * 0.45 - 1, theme::kMarkerRange);
    }
    const double flag_h = std::floor(height * 0.5);
    for (std::size_t i = 0; i < markers.markers.size(); ++i) {
        const Marker& m = markers.markers[i];
        const double x = std::floor(v.x_of(m.position));
        if (x > v.width) break;   // sorted by position: the rest are further right
        add(out.rects, x, 1, 1, height - 2, theme::kMarker);
        add(out.rects, x, 1, theme::kMarkerFlagWidth, flag_h, theme::kMarker);
        // The name runs to the next marker's flag, so names never overlap.
        const double lx = x + theme::kMarkerFlagWidth + 3;
        double room = 160;
        if (i + 1 < markers.markers.size()) {
            room = std::floor(v.x_of(markers.markers[i + 1].position)) - lx - 4;
        }
        room = std::min(room, v.width - lx);
        if (room >= 12 && lx + room > 0) {
            out.labels.push_back({static_cast<float>(lx), 3.0f, static_cast<float>(room), m.name,
                                  theme::kText, theme::kTextCaption});
        }
    }
}

void build_conductor_ruler_scene(ConductorRuler kind, const model::SessionSettings& s,
                                 const ViewState& v, double height, Scene& out) {
    out.clear();
    if (v.width <= 0 || height <= 0) return;
    add(out.rects, 0, 0, v.width, height, theme::kPanel);
    add(out.rects, 0, height - 1, v.width, 1, theme::kBorderSubtle);

    struct Change {
        Frames at;
        std::string label;
    };
    static thread_local std::vector<Change> changes;
    changes.clear();
    const std::vector<TempoEntry> fallback{TempoEntry{}};
    const std::vector<TempoEntry>& map = s.tempo.empty() ? fallback : s.tempo;
    Frames start = 0;
    for (std::size_t i = 0; i < map.size(); ++i) {
        const TempoEntry& e = map[i];
        if (i > 0 && e.at <= start) continue;
        const Frames at = i == 0 ? 0 : e.at;
        start = at;
        std::string label = kind == ConductorRuler::Tempo ? format_bpm(e.bpm) : format_meter(e);
        if (!changes.empty() && changes.back().label == label) continue;
        changes.push_back({at, std::move(label)});
    }

    const double label_y = std::floor((height - theme::kTextCaption) / 2) - 1;
    for (std::size_t i = 0; i < changes.size(); ++i) {
        const double x = std::floor(v.x_of(changes[i].at));
        if (x > v.width) break;
        const double next = i + 1 < changes.size() ? std::floor(v.x_of(changes[i + 1].at))
                                                   : v.width + 1000;
        if (x < 0) {
            // In force at the left edge: pinned there, dimmer than a change.
            if (next < 60) continue;
            const double room = std::min(next - 4 - 6, v.width - 6);
            out.labels.push_back({6.0f, static_cast<float>(label_y), static_cast<float>(room),
                                  changes[i].label, theme::kTextSecondary, theme::kTextCaption});
            continue;
        }
        add(out.rects, x, 2, 1, height - 3, theme::kConductor);
        add(out.rects, x - 2, height - 6, 5, 4, theme::kConductor);
        const double lx = x + 5;
        const double room = std::min(next - lx - 4, v.width - lx);
        if (room >= 12) {
            out.labels.push_back({static_cast<float>(lx), static_cast<float>(label_y),
                                  static_cast<float>(room), changes[i].label, theme::kText,
                                  theme::kTextCaption});
        }
    }
}

const SceneRect* rect_at(const std::vector<SceneRect>& rects, float x, float y) {
    for (auto it = rects.rbegin(); it != rects.rend(); ++it) {
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return &*it;
    }
    return nullptr;
}

}  // namespace sonora::ui
