// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sample view on the timeline (ADR-0096 §3): at kSampleViewMaxFpp or
// closer a clip's waveform is the samples themselves, read from a
// SampleSource, a line per channel lane, and a dot per sample once there is
// room between them. Also the Pencil's stroke in the hand, drawn where the
// release will write it.
//
// Header-only on purpose: ui/qt's CMake files are held by other features
// (ADR-0096 §7). timeline_scene.cpp calls it in place of the overview.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "edit_controller.h"
#include "sample_view.h"
#include "theme.h"
#include "timeline_scene.h"

namespace sonora::ui {

// Whether the timeline draws samples rather than the overview at `v`. No
// source, no sample view: the overview stays, as it does for a test or a
// window with nothing to read the WAVs.
inline bool sample_view_on(const ViewState& v, const SampleSource* samples) {
    return samples != nullptr && v.fpp > 0.0 && v.fpp <= kSampleViewMaxFpp;
}

namespace sample_scene_detail {

inline void rect(std::vector<SceneRect>& out, double x, double y, double w, double h,
                 uint32_t argb) {
    if (w <= 0 || h <= 0) return;
    out.push_back({static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                   static_cast<float>(h), argb});
}

// A level as a y in `lane`, at the operator's vertical zoom: the inverse of
// EditController::pencil_level, so the Pencil writes the level under it.
inline double level_y(const SampleLane& lane, double level, float wave_scale) {
    const double scaled = std::clamp(level * static_cast<double>(wave_scale), -1.0, 1.0);
    return lane.mid() - scaled * lane.height / 2;
}

// A line from y0 to y1 in pixel column px, at least `thick` tall so a flat
// stretch still shows.
inline void segment(std::vector<SceneRect>& out, double px, double y0, double y1, double thick,
                    uint32_t argb) {
    const double top = std::min(y0, y1) - thick / 2;
    rect(out, px, top, 1.0, std::abs(y1 - y0) + thick, argb);
}

}  // namespace sample_scene_detail

// The samples of region `r`, its pixel x0 where its source frame `offset`
// plays, clipped to columns [cx0, cx1) of the row at `lane_y`. One lane per
// channel, as sample_lane splits the row, so the Pencil's lane is the one
// drawn. The samples are drawn as read: the clip's gain is not applied,
// because the Pencil writes the source and the level under it must be the
// level it writes.
inline void draw_sample_curve(const model::Region& r, model::Frames offset, double x0, double cx0,
                              double cx1, double lane_y, const ViewState& v, uint32_t colour,
                              const SampleSource& samples, Scene& out) {
    using namespace sample_scene_detail;
    const int lanes = static_cast<int>(r.channels.size());
    if (lanes == 0 || cx1 <= cx0) return;
    const int first_px = static_cast<int>(std::floor(cx0));
    const int last_px = static_cast<int>(std::ceil(cx1));
    // One frame either side, so the columns at the edges join their neighbours.
    const auto first = static_cast<model::Frames>(
                           std::floor(static_cast<double>(offset) + (first_px - x0) * v.fpp)) - 1;
    const auto last = static_cast<model::Frames>(
                          std::ceil(static_cast<double>(offset) + (last_px - x0) * v.fpp)) + 1;
    const int count = static_cast<int>(last - first + 1);
    if (count <= 0) return;
    static thread_local std::vector<float> buf;
    buf.resize(static_cast<std::size_t>(count));
    const auto at = [&](model::Frames f) {
        const model::Frames i = std::clamp<model::Frames>(f - first, 0, count - 1);
        return static_cast<double>(buf[static_cast<std::size_t>(i)]);
    };
    bool loading = false;
    for (int c = 0; c < lanes; ++c) {
        const SampleLane lane = sample_lane(lane_y, v.track_height, lanes, c);
        if (lane.height < 4.0) continue;
        // The zero line, faint: where a silenced stretch sits.
        rect(out.rects, cx0, std::floor(lane.mid()), cx1 - cx0, 1.0,
             (colour & 0x00ffffffu) | 0x50000000u);
        const model::SourceRef& ch = r.channels[static_cast<std::size_t>(c)];
        // Zeros while loading are not the audio: no line rather than a
        // silence that is not there (ADR-0096 §2).
        if (!samples.ready(ch.source)) {
            loading = true;
            continue;
        }
        samples.read(ch.source, ch.channel, first, count, buf.data());
        double prev = NAN;
        for (int px = first_px; px < last_px; ++px) {
            // The column covers source frames [a, a + fpp).
            const double a = static_cast<double>(offset) + (px - x0) * v.fpp;
            double lo, hi, end;
            if (v.fpp >= 1.0) {
                // Every sample in the column, so a peak between pixels shows.
                const auto ka = static_cast<model::Frames>(std::ceil(a));
                const auto kb = std::max<model::Frames>(
                    ka + 1, static_cast<model::Frames>(std::ceil(a + v.fpp)));
                lo = hi = end = level_y(lane, at(ka), v.wave_scale);
                for (model::Frames k = ka + 1; k < kb; ++k) {
                    end = level_y(lane, at(k), v.wave_scale);
                    lo = std::min(lo, end);
                    hi = std::max(hi, end);
                }
            } else {
                // Between samples the line is straight, as the engine's
                // Draw joins its points.
                const double t = a + v.fpp / 2;
                const auto k = static_cast<model::Frames>(std::floor(t));
                const double frac = t - static_cast<double>(k);
                lo = hi = end = level_y(lane, at(k) + (at(k + 1) - at(k)) * frac, v.wave_scale);
            }
            if (!std::isnan(prev)) {
                lo = std::min(lo, prev);
                hi = std::max(hi, prev);
            }
            if (px + 1 > cx0 && px < cx1) segment(out.rects, px, lo, hi, 1.5, colour);
            prev = end;
        }
        if (v.fpp > kSampleDotsMaxFpp) continue;
        // Four pixels a sample or more: each sample a dot, the thing the
        // Pencil moves.
        for (model::Frames f = first; f <= last; ++f) {
            const double x = x0 + static_cast<double>(f - offset) / v.fpp;
            if (x < cx0 || x >= cx1) continue;
            const double y = level_y(lane, at(f), v.wave_scale);
            rect(out.rects, x - 1.5, y - 1.5, 3.0, 3.0, colour);
        }
    }
    if (loading) {
        const SampleLane all = sample_lane(lane_y, v.track_height, 1, 0);
        out.labels.push_back({static_cast<float>(cx0 + 4), static_cast<float>(all.mid() - 7),
                              static_cast<float>(cx1 - cx0 - 8), "loading samples…",
                              theme::kTextSecondary, theme::kTextCaption});
    }
}

// The Pencil's stroke in the hand, on its clip's channel lane, as the
// release will write it: one point per column crossed, the columns between
// two positions joined, cut at the clip's edges (pencil_points).
inline void draw_pencil_preview(const model::Snapshot& s, const ViewState& v, const Drag& d,
                                Scene& out) {
    using namespace sample_scene_detail;
    if (d.kind != Drag::Kind::Pencil || d.pencil.empty()) return;
    const model::Region* r = find_region(s, d.region);
    if (r == nullptr || r->channels.empty()) return;
    const int lanes = static_cast<int>(r->channels.size());
    const SampleLane lane = sample_lane(v.y_of_track(d.track), v.track_height, lanes, d.channel);
    const double cx0 = std::max(0.0, v.x_of(r->position));
    const double cx1 = std::min(v.width, v.x_of(r->end()));
    double prev = NAN;
    const auto put = [&](long long column, double level) {
        const double y = level_y(lane, std::clamp(level, -1.0, 1.0), v.wave_scale);
        const double x = static_cast<double>(column);
        if (x >= cx0 && x < cx1) segment(out.rects, x, std::isnan(prev) ? y : prev, y, 2.5,
                                         theme::kAccent);
        prev = y;
    };
    for (std::size_t i = 0; i < d.pencil.size(); ++i) {
        const long long c1 = std::llround(d.pencil[i].x);
        const double l1 = d.pencil[i].level;
        if (i == 0) {
            put(c1, l1);
            continue;
        }
        const long long c0 = std::llround(d.pencil[i - 1].x);
        const double l0 = d.pencil[i - 1].level;
        const long long span = c1 - c0;
        if (span == 0) {
            put(c1, l1);
            continue;
        }
        const long long step = span > 0 ? 1 : -1;
        for (long long c = c0 + step;; c += step) {
            put(c, l0 + (l1 - l0) * static_cast<double>(c - c0) / static_cast<double>(span));
            if (c == c1) break;
        }
    }
}

}  // namespace sonora::ui
