// SPDX-License-Identifier: GPL-3.0-or-later
//
// Groove as an edit (ADR-0115): the selected regions move onto a groove's
// slots as one Compound of MoveRegion, so one Undo puts them back. Built on
// EditController's public API only; the text words reach it through
// tempo.edit ("groove ...", ui/qt/src/edit_tempo.cpp).
#pragma once

#include <cstddef>
#include <string>

#include "sonora/groove/groove.h"

namespace sonora::ui {

class EditController;
class OnsetSource;
class GrooveTemplateStore;

struct GrooveReport {
    std::size_t moved = 0;
    std::size_t overlapping = 0;  // left in place: would overlap a neighbour or start before 0
    std::size_t grouped = 0;      // left in place: in a clip group, which moves as one
    std::string message;          // the counts, for the operator
};

// Moves the selected region, or the regions that start inside the selected
// range on the selected tracks, onto `tmpl` at `strength` percent. True when
// at least one region moved; `report` says how many moved and how many were
// left where they were, and why. Refused in Shuffle, with nothing selected,
// or when nothing would move, with the reason in `why`.
bool groove_apply(EditController& ec, const groove::GrooveTemplate& tmpl, double strength,
                  GrooveReport& report, std::string& why);

// The groove of the reference: the regions the selected range covers on its
// first track, their onsets folded onto `slots_per_beat` slots.
bool groove_from_reference(const EditController& ec, const OnsetSource& onsets,
                           std::size_t slots_per_beat, groove::GrooveTemplate& out,
                           std::string& why);

struct GrooveDeps {
    const OnsetSource* onsets = nullptr;       // null: `groove region` is refused
    const GrooveTemplateStore* store = nullptr;  // null: `groove saved` and `save` refused
};

// The words after "groove":
//   swing <percent> <subdivision> [strength]   50..75 percent, 8/16/32
//   region [slots_per_beat] [strength]          the top selected track is the reference,
//                                               the tracks below it are grooved
//   saved <name> [strength]
//   save <name> swing <percent> <subdivision>
//   save <name> region [slots_per_beat]
// Strength is a percent, 1..100, default 100. False with the reason when
// refused, and also when some regions were left in place: the caller shows
// `why` only on false, and a skipped region must be reported.
bool groove_command(EditController& ec, const GrooveDeps& deps, const std::string& text,
                    std::string& why);

// The tempo.edit entry point: the saved templates in the preferences
// directory; no onset source is reachable from here yet, so `region` says so.
bool groove_command(EditController& ec, const std::string& text, std::string& why);

}  // namespace sonora::ui
