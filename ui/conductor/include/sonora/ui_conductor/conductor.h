// SPDX-License-Identifier: GPL-3.0-or-later
//
// Conductor-ruler gestures (ADR-0140): hit-testing the tempo marks, turning a
// vertical drag into a tempo, the meter choices of the meter menu. Pure
// functions over the model's tempo map; every change goes through
// tempo_edit.h, so the map stays one the format allows. No Qt, no I/O.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sonora/model/model.h"

namespace sonora::ui::conductor {

// A mark is grabbed within this many pixels of its line.
constexpr double kHitSlopPixels = 6.0;
// Tempo change per pixel of vertical drag, before snapping.
constexpr double kBpmPerPixel = 0.1;

// Index of the tempo entry whose mark is nearest `x` (pixels from frame 0 at
// `frames_per_pixel`), within kHitSlopPixels; none on a miss. Scrolling is
// the caller's: it passes x already relative to frame 0.
std::optional<std::size_t> hit_test(const std::vector<model::TempoEntry>& map,
                                    std::uint32_t sample_rate, double frames_per_pixel, double x);

struct TempoDrag {
    bool ok = false;
    float bpm = 0.0f;
    std::string why;  // set when !ok
};

// The tempo of entry `index` as it was when the drag began, moved by
// `dy_pixels` (positive = pointer up = faster). Clamped to the format's
// bounds, snapped to 0.01 bpm, or to whole bpm when `whole_bpm` (modifier).
TempoDrag drag_tempo(const std::vector<model::TempoEntry>& map, std::size_t index,
                     double dy_pixels, bool whole_bpm);

// drag_tempo, then tempo_set_at on that entry. `map` is left alone on false.
bool apply_tempo_drag(std::vector<model::TempoEntry>& map, std::uint32_t sample_rate,
                      std::size_t index, double dy_pixels, bool whole_bpm, std::string& why);

// Removes the change at the mark; the first entry cannot go.
bool remove_mark(std::vector<model::TempoEntry>& map, std::uint32_t sample_rate,
                 std::size_t index, std::string& why);

// A meter on the bar line nearest `near` (frames); `at` is where it went.
bool apply_meter(std::vector<model::TempoEntry>& map, std::uint32_t sample_rate,
                 model::Frames near, std::uint32_t beats, std::uint32_t note,
                 model::Frames& at, std::string& why);

struct MeterChoices {
    std::vector<std::uint32_t> beats;  // 1..32
    std::vector<std::uint32_t> notes;  // 1, 2, 4, 8, 16
};
MeterChoices meter_choices();

}  // namespace sonora::ui::conductor
