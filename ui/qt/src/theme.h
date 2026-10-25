// SPDX-License-Identifier: GPL-3.0-or-later
//
// The design tokens, in one place. brain/specs/ui-design.md is the spec and
// says why each value is what it is; this is the only file that may spell a
// colour or a size. The scene-graph items (meters, timeline, rulers) read
// these constants directly, and QML reads the same values through the
// `Theme` singleton (theme_object.h), so a token changed here changes every
// view and no literal can drift away from it.
//
// No Qt here: meter_model.cpp and timeline_scene.cpp are Qt-free and their
// tests check colours by these names.
#pragma once

#include <cstdint>

namespace sonora::ui::theme {

// Colours are 0xAARRGGBB, opaque unless the name says otherwise.

// --- surfaces, darkest to lightest ---
constexpr uint32_t kSunken = 0xff0d0f12;     // timeline lanes, meter wells
constexpr uint32_t kWindow = 0xff121417;     // the window behind everything
constexpr uint32_t kPanel = 0xff1a1d22;      // transport, toolbars, headers
constexpr uint32_t kRaised = 0xff252930;     // buttons at rest, strips
constexpr uint32_t kHover = 0xff2e333b;      // a raised surface under the pointer
constexpr uint32_t kLaneA = 0xff14171b;      // alternate timeline lanes
constexpr uint32_t kLaneB = 0xff111316;
constexpr uint32_t kBorderSubtle = 0xff2a2f36;
constexpr uint32_t kBorderStrong = 0xff3a4048;
// Depth without chrome: a 1 px light edge on top of a raised surface, and a
// soft dark edge under a panel. Translucent, so they work on any surface.
constexpr uint32_t kHighlight = 0x12ffffff;
constexpr uint32_t kShadow = 0x73000000;

// --- text ---
constexpr uint32_t kText = 0xffe8ebef;       // 14.1:1 on panel
constexpr uint32_t kTextSecondary = 0xffa3abb5;  // 7.3:1 on panel
constexpr uint32_t kTextTertiary = 0xff7c8590;   // 4.5:1 on panel; never on raised
constexpr uint32_t kTextOnAccent = 0xff121417;   // dark text on a lit button

// --- meaning; each used for one thing only ---
constexpr uint32_t kAccent = 0xff3fc1b0;     // selection, focus, the insertion cursor
constexpr uint32_t kRecord = 0xfff0524f;     // record arm, record transport, recording
constexpr uint32_t kSolo = 0xfff2c14e;
constexpr uint32_t kMute = 0xff6b9bf2;
constexpr uint32_t kInput = 0xff4cc38a;      // input monitoring, "engine alive"
constexpr uint32_t kCaution = 0xffe8b04b;    // status line, unverified, stale
constexpr uint32_t kAlarm = 0xffff5a5f;      // banners: engine gone, software GL
constexpr uint32_t kAlarmSurface = 0xff3a1618;
constexpr uint32_t kPlayhead = 0xfff5f7fa;
constexpr uint32_t kBypass = 0xff9d7cf0;     // a bypassed insert
constexpr uint32_t kMarker = 0xffe0a83c;     // markers and memory locations
constexpr uint32_t kMarkerRange = 0x59e0a83c;  // a selection marker's bar: marker at 35 %
constexpr uint32_t kConductor = 0xff8ccf7e;  // tempo and meter changes on their rulers

// --- timeline ---
constexpr uint32_t kGridBar = 0xff2b3038;
constexpr uint32_t kGridBeat = 0xff1d2127;
constexpr uint32_t kRulerTick = 0xff4a525b;
constexpr uint32_t kSelectionFill = 0x333fc1b0;  // accent at 20 %, over regions
constexpr uint32_t kRegionSelectedOutline = kAccent;
// Fades: the gain curve as a `text` line, the part of the region the fade
// takes away darkened beneath it, a crossfade as a faint box over the cut.
constexpr uint32_t kFadeLine = 0xffe8ebef;
constexpr uint32_t kFadeShade = 0x8c000000;       // black at 55 %
constexpr uint32_t kCrossfadeFill = 0x1fe8ebef;   // text at 12 %
constexpr uint32_t kCrossfadeEdge = 0x80e8ebef;   // text at 50 %

// Region colours, by track, cycling. Muted mid-tones so a session of 18
// tracks reads as calm; the waveform is drawn lighter than the body.
constexpr int kRegionPaletteSize = 8;
constexpr uint32_t kRegionPalette[kRegionPaletteSize] = {
    0xff4c7fd9,  // blue
    0xff2fa39a,  // teal
    0xff5ba85a,  // green
    0xffa3a34a,  // olive
    0xffd9873e,  // orange
    0xffc9545a,  // red
    0xff8c6bd1,  // violet
    0xff6b8299,  // slate
};

// --- meters (meter_model.cpp) ---
constexpr uint32_t kMeterWell = kSunken;
constexpr uint32_t kMeterNoData = 0xff2a2e34;
constexpr uint32_t kMeterGreen = 0xff3dd68c;
constexpr uint32_t kMeterAmber = 0xfff2b33d;
constexpr uint32_t kMeterRed = 0xfff0524f;
constexpr uint32_t kMeterHold = 0xfff5f7fa;
constexpr uint32_t kMeterOverOff = 0xff2b3036;
constexpr uint32_t kMeterOverOn = 0xffff3b3b;
constexpr uint32_t kMeterMark = 0xff4a525b;

// --- spacing: a 4 px grid ---
constexpr int kSpace1 = 4;
constexpr int kSpace2 = 8;
constexpr int kSpace3 = 12;
constexpr int kSpace4 = 16;
constexpr int kSpace6 = 24;
constexpr int kRadius = 4;
constexpr int kRadiusSmall = 2;

// --- typography: one sans family, a five-step scale ---
constexpr const char* kFontFamily = "Inter";
constexpr const char* kFontFallback = "DejaVu Sans";
constexpr int kTextOverline = 9;   // upper-case field labels above a value
constexpr int kTextCaption = 10;   // ruler labels, meter scales
constexpr int kTextSmall = 11;     // button captions, secondary labels
constexpr int kTextBody = 13;      // names, status
constexpr int kTextTitle = 15;     // window tabs
constexpr int kTextCounter = 28;   // the main counter, tabular numerals
constexpr int kTextDisplay = 40;   // NO DATA over dead meters, and nothing else

// --- fixed layout dimensions ---
constexpr int kTransportHeight = 64;
constexpr int kToolbarHeight = 40;
constexpr int kRulerHeight = 20;
constexpr int kTrackHeaderWidth = 216;
constexpr int kTrackHeight = 64;
constexpr int kRegionNameBand = 14;
// The marker ruler's flag, from the marker's frame rightwards, and how far
// outside it a click still takes the marker.
constexpr int kMarkerFlagWidth = 9;
constexpr int kMarkerHitSlop = 3;
constexpr int kInsertSlotHeight = 16;

// The colour a region on display-order track `index` gets.
constexpr uint32_t region_colour(int index) {
    return kRegionPalette[(index < 0 ? 0 : index) % kRegionPaletteSize];
}

// Mixes `a` toward `b` by `t` (0..1), opaque. For the region body (palette
// toward the lane) and the waveform (palette toward white).
constexpr uint32_t mix(uint32_t a, uint32_t b, float t) {
    const auto ch = [](uint32_t c, int s) { return static_cast<float>((c >> s) & 0xffu); };
    const auto lerp = [t](float x, float y) {
        return static_cast<uint32_t>(x + (y - x) * t + 0.5f) & 0xffu;
    };
    return 0xff000000u | (lerp(ch(a, 16), ch(b, 16)) << 16) | (lerp(ch(a, 8), ch(b, 8)) << 8) |
           lerp(ch(a, 0), ch(b, 0));
}

constexpr uint32_t region_body(uint32_t palette) { return mix(palette, kSunken, 0.62f); }
constexpr uint32_t region_body_selected(uint32_t palette) { return mix(palette, kSunken, 0.45f); }
constexpr uint32_t region_name_band(uint32_t palette) { return mix(palette, kSunken, 0.40f); }
constexpr uint32_t region_wave(uint32_t palette) { return mix(palette, 0xffffffffu, 0.35f); }
constexpr uint32_t region_muted(uint32_t) { return 0xff2a2e34; }

}  // namespace sonora::ui::theme
