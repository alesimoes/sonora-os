// SPDX-License-Identifier: GPL-3.0-or-later
//
// The automation mode selector (ADR-0027) open on the first strip: the five
// modes the engine plays, the current one lit, the keyboard's row ringed,
// and Trim dimmed because the engine refuses it. The next strips show
// Touch, Latch and Write lit.
#include <QQuickItem>
#include <cstdio>
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
    s.apply = [](ShotContext& c0) { when_open(c0, [](ShotContext& c) {
        EditController& edit = c.session.controller();
        std::string why;
        const model::AutomationMode modes[] = {model::AutomationMode::Read,
                                               model::AutomationMode::Touch,
                                               model::AutomationMode::Latch,
                                               model::AutomationMode::Write,
                                               model::AutomationMode::Off};
        for (int t = 0; t < 5 && t < c.session.trackCount(); ++t) {
            if (!edit.set_automation_mode(t, modes[t], why))
                std::fprintf(stderr, "sonora-ui: automation mode: %s\n", why.c_str());
        }
        QQuickItem* sel = find(c.view.rootObject(), QStringLiteral("autoMode0"));
        if (sel == nullptr) {
            std::fprintf(stderr, "sonora-ui: no strip selector found\n");
            return;
        }
        sel->setProperty("open", true);
        // One step down from Read, as a Down key would: the ring shows.
        sel->setProperty("hot", 2);
    }); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
