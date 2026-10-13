// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtQml>

#include "meter_item.h"
#include "shell_model.h"

namespace sonora::ui {

// The `Sonora` QML module: the meter item, and the shell model as a type QML
// can name but not create (there is exactly one, made in main()).
inline void register_types() {
    qmlRegisterType<MeterItem>("Sonora", 1, 0, "Meter");
    qmlRegisterUncreatableType<ShellModel>("Sonora", 1, 0, "ShellModel",
                                           QStringLiteral("made once, in main()"));
}

}  // namespace sonora::ui
