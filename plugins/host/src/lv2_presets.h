// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// LV2 factory presets (ADR-0029 §1, §5): the pset:Preset resources lilv
// relates to a plugin, from its own bundle or any other bundle on the LV2
// path. Listing reads their labels; reading one turns it into what a
// session holds, port values as params and the rest as the plugin's state
// in the encoding lv2_host.cpp's save_state() writes (host_control.h
// StateBlob, one section). No plugin code runs here: --read-preset then
// restores the state into an instance, which is the plugin's say.

#include "lv2_host.h"
#include "sonora/plugin/probe_record.h"

#include <string>
#include <vector>

namespace sonora::plugin_host {

// At most `cap`, sorted by name, then id, so a listing is the same on
// every machine. Ids are "lv2:" and the preset's URI.
std::vector<plugin::probe::Preset> lv2_factory_presets(Lv2World& world, const LilvPlugin* plugin, std::size_t cap);

struct Lv2PresetContent {
    std::string name;
    std::vector<plugin::probe::PresetValue> params;  // port symbol, plain value
    // The state as Turtle, the section lv2_host's restore_state() reads; empty
    // when the plugin has no state:interface (its preset is port values).
    std::string state;
};

enum class Lv2PresetResult { Ok, NotFound, Refused };

// `preset_uri` without the "lv2:". NotFound: lilv relates no such preset to
// this plugin. Refused, with the reason: lilv cannot read it, or it applies
// to another plugin, or a port value is not a finite number.
Lv2PresetResult lv2_read_preset(Lv2World& world, const LilvPlugin* plugin, const std::string& preset_uri,
                                Lv2PresetContent& out, std::string& error);

}  // namespace sonora::plugin_host
