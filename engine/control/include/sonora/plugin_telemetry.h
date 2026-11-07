// SPDX-License-Identifier: GPL-3.0-or-later
//
// From a plugin's telemetry block to the wire (ADR-0057): the control
// thread's read gives a TelemetrySnapshot of untrusted words; what a client
// receives is only what the catalog declared for that plugin.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "sonora/api/messages.h"
#include "sonora/plugin/telemetry.h"

namespace sonora {

// False when the catalog entry declares no meter and no curve (nothing to
// send). Otherwise `out` carries the declared meters (the mask limited to
// them: a plugin cannot publish a slot the catalog did not name) and the
// declared curves, each cut to the points the catalog says it has, and true.
// The snapshot is already finite (TelemetryBlock::read); the wire encoder
// refuses what is not, so this never builds such a frame.
inline bool telemetry_frame(const api::PluginInfo& info, uint64_t insert,
                            const plugin::TelemetrySnapshot& s, api::PluginTelemetry& out) {
    if (info.meters.empty() && info.curves.empty()) return false;
    const uint32_t meters = static_cast<uint32_t>(
        std::min<std::size_t>(info.meters.size(), plugin::kMaxTelemetryMeters));
    const uint32_t curves = static_cast<uint32_t>(
        std::min<std::size_t>(info.curves.size(), plugin::kMaxTelemetryCurves));
    out.insert = insert;
    out.sequence = s.sequence;
    out.meter_mask = meters >= 32 ? s.meter_mask : (s.meter_mask & ((1u << meters) - 1u));
    out.meters.assign(s.meters, s.meters + meters);
    out.curves.assign(curves, std::vector<float>());
    for (uint32_t c = 0; c < curves; ++c) {
        const uint32_t n = std::min(s.curve_points[c], info.curves[c].points);
        out.curves[c].assign(s.curves[c], s.curves[c] + n);
    }
    return true;
}

}  // namespace sonora
