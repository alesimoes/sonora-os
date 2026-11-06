// SPDX-License-Identifier: GPL-3.0-or-later
//
// VCA masters and an open folder (ADR-0048) in the Mix window: "Drums" and
// "Overheads" strips to the right of the returns, each with its fader and
// mute, their names on the tracks they hold (a track can be on two VCAs),
// and a folder tag on the tracks of the "Keys" folder.
#include <QQuickItem>
#include <QTimer>
#include <cstdio>

#include "mix_model.h"
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

// By the visual tree: findChild would miss items the QML parents elsewhere.
QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

bool run(ShotContext& c, const char* command, const char* argument) {
    if (c.model.invokeWith(QString::fromLatin1(command), -1, QString::fromUtf8(argument)))
        return true;
    std::fprintf(stderr, "sonora-ui: %s: %s\n", command, qPrintable(c.model.statusText()));
    return false;
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-vca");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.apply = [](ShotContext& c0) {
        when_open(c0, [](ShotContext& c) {
            auto* mixer = c.view.rootObject()->property("mixer").value<MixModel*>();
            if (mixer == nullptr) return;
            EditController& edit = c.session.controller();
            edit.select_range(0, 0, 0, 4);
            if (!run(c, "vca.new", "Drums")) return;
            edit.select_range(3, 0, 0, 3);
            if (!run(c, "vca.new", "Overheads")) return;
            edit.select_range(8, 0, 0, 3);
            if (!run(c, "folder.new", "Keys")) return;
            mixer->setVcaGainText(0, QStringLiteral("-4.5"));
            mixer->toggleVcaMute(1);
            // The VCA strips are the last on the right: scroll them into view.
            QTimer::singleShot(400, [view = &c.view] {
                QQuickItem* strips = named(view->rootObject(), QStringLiteral("mixStrips"));
                if (strips == nullptr) return;
                const double room = strips->property("contentWidth").toDouble() - strips->width();
                if (room > 0) strips->setProperty("contentX", room);
            });
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
