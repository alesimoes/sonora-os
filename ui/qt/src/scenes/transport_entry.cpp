// SPDX-License-Identifier: GPL-3.0-or-later
//
// Counter entry: the Transport window open on the demo's tempo map, its
// counter clicked and a position typed, the caption saying what it takes.
// Enter would locate there.
#include <QMetaObject>
#include <QQuickItem>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("transport-entry");
    s.tempo_map = true;
    s.take = false;
    s.setup = [](ShotContext& c) { c.transport.setOpen(true); };
    s.apply = [](ShotContext& c) {
        QQuickItem* root = c.view.rootObject();
        auto* counter = root->findChild<QQuickItem*>(QStringLiteral("transportCounter"));
        if (counter == nullptr) return;
        QMetaObject::invokeMethod(counter, "begin");
        if (auto* input = root->findChild<QQuickItem*>(QStringLiteral("transportCounterInput"))) {
            input->setProperty("text", QStringLiteral("25|3|480"));
        }
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
