// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window with I/O, inserts and sends collapsed: their height is the
// fader's (ui-design.md, "Mixer strip anatomy"). A plugin in a collapsed
// bank shows as "+N".
//
// The second scene hides three strips and opens the Strips menu (ADR-0130):
// the cards after a hidden one close up, the badge counts them.
#include <QMetaObject>
#include <QQuickItem>
#include <QVariant>

#include "mix_model.h"
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

// The Mix window's own item: the one that holds the hidden set.
QQuickItem* mix_window(QQuickItem* item) {
    if (item->property("hiddenIds").isValid()) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = mix_window(c)) return f;
    }
    return nullptr;
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-views-collapsed");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.apply = [](ShotContext& c) {
        auto* mixer = c.view.rootObject()->property("mixer").value<MixModel*>();
        if (mixer == nullptr) return;
        mixer->setShowIO(false);
        mixer->setShowInsertsAE(false);
        mixer->setShowSends(false);
    };
    return s;
}()};

const ShotRegistrar hidden_registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("views-hidden");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.apply = [](ShotContext& c) {
        QQuickItem* mix = mix_window(c.view.rootObject());
        if (mix == nullptr) return;
        for (const char* key : {"t1", "t2", "t5"})
            QMetaObject::invokeMethod(mix, "setStripHidden", Q_ARG(QVariant, QString::fromLatin1(key)),
                                      Q_ARG(QVariant, true));
        if (QQuickItem* menu = mix->findChild<QQuickItem*>(QStringLiteral("stripsPopup")))
            QMetaObject::invokeMethod(menu, "toggle");
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
