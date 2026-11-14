// SPDX-License-Identifier: GPL-3.0-or-later
//
// The loop handle (ADR-0098 4a): the maths of dragging a selected clip's
// bottom-right corner past its end. Qt-free; EditController calls it.
#pragma once

#include "sonora/model/types.h"

namespace sonora::ui {

// Whole repeats a drag `past` frames beyond the end of a clip `length` long
// lays, snapped to the nearest: 2.4 lengths lay two, under half a length none.
int loop_handle_repeats(model::Frames length, model::Frames past);

}  // namespace sonora::ui
