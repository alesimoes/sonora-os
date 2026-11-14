// SPDX-License-Identifier: GPL-3.0-or-later
//
// Export Stems (interchange.stems): the Bounce window's Stems section, and
// the held notice naming the folder and the file count once it is done.
#include <QQuickItem>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

void open(ShotContext& c) {
    c.model.invoke(QStringLiteral("window.bounce"), -1);
    if (auto* name = c.view.rootObject()->findChild<QQuickItem*>(QStringLiteral("bounceName"))) {
        name->setProperty("text", QStringLiteral("Mix"));
    }
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("stems");
    s.take = false;
    s.apply = [](ShotContext& c) { open(c); };
    return s;
}()};

const ShotRegistrar done_registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("stems-done");
    s.take = false;
    s.apply = [](ShotContext& c) {
        open(c);
        c.model.startStems(false, QStringLiteral("track"), {}, QStringLiteral("bwf"), 24, 0,
                           QStringLiteral("auto"), QStringLiteral("Mix"));
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
