// SPDX-License-Identifier: GPL-3.0-or-later
//
// A track with takes to comp, for the tests and the screenshot scenes: two
// more recorded passes of the track's phrase on playlists of their own
// (what a loop take leaves, ADR-0016 §3), so the track has take lanes. The
// sources name files that do not exist, as the demo session's do: their
// audio is synthetic and the window says so.
#pragma once

#include <string>

#include "timeline_document.h"

namespace sonora::ui {

// On the track at display row `row`: sources "<name>_02" and "<name>_03",
// each 40 bars, and two playlists "<name> take 2" and "<name> take 3" each
// holding the phrase from bar `from_bar` for `bars` bars, played from the
// same place in its own take. The active playlist is left as it is. One
// operation at a time through `doc`, so it works on the engine's document
// as on a model one. False, with why, at the first one refused.
bool add_demo_takes(TimelineDocument& doc, int row, int from_bar, int bars, std::string& why);

}  // namespace sonora::ui
