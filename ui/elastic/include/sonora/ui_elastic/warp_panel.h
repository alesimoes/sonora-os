// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Warp panel's QML (ui/elastic/qml/WarpPanel.qml) as text, built into
// the binary. A loader gives it kWarpPanelUrl, so its implicit imports
// (SButton beside it, the Sonora module) resolve as for ui/qt's QML.
#pragma once

namespace sonora::ui {

constexpr const char* kWarpPanelUrl = "qrc:/sonora/qml/WarpPanel.qml";

const char* warp_panel_qml();

}  // namespace sonora::ui
