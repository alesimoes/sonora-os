// SPDX-License-Identifier: GPL-3.0-or-later
//
// File > Save as Template's dialog, as app.save_as_template without a name
// opens it: the first free name offered, the templates folder it goes in,
// and the "Markers" check box, off by default (ADR-0030 §2); a category
// typed in, which saves it one folder deeper (ADR-0146 §4).
#include <QQuickItem>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-save-as-template");
    s.take = false;
    s.apply = [](ShotContext& c) {
        c.model.invoke(QStringLiteral("app.save_as_template"), -1);
        if (auto* category = c.view.rootObject()->findChild<QQuickItem*>(QStringLiteral("saveAsCategory")))
            category->setProperty("text", QStringLiteral("Band"));
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
