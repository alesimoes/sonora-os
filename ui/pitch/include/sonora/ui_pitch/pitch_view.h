// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Pitch editor's picture, in pixels: a note blob per note over a piano
// ruler, one row per semitone, time across. Computed here so the panel only
// draws, and so a press is resolved to a note (hit_test) the same way in a
// test as on screen.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sonora/ui_pitch/pitch_editor_model.h"

namespace sonora::ui_pitch {

struct PitchViewport {
    double width = 0.0;   // pixels
    double height = 0.0;
    uint64_t frame_from = 0;  // region frames shown, [from, to)
    uint64_t frame_to = 0;
    double cents_low = 0.0;   // bottom edge, cents from A4, on a semitone boundary
    double cents_high = 0.0;  // top edge
};

// The whole region across, and every note's original and edited pitch
// with three semitones above and below, one octave at least.
PitchViewport fit_viewport(const PitchEditorModel& model, double width, double height);

double x_of_frame(const PitchViewport& v, double frame);
double y_of_cents(const PitchViewport& v, double cents);
// Cents per pixel down the view: what a vertical drag of `dy` pixels means.
double cents_of_dy(const PitchViewport& v, double dy);

struct NoteBlob {
    int index = 0;
    double x = 0.0, y = 0.0, w = 0.0, h = 0.0;  // the edited note
    double origin_y = 0.0;                      // where it was found
    bool moved = false;                         // edited pitch differs from found
    bool selected = false;
    std::string label;                          // "E3 +12"
    std::vector<double> contour;                // x0, y0, x1, y1, ... as it will sound
};

// The contour is the found one with the edit's shift, drift and vibrato
// scales applied to a straight-line split of it; the render splits with a
// smoothed trend instead (common/pitch plan_pieces), so the drawn line is a
// close preview, never the rendered result.
std::vector<NoteBlob> note_blobs(const PitchEditorModel& model, const PitchViewport& v);

// The note under (x, y), or -1. A blob is at least kMinHitPixels tall and
// wide for the pointer, and the nearest centre wins where two are in reach.
inline constexpr double kMinHitPixels = 10.0;
int hit_test(const std::vector<NoteBlob>& blobs, double x, double y);

struct RulerRow {
    double y = 0.0, h = 0.0;
    std::string name;  // "C4" on every C, empty otherwise
    bool black = false;
};
std::vector<RulerRow> ruler_rows(const PitchViewport& v);

// "A4", "C#3": the equal-tempered note nearest `cents` from A4.
std::string note_name(double cents);
// "+12", "-7", "0": cents off that note.
std::string cents_off_label(double cents);
std::string key_name(int key);  // 0 = C
std::string scale_name(pitch::Scale scale);

}  // namespace sonora::ui_pitch
