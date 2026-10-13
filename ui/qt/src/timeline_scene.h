// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the timeline and the rulers show, as rectangles and labels in item
// coordinates, separated from how the scene graph draws them -- the same
// split as meter_model / MeterItem. No Qt: every pixel's colour can be
// tested by asking which rectangle covers it.
//
// Two layers, because they change at different rates. The content (lanes,
// grid, regions, waveforms, selection) changes when the operator edits,
// zooms or scrolls. The overlay (playhead, insertion cursor) moves 60 times
// a second while the transport rolls, and costs a handful of vertices.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edit_controller.h"
#include "peak_source.h"
#include "time_format.h"

namespace sonora::ui {

struct SceneRect {
    float x = 0, y = 0, w = 0, h = 0;
    uint32_t argb = 0;  // straight alpha; the item premultiplies
};

struct SceneLabel {
    float x = 0, y = 0;  // top left
    float max_w = 0;     // elided to this width
    std::string text;
    uint32_t argb = 0;
    int px = 10;         // pixel size
};

struct Scene {
    std::vector<SceneRect> rects;
    std::vector<SceneLabel> labels;
    void clear() {
        rects.clear();
        labels.clear();
    }
};

// The track lanes and everything on them, for a timeline of view.width x
// view.height. `drag` previews a gesture in progress.
void build_timeline_scene(const model::Snapshot& s, const ViewState& view, const Selection& sel,
                          const Drag& drag, bool grid_lines, GridValue grid,
                          const PeakSource& peaks, Scene& out);

struct TransportOverlay {
    model::Frames playhead = 0;
    bool rolling = false;    // playing or recording: the playhead is drawn
    bool recording = false;  // and drawn in the record colour
};

// The insertion cursor while stopped, the playhead while rolling.
void build_timeline_overlay(const ViewState& view, const Selection& sel,
                            const TransportOverlay& t, std::vector<SceneRect>& out);

// One ruler strip, `height` pixels tall, for the same view.
void build_ruler_scene(TimeFormat fmt, const model::SessionSettings& s, const ViewState& view,
                       double height, Scene& out);

// The topmost rectangle containing (x, y), or nullptr. For tests.
const SceneRect* rect_at(const std::vector<SceneRect>& rects, float x, float y);

}  // namespace sonora::ui
