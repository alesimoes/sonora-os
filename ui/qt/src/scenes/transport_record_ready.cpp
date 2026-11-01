// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pro Tools' Record only arms: the Record button on the bar and in the
// Transport window, sent as transport.record_ready, is lit in the record
// colour (the blink's lit half, held still for the picture) and the state
// pill reads READY. The take starts when Play is pressed. The demo arms its
// tracks, so the engine accepts it.
#include <QQuickItem>
#include <QQuickView>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("transport-record-ready");
    s.args = QStringLiteral("--keymap keymaps/protools.json");
    s.take = false;
    s.setup = [](ShotContext& c) { c.transport.setOpen(true); };
    s.apply = [](ShotContext& c) {
        c.model.invoke(QStringLiteral("transport.record_ready"), -1);
        for (const char* name : {"barRecord", "transportRecord"}) {
            if (auto* b = c.view.rootObject()->findChild<QQuickItem*>(QString::fromLatin1(name)))
                b->setProperty("steady", true);
        }
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
