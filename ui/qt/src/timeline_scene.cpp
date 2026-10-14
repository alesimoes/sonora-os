// SPDX-License-Identifier: GPL-3.0-or-later
#include "timeline_scene.h"

#include <algorithm>
#include <cmath>

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
        if (y + v.track_height < 0 || y > v.height) continue;
        add(out.rects, 0, y, v.width, v.track_height, i % 2 ? theme::kLaneB : theme::kLaneA);
        add(out.rects, 0, y + v.track_height - 1, v.width, 1, theme::kBorderSubtle);
    }

    // Grid: bar lines always, faintly; the grid value's lines in Grid mode.
    // Both follow the tempo map, as the Bars|Beats ruler above them does.
    {
        static thread_local std::vector<GridLine> lines;
        const double a = v.scroll, b = v.scroll + v.width * v.fpp;
        sonora::ui::grid_lines(a, b, grid, !grid_lines, s.settings, lines);
        // Lines closer than 4 px are noise, not a grid: none are drawn.
        bool dense = false;
        for (std::size_t i = 1; i < lines.size() && !dense; ++i) {
            dense = (lines[i].frame - lines[i - 1].frame) / v.fpp < 4.0;
        }
        if (lines.size() >= 4096) dense = true;
        for (std::size_t i = 0; !dense && i < lines.size(); ++i) {
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

    // A time range on the selected track, or the one being dragged out.
    Frames ra = sel.start, rb = sel.end;
    int rt = sel.track;
    if (drag.kind == Drag::Kind::Range && drag.moved) {
        ra = drag.start;
        rb = drag.end;
        rt = drag.track;
    }
    if (rb > ra && rt >= 0 && sel.region == kNoId) {
        const double x0 = std::max(0.0, v.x_of(ra)), x1 = std::min(v.width, v.x_of(rb));
        add(out.rects, x0, v.y_of_track(rt), x1 - x0, v.track_height, theme::kSelectionFill);
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

const SceneRect* rect_at(const std::vector<SceneRect>& rects, float x, float y) {
    for (auto it = rects.rbegin(); it != rects.rend(); ++it) {
        if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return &*it;
    }
    return nullptr;
}

}  // namespace sonora::ui
