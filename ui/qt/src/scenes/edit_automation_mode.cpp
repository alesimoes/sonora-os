// SPDX-License-Identifier: GPL-3.0-or-later
//
// The automation mode on the Edit window's track headers: the first four
// tracks stepped with track.step_automation_mode (the command the
// ctrl+alt+m key sends) to Touch, Latch, Write and Off, and the first
// header's list open.
#include <QQuickItem>
#include <cstdio>

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
    s.name = QStringLiteral("edit-automation-mode");
    s.window = QStringLiteral("edit");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            const QString step = QStringLiteral("track.step_automation_mode");
            // Read is the default: one step is Touch, two Latch, three Write
            // and four Off for the tracks 0..3 (track 0 stays on Touch).
            for (int t = 0; t < 4 && t < c.session.trackCount(); ++t) {
                for (int n = 0; n < (t == 3 ? 4 : t + 1); ++n) c.model.invoke(step, t);
            }
            QQuickItem* sel = find(c.view.rootObject(), QStringLiteral("editAutoMode0"));
            if (sel == nullptr) std::fprintf(stderr, "sonora-ui: no header selector found\n");
            else sel->setProperty("open", true);
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
