// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Groove panel's QML (ui/groove/qml/GroovePanel.qml) as text, built into
// the binary. A loader gives it kGroovePanelUrl, so its implicit imports
// (SButton beside it, the Sonora module) resolve as for ui/qt's QML.
#pragma once

namespace sonora::ui {

constexpr const char* kGroovePanelUrl = "qrc:/sonora/qml/GroovePanel.qml";

const char* groove_panel_qml();

}  // namespace sonora::ui
