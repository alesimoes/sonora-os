// SPDX-License-Identifier: GPL-3.0-or-later
//
// Editing on the Tempo and Meter rulers (time.tempo_map, ADR-0140): what the
// ruler's gestures do to the session, given the Edit window's controller.
// Hit-testing and drag arithmetic are ui/conductor's; every change goes
// through EditController's tempo calls, so each gesture is one Undo Tempo
// step. Qt-free, so it tests without a display; ConductorRulerItem calls it.
//
// `x` is in ruler pixels from its left edge (the view's scroll applies).
// Every edit is refused, with the reason in `why`, while a take is open:
// the map the take is being recorded against must not move under it.
#pragma once

#include <cstdint>
#include <string>

namespace sonora::ui {

class EditController;

namespace conductor_edit {

// The tempo mark under `x`, or -1.
int mark_at(const EditController& ctl, double x);

// The value a drag of mark `index` by `dy` pixels (up positive) would set,
// as the tooltip shows it ("132.40 bpm"); empty when there is no such mark.
std::string drag_label(const EditController& ctl, int index, double dy, bool whole_bpm);

// Ends the drag: one set_tempo at the mark, so one Undo Tempo step.
bool drag_end(EditController& ctl, bool recording, int index, double dy, bool whole_bpm,
              std::string& why);

// The value the inline field opens with: mark `index`'s tempo, or with -1
// the tempo in force at the insertion point ("120.00").
std::string tempo_text(const EditController& ctl, int index);

// The typed value ("96", "96 bpm"): at mark `index`, or with -1 a new change
// at the insertion point.
bool type_tempo(EditController& ctl, bool recording, int index, const std::string& text,
                std::string& why);

// Removes mark `index`; the first is refused, with the reason.
bool remove(EditController& ctl, bool recording, int index, std::string& why);

// The meter in force at `x` ("4/4"), what the picker opens on.
std::string meter_text(const EditController& ctl, double x);

// A meter on the bar line nearest `x`; the insertion point goes there.
bool set_meter(EditController& ctl, bool recording, double x, std::uint32_t beats,
               std::uint32_t note, std::string& why);

}  // namespace conductor_edit
}  // namespace sonora::ui
