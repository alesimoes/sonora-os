// SPDX-License-Identifier: GPL-3.0-or-later
#include "lv2_presets.h"

#include <lv2/atom/atom.h>
#include <lv2/presets/presets.h>
#include <lv2/state/state.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace sonora::plugin_host {

namespace {

namespace pr = plugin::probe;

// lv2_host.cpp's save_state() names its states so; a restore does not
// read the name (lilv_state_new_from_string looks for the pset:Preset).
constexpr const char* kStateUri = "urn:sonora:plugin-state";
constexpr const char* kRdfsLabel = "http://www.w3.org/2000/01/rdf-schema#label";

struct Node {
    explicit Node(LilvNode* n) : n(n) {}
    ~Node() { lilv_node_free(n); }
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    LilvNode* n;
};

std::string label_of(LilvWorld* w, const LilvNode* preset) {
    Node label_uri(lilv_new_uri(w, kRdfsLabel));
    Node label(lilv_world_get(w, preset, label_uri.n, nullptr));
    std::string s = label.n && lilv_node_is_string(label.n) ? lilv_node_as_string(label.n) : "";
    if (s.size() > pr::kMaxRecordText) s.resize(pr::kMaxRecordText);
    return s;
}

// The presets lilv relates to the plugin, their resources loaded so their
// labels and state are in the world.
std::vector<std::string> related(Lv2World& world, const LilvPlugin* plugin) {
    std::vector<std::string> out;
    Node preset_class(lilv_new_uri(world.world(), LV2_PRESETS__Preset));
    LilvNodes* presets = lilv_plugin_get_related(plugin, preset_class.n);
    if (!presets) return out;
    LILV_FOREACH (nodes, i, presets) {
        const LilvNode* p = lilv_nodes_get(presets, i);
        if (!lilv_node_is_uri(p)) continue;
        lilv_world_load_resource(world.world(), p);
        out.emplace_back(lilv_node_as_uri(p));
    }
    lilv_nodes_free(presets);
    return out;
}

struct PortValues {
    Lv2World* world;
    LV2_URID f32, f64, i32, i64, boolean;
    std::vector<pr::PresetValue> values;
    std::set<std::string> seen;
    std::string bad;  // the first value that is not a finite number
};

void collect(const char* symbol, void* user, const void* value, uint32_t size, uint32_t type) {
    auto& pv = *static_cast<PortValues*>(user);
    if (!symbol || !*symbol || !value || pv.values.size() >= pr::kMaxRecordParams) return;
    double v = NAN;
    if (type == pv.f32 && size == sizeof(float)) {
        float f;
        std::memcpy(&f, value, sizeof f);
        v = f;
    } else if (type == pv.f64 && size == sizeof(double)) {
        std::memcpy(&v, value, sizeof v);
    } else if ((type == pv.i32 || type == pv.boolean) && size == sizeof(int32_t)) {
        int32_t i;
        std::memcpy(&i, value, sizeof i);
        v = i;
    } else if (type == pv.i64 && size == sizeof(int64_t)) {
        int64_t i;
        std::memcpy(&i, value, sizeof i);
        v = double(i);
    }
    if (!std::isfinite(v) || std::fabs(v) > 3.4e38) {
        if (pv.bad.empty()) pv.bad = symbol;
        return;
    }
    if (!pv.seen.insert(symbol).second) return;
    pv.values.push_back({symbol, static_cast<float>(v)});
}

}  // namespace

std::vector<pr::Preset> lv2_factory_presets(Lv2World& world, const LilvPlugin* plugin, std::size_t cap) {
    std::vector<pr::Preset> out;
    for (const std::string& uri : related(world, plugin)) {
        Node node(lilv_new_uri(world.world(), uri.c_str()));
        std::string name = label_of(world.world(), node.n);
        if (name.empty()) {
            // A preset without a label is still one; name it by its URI's tail.
            const auto cut = uri.find_last_of("#/");
            name = cut == std::string::npos ? uri : uri.substr(cut + 1);
        }
        if (uri.size() + 4 > pr::kMaxRecordText) continue;
        out.push_back({"lv2:" + uri, std::move(name)});
    }
    std::sort(out.begin(), out.end(),
              [](const pr::Preset& a, const pr::Preset& b) { return a.name != b.name ? a.name < b.name : a.id < b.id; });
    out.erase(std::unique(out.begin(), out.end(), [](const pr::Preset& a, const pr::Preset& b) { return a.id == b.id; }),
              out.end());
    if (out.size() > cap) out.resize(cap);
    return out;
}

Lv2PresetResult lv2_read_preset(Lv2World& world, const LilvPlugin* plugin, const std::string& preset_uri,
                                Lv2PresetContent& out, std::string& error) {
    const std::vector<std::string> mine = related(world, plugin);
    if (std::find(mine.begin(), mine.end(), preset_uri) == mine.end()) {
        error = "no preset <" + preset_uri + "> of this plugin";
        return Lv2PresetResult::NotFound;
    }
    LilvWorld* w = world.world();
    UridMap& u = world.urids();
    Node node(lilv_new_uri(w, preset_uri.c_str()));
    LilvState* state = lilv_state_new_from_world(w, u.map_feature(), node.n);
    if (!state) {
        error = "lilv cannot read preset <" + preset_uri + ">";
        return Lv2PresetResult::Refused;
    }
    const LilvNode* of = lilv_state_get_plugin_uri(state);
    const LilvNode* want = lilv_plugin_get_uri(plugin);
    if (!of || !lilv_node_equals(of, want)) {
        lilv_state_free(state);
        error = "preset <" + preset_uri + "> is another plugin's state";
        return Lv2PresetResult::Refused;
    }
    PortValues pv{&world,
                  u.map(LV2_ATOM__Float),
                  u.map(LV2_ATOM__Double),
                  u.map(LV2_ATOM__Int),
                  u.map(LV2_ATOM__Long),
                  u.map(LV2_ATOM__Bool),
                  {},
                  {},
                  {}};
    lilv_state_emit_port_values(state, collect, &pv);
    if (!pv.bad.empty()) {
        lilv_state_free(state);
        error = "preset <" + preset_uri + "> sets port \"" + pv.bad + "\" to something that is not a finite number";
        return Lv2PresetResult::Refused;
    }
    Lv2PresetContent c;
    c.name = label_of(w, node.n);
    c.params = std::move(pv.values);
    Node state_iface(lilv_new_uri(w, LV2_STATE__interface));
    if (lilv_plugin_has_extension_data(plugin, state_iface.n)) {
        // The port values travel in the Turtle too; restore_state() passes
        // no set_value, so `params` alone decides them, as for a saved state.
        char* text = lilv_state_to_string(w, u.map_feature(), u.unmap_feature(), state, kStateUri, nullptr);
        if (!text) {
            lilv_state_free(state);
            error = "preset <" + preset_uri + "> could not be written as a state";
            return Lv2PresetResult::Refused;
        }
        c.state = text;
        lilv_free(text);
    }
    lilv_state_free(state);
    out = std::move(c);
    return Lv2PresetResult::Ok;
}

}  // namespace sonora::plugin_host
