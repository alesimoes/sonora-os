// SPDX-License-Identifier: GPL-3.0-or-later
//
// native.editors, the engine's side of the catalog (ADR-0057): what a Sonora
// bundle's descriptor.json adds to a catalog entry, and what is sent of a
// plugin's telemetry.
//   - the hints are taken whole or not at all: each thing the wire decoder
//     would refuse (a repeated id, a dangling curve_source, an inverted
//     range, too many meters) leaves the entry as the scan made it;
//   - a widget the parameter's shape cannot take is dropped, not sent;
//   - catalog_entry() reads descriptor.json from an LV2 bundle and from a
//     VST3 one, and a bundle without one is an ordinary entry;
//   - telemetry_frame() sends the declared slots only, and what it builds the
//     codec accepts.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "sonora/api/wire.h"
#include "sonora/plugin/probe_record.h"
#include "sonora/plugin_scanner.h"
#include "sonora/plugin_telemetry.h"

using namespace sonora;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

const char* const kGood = R"({
  "id": "org.sonora.test", "name": "Test",
  "params": [
    {"id": "bypass", "name": "Bypass", "index": 0},
    {"id": "gain", "name": "Gain", "index": 1, "section": "Level", "widget": "knob", "curve_source": "response"},
    {"id": "mode", "name": "Mode", "index": 2, "section": "Level", "widget": "switch"},
    {"id": "pick", "name": "Pick", "index": 3, "widget": "choice"}
  ],
  "meters": [
    {"id": "gr", "name": "Gain reduction", "unit": "dB", "kind": "gain_reduction", "min": 0.0, "max": 24.0},
    {"id": "in", "name": "Input", "unit": "dB", "kind": "input", "min": -60.0, "max": 6.0}
  ],
  "curves": [
    {"id": "response", "name": "Response", "x_unit": "Hz", "y_unit": "dB", "points": 128, "x_log": true,
     "x_min": 20.0, "x_max": 20000.0, "y_min": -24.0, "y_max": 24.0}
  ]
})";

api::PluginInfo entry() {
    api::PluginInfo p;
    p.id = "urn:test";
    p.name = "Test";
    api::PluginParam gain, mode, pick;
    gain.key = "gain";
    gain.name = "Gain";
    mode.key = "other_key";
    mode.name = "Mode";   // matched by name: a VST3 key is a number
    mode.steps = 2;
    pick.key = "pick";
    pick.name = "Pick";   // continuous: a choice cannot be drawn
    p.params = {gain, mode, pick};
    for (uint32_t i = 0; i < p.params.size(); ++i) p.params[i].index = i;
    return p;
}

// Whether `text` changes the entry, which a refusal must not.
bool refused(const std::string& text) {
    api::PluginInfo p = entry();
    const bool took = apply_descriptor_hints(p, text);
    const api::PluginInfo plain = entry();
    return !took && p.meters.empty() && p.curves.empty() && p.params[0].section.empty() &&
           p.params[0].curve_source.empty() && p.params[1].widget == plain.params[1].widget;
}

std::string with(const std::string& from, const std::string& to) {
    std::string s = kGood;
    const auto at = s.find(from);
    if (at == std::string::npos) std::printf("  (test bug: %s not in the descriptor)\n", from.c_str());
    else s.replace(at, from.size(), to);
    return s;
}

void taken() {
    std::printf("taken: a valid descriptor\n");
    api::PluginInfo p = entry();
    check(apply_descriptor_hints(p, kGood), "accepted");
    check(p.meters.size() == 2 && p.meters[0].slot == 0 && p.meters[1].slot == 1 &&
              p.meters[0].kind == uint32_t(api::PluginMeterKind::GainReduction) && p.meters[1].id == "in",
          "meters in slot order, kinds read");
    check(p.curves.size() == 1 && p.curves[0].slot == 0 && p.curves[0].points == 128 && p.curves[0].x_log &&
              p.curves[0].x_min == 20.0f && p.curves[0].y_max == 24.0f,
          "the curve and its axes");
    check(p.params[0].section == "Level" && p.params[0].widget == uint32_t(api::PluginWidget::Knob) &&
              p.params[0].curve_source == "response",
          "a parameter matched by key");
    check(p.params[1].section == "Level" && p.params[1].widget == uint32_t(api::PluginWidget::Switch),
          "a parameter matched by name");
    check(p.params[2].widget == 0, "a choice on a continuous parameter is dropped to Auto");
    // What a client then receives decodes.
    api::PluginCatalog c, back;
    c.generation = 1;
    c.plugins = {p};
    std::string bytes, err;
    check(api::wire::encode(c, bytes, err) && api::wire::decode(bytes, back, err), "the catalog round trips");
}

void refusals() {
    std::printf("refusals: nothing is half applied\n");
    check(refused("not json"), "not JSON");
    check(refused("[]"), "not an object");
    check(refused(with("\"id\": \"in\"", "\"id\": \"gr\"")), "a repeated meter id");
    check(refused(with("\"max\": 24.0", "\"max\": -1.0")), "an inverted meter range");
    check(refused(with("\"gain_reduction\"", "\"loudness\"")), "an unknown meter kind");
    check(refused(with("\"curve_source\": \"response\"", "\"curve_source\": \"missing\"")), "a dangling curve_source");
    check(refused(with("\"widget\": \"switch\"", "\"widget\": \"slider\"")), "an unknown widget");
    check(refused(with("\"points\": 128", "\"points\": 1")), "a curve of one point");
    check(refused(with("\"points\": 128", "\"points\": 129")), "a curve past the cap");
    check(refused(with("\"x_min\": 20.0", "\"x_min\": 0.0")), "a log axis from 0");
    check(refused(with("\"section\": \"Level\", \"widget\": \"knob\"",
                       "\"section\": \"" + std::string(65, 'x') + "\", \"widget\": \"knob\"")),
          "a section past 64 bytes");
    std::string many = "\"meters\": [";
    for (int i = 0; i < 9; ++i)
        many += std::string(i ? "," : "") + "{\"id\":\"m" + std::to_string(i) +
                "\",\"name\":\"M\",\"unit\":\"dB\",\"kind\":\"other\",\"min\":0.0,\"max\":1.0}";
    many += "]";
    std::string nine = kGood;
    const auto at = nine.find("\"meters\"");
    nine.replace(at, nine.find("],", at) + 1 - at, many);
    check(refused(nine), "nine meters");
}

void bundles() {
    std::printf("bundles: catalog_entry reads the bundle's own descriptor\n");
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("sonora_descriptor_hints_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "a.lv2");
    fs::create_directories(root / "b.vst3" / "Contents" / "Resources");
    fs::create_directories(root / "plain.lv2");
    std::ofstream(root / "a.lv2" / "descriptor.json") << kGood;
    std::ofstream(root / "b.vst3" / "Contents" / "Resources" / "descriptor.json") << kGood;
    const auto record = [&](const std::string& bundle, const char* format) {
        plugin::probe::Record r;
        r.uri = std::string(format) == "vst3" ? "vst3:0123456789ABCDEF0123456789ABCDEF" : "urn:test";
        r.format = format;
        r.bundle = bundle;
        r.name = "Test";
        r.outcome = plugin::probe::Outcome::Ok;
        r.audio_in = r.audio_out = 1;
        plugin::probe::Param g, m;
        g.key = "gain";
        g.symbol = "gain";
        g.name = "Gain";
        m.key = "mode";
        m.symbol = "mode";
        m.name = "Mode";
        m.steps = 2;
        r.params = {g, m};
        return r;
    };
    const api::PluginInfo lv2 = catalog_entry(record((root / "a.lv2").string(), "lv2"));
    check(lv2.meters.size() == 2 && lv2.params[0].section == "Level", "an LV2 bundle: descriptor.json at its top");
    const api::PluginInfo vst3 = catalog_entry(record((root / "b.vst3").string(), "vst3"));
    check(vst3.meters.size() == 2 && vst3.curves.size() == 1, "a VST3 bundle: Contents/Resources/descriptor.json");
    const api::PluginInfo plain = catalog_entry(record((root / "plain.lv2").string(), "lv2"));
    check(plain.meters.empty() && plain.curves.empty() && plain.params[0].section.empty(),
          "a bundle with no descriptor is an ordinary entry");
    plugin::probe::Record refused_record = record((root / "a.lv2").string(), "lv2");
    refused_record.outcome = plugin::probe::Outcome::Refused;
    refused_record.reason = "no";
    check(catalog_entry(refused_record).meters.empty(), "a refused plugin gets no hints");
    fs::remove_all(root);
}

void frames() {
    std::printf("frames: only what the catalog declared is sent\n");
    api::PluginInfo p = entry();
    check(apply_descriptor_hints(p, kGood), "a catalog entry with hints");
    plugin::TelemetrySnapshot s;
    s.sequence = 7;
    s.meter_mask = 0xFFu;   // a plugin that says it published all eight
    for (uint32_t i = 0; i < plugin::kMaxTelemetryMeters; ++i) s.meters[i] = float(i);
    s.curve_points[0] = 128;
    s.curve_points[1] = 64;   // a curve the catalog never declared
    for (uint32_t i = 0; i < 128; ++i) s.curves[0][i] = float(i);
    api::PluginTelemetry f;
    check(telemetry_frame(p, 42, s, f), "a frame");
    check(f.insert == 42 && f.sequence == 7 && f.meter_mask == 3u && f.meters.size() == 2 &&
              f.meters[1] == 1.0f,
          "two meters, the mask limited to them");
    check(f.curves.size() == 1 && f.curves[0].size() == 128, "one curve, the undeclared one dropped");
    s.curve_points[0] = 100;
    p.curves[0].points = 64;
    check(telemetry_frame(p, 42, s, f) && f.curves[0].size() == 64, "a curve is cut to its declared points");
    s.curve_points[0] = 0;
    check(telemetry_frame(p, 42, s, f) && f.curves[0].empty(), "a curve never published is empty");
    std::string bytes, err;
    api::PluginTelemetry back;
    check(api::wire::encode(f, bytes, err) && api::wire::decode(bytes, back, err), "the codec takes it");
    api::PluginInfo none;
    check(!telemetry_frame(none, 1, s, f), "a plugin that declares nothing sends nothing");
}

}  // namespace

int main() {
    taken();
    refusals();
    bundles();
    frames();
    if (failures != 0) return EXIT_FAILURE;
    std::printf("descriptor_hints: ok\n");
    return EXIT_SUCCESS;
}
