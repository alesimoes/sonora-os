// SPDX-License-Identifier: GPL-3.0-or-later
//
// The words of an arrangement edit (ADR-0116), apart from the controller so
// a test and the window say the same thing.
#pragma once

#include <string>

#include "sonora/model/arrangement.h"

namespace sonora::ui {

// What the operator is told when `section` went to its destination: the
// verb, and what the edit leaves where it was (kSectionEditLimits).
std::string arrangement_done_message(const std::string& section, model::SectionMode mode);

// A refusal, ending on what did not happen, so a held notice reads whole.
std::string arrangement_refusal_message(const std::string& reason, model::SectionMode mode);

}  // namespace sonora::ui
