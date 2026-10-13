// SPDX-License-Identifier: GPL-3.0-or-later
//
// Timeline positions as an engineer reads them: timecode from the session's
// origin (01:00:00:00 by default, session-format.md), bars|beats from the
// tempo map, minutes:seconds, samples. Also where the ruler ticks and the
// grid lines go. No Qt; every format is a pure function and is tested.
//
// Positions are frames from the timeline's sample 0, as everywhere in the
// model. Audio is anchored in samples (session-format.md, "tempo"): bars and
// beats are a reading of a position, never a position.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "model_access.h"

namespace sonora::ui {

enum class TimeFormat { Timecode, BarsBeats, MinSec, Samples };

// "01:00:12:05" (non-drop) or "01:00:12;05" (29.97df). Uses the origin.
std::string format_timecode(model::Frames f, const model::SessionSettings& s);
// "5|3|480": bar, beat, ticks at 960 per beat, counting from 1|1|000.
std::string format_bars_beats(model::Frames f, const model::SessionSettings& s);
// "1:23.456"; hours appear when needed ("1:02:03.000").
std::string format_min_sec(model::Frames f, uint32_t rate);
std::string format_samples(model::Frames f);
std::string format_time(TimeFormat fmt, model::Frames f, const model::SessionSettings& s);
const char* time_format_name(TimeFormat fmt);

// Frames per beat and per bar at the session's (single, Phase 2) tempo.
double frames_per_beat(const model::SessionSettings& s);
double frames_per_bar(const model::SessionSettings& s);

// --- grid ---
enum class GridValue { Bar, Beat, Eighth, Sixteenth };
const char* grid_value_name(GridValue g);
GridValue next_grid_value(GridValue g);
double grid_frames(GridValue g, const model::SessionSettings& s);
// The grid line nearest `f`, never below 0.
model::Frames snap_to_grid(model::Frames f, GridValue g, const model::SessionSettings& s);

// --- ruler ---
struct RulerTick {
    double x = 0;       // pixels from the view's left edge
    bool major = false;  // major ticks carry a label
    std::string label;
};

// The ticks of one ruler for the view [first, first + width * fpp) frames,
// fpp frames per pixel. Majors are at least `min_major_px` apart, chosen
// from intervals an engineer expects (whole seconds, whole bars, ...).
void ruler_ticks(TimeFormat fmt, double first, double fpp, double width,
                 const model::SessionSettings& s, std::vector<RulerTick>& out,
                 double min_major_px = 90.0);

}  // namespace sonora::ui
