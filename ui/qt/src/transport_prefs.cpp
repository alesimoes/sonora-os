// SPDX-License-Identifier: GPL-3.0-or-later
#include "transport_prefs.h"

#include "sonora/api/state_block.h"

namespace sonora::ui {

TransportToggles plan_transport_toggles(const Preferences& prefs, uint32_t transport_flags) {
    const bool click_on = (transport_flags & api::kTransportClick) != 0;
    const bool count_on = (transport_flags & api::kTransportCountOff) != 0;
    return {prefs.click_at_start != click_on, prefs.count_off_at_start != count_on};
}

}  // namespace sonora::ui
