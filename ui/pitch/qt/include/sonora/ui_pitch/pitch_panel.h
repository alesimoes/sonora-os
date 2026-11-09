// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Pitch panel's QML (ui/pitch/qt/PitchPanel.qml) as text, built into the
// binary, as ui/groove does with its panel. A loader gives it kPitchPanelUrl,
// so its implicit imports (SButton and Knob beside it, the Sonora module)
// resolve as for ui/qt's QML. No Qt header here: only ui/qt includes Qt.
#pragma once

namespace sonora::ui_pitch {

constexpr const char* kPitchPanelUrl = "qrc:/sonora/qml/PitchPanel.qml";

const char* pitch_panel_qml();

}  // namespace sonora::ui_pitch
