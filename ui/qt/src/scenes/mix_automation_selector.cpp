// SPDX-License-Identifier: GPL-3.0-or-later
//
// The automation mode selector (ADR-0027) open on the first strip: the five
// modes the engine plays, the current one lit, and Trim dimmed because the
// engine refuses it. The next strips show Touch, Latch and Write lit.
#include <QCoreApplication>
#include <QEventLoop>
#include <QCoreApplication>
#include <QEventLoop>
#include <QObject>
#include <QThread>
#include <QThread>
#include <cstdio>
#include <QQuickItem>
#include <string>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

QQuickItem* find(QQuickItem* root, const QString& name) {
    if (root == nullptr) return nullptr;
    if (root->objectName() == name) return root;
    for (QQuickItem* c : root->childItems()) {
        if (QQuickItem* hit = find(c, name)) return hit;
    }
    return nullptr;
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-automation-selector");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.apply = [](ShotContext& c) {
        EditController& edit = c.session.controller();
        std::string why;
        const model::AutomationMode modes[] = {model::AutomationMode::Read,
                                               model::AutomationMode::Touch,
                                               model::AutomationMode::Latch,
                                               model::AutomationMode::Write,
                                               model::AutomationMode::Off};
        for (int t = 0; t < 5 && t < c.session.trackCount(); ++t) {
            // The session may still be opening: let the engine answer, a
            // few seconds at most, rather than draw a picture of Read.
            for (int tries = 0; tries < 50 && !edit.set_automation_mode(t, modes[t], why); ++tries) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
                QThread::msleep(100);
            }
        }
        QQuickItem* sel = find(c.view.rootObject(), QStringLiteral("autoMode0"));
        if (sel == nullptr) std::fprintf(stderr, "sonora-ui: no strip selector found\n");
        else sel->setProperty("open", true);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
