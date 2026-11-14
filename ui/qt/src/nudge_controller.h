// SPDX-License-Identifier: GPL-3.0-or-later
//
// Holds the nudge value for one window (ADR-0149). EditController and
// SessionModel own one and call reset() when a session opens; the command
// nudge.value.next calls advance().
#pragma once

#include "nudge_value.h"

namespace sonora::ui {

class NudgeController {
public:
    NudgeValue value() const { return value_; }
    const char* label() const { return nudge_value_name(value_); }
    void advance() { value_ = next_nudge_value(value_); }
    void reset() { value_ = NudgeValue::FollowGrid; }
    model::Frames target(model::Frames f, int direction, GridValue grid,
                         const model::SessionSettings& s) const {
        return nudge_value_target(f, direction, value_, grid, s);
    }

private:
    NudgeValue value_ = NudgeValue::FollowGrid;
};

}  // namespace sonora::ui
