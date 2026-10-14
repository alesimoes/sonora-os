// SPDX-License-Identifier: GPL-3.0-or-later
//
// How a fader's travel maps to gain, and how gain and pan are written.
//
// A fader is not linear in dB: an engineer rides it between -20 and +6 dB
// and wants most of the travel there, with 0 dB well below the top and the
// last few millimetres falling to silence. The table below is our own
// shape (ADR-0004: we reproduce workflow, not a vendor's curve): +12 dB at
// the top, as in Pro Tools, 0 dB at 78 % of the travel, -inf at the bottom.
// Piecewise linear in dB between the points, so the mapping is monotonic,
// exactly invertible, and testable without a display.
#pragma once

#include <string>

namespace sonora::ui {

// 0 (bottom, -inf) .. 1 (top, +12 dB).
float fader_position_from_db(float db);
float fader_db_from_position(float position);

// Gain at or below this is silence and is written "-inf".
constexpr float kFaderSilenceDb = -144.0f;
// The top of the travel.
constexpr float kFaderMaxDb = 12.0f;

// "+3.5", "0.0", "-12.0", "-inf". One decimal, always signed except 0.0.
std::string gain_text(float db);
// "<100" .. "<1", "C", "1>" .. "100>", for pan -1 .. +1, as consoles mark it.
std::string pan_text(float pan);

// One step of a fader drag: `dy` pixels (positive is down) on a fader whose
// travel is `travel` pixels. `fine` divides the motion by ten, for the
// last half-dB of a vocal ride. A gain past an end of the travel is kept
// until the drag moves back onto it.
float fader_drag(float db, float dy, float travel, bool fine);

// What the operator typed into a fader's readout: "-6", "-6.5 dB", "+3",
// "0", "-inf" (any case, spaces allowed). The result is clamped to nothing:
// a gain off the travel (above +12 dB) is refused with the range in `why`,
// rather than quietly written as +12. Locale-free, so "-6.5" is the same on
// a French desktop.
bool parse_gain_text(const std::string& text, float& db, std::string& why);
// A pan typed into its readout, in pan_text's own spelling ("<30", "30>",
// "C") or as a console's ("L30", "R30") or a signed number ("-30" is left).
// 100 is hard over; past it is refused.
bool parse_pan_text(const std::string& text, float& pan, std::string& why);

// The linear gain a ParameterChange carries for `db` (api::ParamId::Gain:
// 0 .. 4, which is +12.04 dB). At or below the fader's -inf it is 0.
float fader_linear_gain(float db);

}  // namespace sonora::ui
