// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QtQml>

#include "insert_model.h"
#include "meter_item.h"
#include "mix_model.h"
#include "scene_item.h"
#include "session_model.h"
#include "shell_model.h"
#include "theme_object.h"
#include "transport_model.h"

namespace sonora::ui {

// The `Sonora` QML module: the scene-graph items (meter, timeline, ruler),
// the two models as types QML can name but not create (there is exactly one
// of each, made in main()), and the Theme singleton.
inline void register_types() {
    qmlRegisterType<MeterItem>("Sonora", 1, 0, "Meter");
    qmlRegisterType<TimelineItem>("Sonora", 1, 0, "Timeline");
    qmlRegisterType<RulerItem>("Sonora", 1, 0, "Ruler");
    qmlRegisterUncreatableType<TransportModel>("Sonora", 1, 0, "TransportModel",
                                               QStringLiteral("made once, in main()"));
    qmlRegisterUncreatableType<ShellModel>("Sonora", 1, 0, "ShellModel",
                                           QStringLiteral("made once, in main()"));
    qmlRegisterUncreatableType<SessionModel>("Sonora", 1, 0, "SessionModel",
                                             QStringLiteral("made once, in main()"));
    qmlRegisterUncreatableType<InsertModel>("Sonora", 1, 0, "InsertModel",
                                            QStringLiteral("made once, in main()"));
    qmlRegisterUncreatableType<MixModel>("Sonora", 1, 0, "MixModel",
                                         QStringLiteral("made once, in main()"));
    qmlRegisterSingletonType<ThemeObject>("Sonora", 1, 0, "Theme",
                                          [](QQmlEngine*, QJSEngine*) -> QObject* {
                                              return new ThemeObject;
                                          });
}

}  // namespace sonora::ui
