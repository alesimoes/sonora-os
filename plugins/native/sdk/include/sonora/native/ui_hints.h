// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a processor tells an editor beyond its parameter table (ADR-0057,
// native.editors): which section each parameter sits in, how to draw it,
// which curve it shapes, and the meters and curves the processor publishes
// through native::Telemetry. Static data like the table itself. Generated
// into the bundle's descriptor.json, read by the engine's scan and sent to
// the interface in the catalog; a processor that declares nothing is drawn
// as before.
//
// Meter slot n is the n-th entry of `meters`, curve index n the n-th of
// `curves`: the slot a processor passes to Telemetry::meter() is the place
// in these tables.
#pragma once

#include <cstdint>

namespace sonora::native {

enum class Widget : std::uint8_t { Auto, Knob, Fader, Switch, Choice };
enum class MeterKind : std::uint8_t { Other, GainReduction, InputLevel, OutputLevel };

// By ParamInfo::id.
struct ParamHint {
    const char* id;
    const char* section;       // null: none
    Widget widget;
    const char* curve_source;  // CurveDecl::id this parameter shapes, or null
};

struct MeterDecl {
    const char* id;
    const char* name;
    const char* unit;
    MeterKind kind;
    float min, max;
};

struct CurveDecl {
    const char* id;
    const char* name;
    const char* x_unit;
    const char* y_unit;
    std::uint32_t points;  // 2 .. 128
    bool x_log;
    float x_min, x_max, y_min, y_max;
};

struct UiHints {
    const ParamHint* params;
    std::uint32_t param_count;
    const MeterDecl* meters;
    std::uint32_t meter_count;
    const CurveDecl* curves;
    std::uint32_t curve_count;
};

// The meters most processors share, so their ids and ranges agree: put them
// in a table in this order and give publish_levels() the slots of the input
// and output entries.
inline constexpr MeterDecl kGainReductionMeter = {"gr", "Gain reduction", "dB",
                                                  MeterKind::GainReduction, 0.0f, 24.0f};
inline constexpr MeterDecl kInputMeter = {"in", "Input", "dB", MeterKind::InputLevel, -60.0f, 6.0f};
inline constexpr MeterDecl kOutputMeter = {"out", "Output", "dB", MeterKind::OutputLevel, -60.0f,
                                           6.0f};

}  // namespace sonora::native
