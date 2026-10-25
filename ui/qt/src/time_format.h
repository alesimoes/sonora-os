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
//
// Bars and beats are read through the whole tempo map. Each entry's tempo
// and meter hold from its `at` to the next entry's. A tempo change alone
// carries the count on, so a bar may straddle it; a meter change starts a
// new bar where it stands, and a bar it cuts short is still counted, as a
// bar. brain/specs/session-format.md ("tempo") states this rule for every
// reader of the map, the click included. (Whether Pro Tools and Logic
// always move a meter change to a bar line is unverified: Pro Tools' Meter
// Change dialog appears to have a snap-to-bar option that can be off. Our
// map holds frames either way, and a reading that moved the change would
// put the bar lines somewhere the session does not.)
//
// The reading itself is common/model's MusicalMap (ADR-0014 §2,
// brain/specs/midi.md §2.3), in integer ticks: the counters, the ruler, the
// grid and a MIDI note at 5|1|000 are one function. What is left here is
// formatting, and where to draw.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "model_access.h"
#include "sonora/model/musical_map.h"

namespace sonora::ui {

enum class TimeFormat { Timecode, BarsBeats, MinSec, Samples };

// "01:00:12:05" (non-drop) or "01:00:12;05" (29.97df). Uses the origin.
std::string format_timecode(model::Frames f, const model::SessionSettings& s);
// "5|3|480": bar, beat, ticks at 960 per beat, counting from 1|1|000.
std::string format_bars_beats(model::Frames f, const model::SessionSettings& s);

// A position read as the tempo map counts it: 1-based bar and beat, 960
// ticks per beat (the meter's denominator note).
struct MusicalTime {
    int64_t bar = 1;
    int beat = 1;
    int tick = 0;
    bool operator==(const MusicalTime& o) const {
        return bar == o.bar && beat == o.beat && tick == o.tick;
    }
};
MusicalTime musical_time(model::Frames f, const model::SessionSettings& s);
// Where `t` falls, to the nearest frame. Any beat or tick of a bar a meter
// change cut short, past the change, reads as the change's own frame; a
// position before 1|1|000 as 0.
model::Frames frame_of(const MusicalTime& t, const model::SessionSettings& s);
// format_bars_beats() of every tempo map entry's `at` (the first's as 0),
// from one map: the Transport window's list is n log n, where a map per
// entry would be n^2 on a long map.
std::vector<std::string> tempo_entry_positions(const model::SessionSettings& s);
// The tempo map entry in force at `f`, by index. An empty map (which the
// session format forbids) reads as the default entry.
std::size_t tempo_index_at(model::Frames f, const model::SessionSettings& s);
const model::TempoEntry& tempo_at(model::Frames f, const model::SessionSettings& s);
// "120.00", and "4/4".
std::string format_bpm(float bpm);
std::string format_meter(const model::TempoEntry& t);
// "1:23.456"; hours appear when needed ("1:02:03.000").
std::string format_min_sec(model::Frames f, uint32_t rate);
std::string format_samples(model::Frames f);
std::string format_time(TimeFormat fmt, model::Frames f, const model::SessionSettings& s);
const char* time_format_name(TimeFormat fmt);

// Frames per beat and per bar at the tempo map's FIRST entry. A fixed step
// for what needs one (a ruler's spacing); anything drawn, snapped or nudged
// against the timeline follows the whole map.
double frames_per_beat(const model::SessionSettings& s);
double frames_per_bar(const model::SessionSettings& s);

// --- grid ---
enum class GridValue { Bar, Beat, Eighth, Sixteenth };
const char* grid_value_name(GridValue g);
GridValue next_grid_value(GridValue g);
double grid_frames(GridValue g, const model::SessionSettings& s);
// Where one grid step from `f` lands, `direction` 1 or -1: the step is
// read where the move happens on the tempo map (at `f` going forward, just
// before it going back), so a beat nudge after a tempo change is that
// tempo's beat and a bar nudge across a meter change is the bar it
// crosses. The move is relative, as Pro Tools' nudge is: a region off the
// grid keeps its offset. Never below 0.
model::Frames nudge_target(model::Frames f, int direction, GridValue g,
                           const model::SessionSettings& s);
// The grid line nearest `f` on the tempo map, never below 0.
model::Frames snap_to_grid(model::Frames f, GridValue g, const model::SessionSettings& s);

struct GridLine {
    double frame = 0;  // the frame nearest the line
    bool bar = false;  // a bar line, not only a line of the grid value
    model::Ticks tick = 0;  // where the line is, exactly
};
// The lines in [a, b] frames, in order: every bar line, and with
// `bars_only` false every line of `g` too. At most `max` of them.
void grid_lines(double a, double b, GridValue g, bool bars_only,
                const model::SessionSettings& s, std::vector<GridLine>& out,
                std::size_t max = 4096);

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
