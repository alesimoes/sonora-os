// SPDX-License-Identifier: GPL-3.0-or-later
//
// File > New Session's dialog: a name typed, the sessions folder it goes
// in, BWF, and the rate and depth this system runs at lit. 96 kHz has been
// clicked, and the dialog says why it cannot be had here.
#include <QMetaObject>
#include <QQuickItem>
#include <QVariantMap>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("new-session");
    s.take = false;
    s.apply = [](ShotContext& c) {
        QQuickItem* root = c.view.rootObject();
        auto* panel = root->findChild<QQuickItem*>(QStringLiteral("newSessionPanel"));
        if (panel == nullptr) return;
        QMetaObject::invokeMethod(panel, "show");
        if (auto* name = root->findChild<QQuickItem*>(QStringLiteral("newSessionName")))
            name->setProperty("text", QStringLiteral("Band Tracking Nov 2"));
        for (const QVariant& r : c.session.newSessionChoices()[QStringLiteral("rates")].toList()) {
            const QVariantMap m = r.toMap();
            if (m[QStringLiteral("text")].toString() == QStringLiteral("96 kHz") &&
                !m[QStringLiteral("on")].toBool()) {
                panel->setProperty("problem", m[QStringLiteral("text")].toString() +
                                                  QStringLiteral(": ") +
                                                  m[QStringLiteral("why")].toString());
            }
        }
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
