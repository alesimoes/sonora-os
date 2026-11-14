// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/bus_order.h"

namespace sonora {

bool order_buses(uint32_t engine_tracks, const std::vector<SendBinding>& sends,
                 const std::vector<uint32_t>& bus_outputs, std::vector<uint32_t>& order) {
    const auto bus_count = static_cast<uint32_t>(bus_outputs.size());
    std::vector<uint32_t> inputs(bus_count, 0);
    for (const SendBinding& s : sends) {
        if (s.strip >= engine_tracks) ++inputs[s.bus];
    }
    for (uint32_t b = 0; b < bus_count; ++b) {
        if (bus_outputs[b] != kNoBus) ++inputs[bus_outputs[b]];
    }
    order.clear();
    std::vector<uint8_t> placed(bus_count, 0);
    while (order.size() < bus_count) {
        uint32_t next = bus_count;
        for (uint32_t b = 0; b < bus_count && next == bus_count; ++b) {
            if (!placed[b] && inputs[b] == 0) next = b;
        }
        if (next == bus_count) return false;
        placed[next] = 1;
        order.push_back(next);
        for (const SendBinding& s : sends) {
            if (s.strip == engine_tracks + next) --inputs[s.bus];
        }
        if (bus_outputs[next] != kNoBus) --inputs[bus_outputs[next]];
    }
    return true;
}

}  // namespace sonora
