// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the parameter table generates (ADR-0011 §2.4), none of it real-time:
// - the JSON descriptor shipped inside each bundle, which the UI reads to
//   build an editor (§2.6) and the mix assistant reads to know what it may
//   set (§4.4);
// - state and presets: canonical JSON through common/json, carrying the
//   processor id, its state version and values by string id. An unknown id
//   is refused with a message; a missing id takes its default.
//
// The LV2 TTL and VST3 parameter info come from the same table in the
// format adapters (N1).
#pragma once

#include <string>

#include "sonora/native/processor.h"

namespace sonora::native {

// Where a plugin format addresses a parameter by something other than its
// id. The format adapter knows (DPF's LV2 exporter names the bypass port
// "lv2_enabled" and inverts it); the descriptor carries it so an editor
// built from the descriptor finds the port.
struct FormatSymbol {
    std::uint32_t index;  // into Descriptor::params
    const char* format;   // "lv2"
    const char* symbol;   // what that format calls it
    bool inverted;        // the format's value is (min + max) - ours
};

// With `symbols`, each listed parameter gets a "formats" object:
// {"lv2": {"symbol": "lv2_enabled", "inverted": true}}.
std::string descriptor_json(const Descriptor& d, const FormatSymbol* symbols = nullptr,
                            std::uint32_t symbol_count = 0);

std::string save_state(const Processor& p);
// On failure the processor is unchanged and `error` says why.
bool load_state(Processor& p, const std::string& text, std::string& error);

}  // namespace sonora::native
