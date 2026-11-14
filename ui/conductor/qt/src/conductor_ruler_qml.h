// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ruler editor's QML (ui/conductor/qt/qml/ConductorRulerEdit.qml) as text,
// built into the binary. EditWindow creates it at kConductorRulerEditUrl, so
// its implicit imports (Theme, SButton) resolve as for ui/qt's QML.
#pragma once

namespace sonora::ui {

constexpr const char* kConductorRulerEditUrl = "qrc:/sonora/qml/ConductorRulerEdit.qml";

const char* conductor_ruler_qml();

}  // namespace sonora::ui
