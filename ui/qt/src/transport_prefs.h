// SPDX-License-Identifier: GPL-3.0-or-later
//
// Click and Count Off are toggles on the wire (ADR-0020), so a preference is
// applied by comparing it with transport_flags from the state block and
// toggling only what differs: a reconnect never inverts what the engine has.
#pragma once

#include <cstdint>

#include "preferences.h"

namespace sonora::ui {

struct TransportToggles {
    bool toggle_click = false;       // send transport.toggle_click
    bool toggle_count_off = false;   // send transport.toggle_count_off
};

TransportToggles plan_transport_toggles(const Preferences& prefs, uint32_t transport_flags);

}  // namespace sonora::ui
