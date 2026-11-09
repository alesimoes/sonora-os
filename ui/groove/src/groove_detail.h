// SPDX-License-Identifier: GPL-3.0-or-later
//
// What groove_apply.cpp and groove_command.cpp share inside the module.
#pragma once

#include <string>

#include "sonora/ui_groove/groove_command.h"

namespace sonora::ui::groove_detail {

// Which selected tracks are grooved: all of them, or those below the first,
// whose regions are the reference (`groove region`).
enum class Targets { Selection, BelowReference };

bool apply(EditController& ec, const groove::GrooveTemplate& tmpl, double strength,
           Targets which, GrooveReport& report, std::string& why);

}  // namespace sonora::ui::groove_detail
