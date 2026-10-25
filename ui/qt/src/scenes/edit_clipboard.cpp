// SPDX-License-Identifier: GPL-3.0-or-later
//
// The snare's two crossfaded regions copied and pasted after the next
// track's last region, through the keys' commands, with the Edit menu open
// on "Undo Paste".
#include <QQuickItem>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-clipboard");
    s.take = false;
    s.apply = [](ShotContext& c) {
        const auto* p = c.playlist;
        const int track = c.track;
        if (p == nullptr || p->regions.size() <= 2 || c.session.trackCount() <= track + 1) return;
        EditController& edit = c.session.controller();
        const auto from = p->regions[1].position, to = p->regions[2].end();
        edit.select_range(track, from, to);
        c.model.invoke(QStringLiteral("edit.copy"), -1);
        const auto* next = active_playlist(edit.snapshot().edit->tracks[track + 1].get());
        const auto after = next && !next->regions.empty() ? next->regions.back().end() : to;
        c.model.invoke(QStringLiteral("track.select"), track + 1);
        edit.set_insertion(after + (to - from) / 10);
        c.model.invoke(QStringLiteral("edit.paste"), -1);
        c.model.invoke(QStringLiteral("zoom.fit_session"), -1);
        QQuickItem* root = c.view.rootObject();
        auto* button = root->findChild<QQuickItem*>(QStringLiteral("editMenuButton"));
        auto* menu = root->findChild<QQuickItem*>(QStringLiteral("editMenu"));
        if (button != nullptr && menu != nullptr) {
            const QPointF at = button->mapToScene(QPointF(0, button->height()));
            menu->setProperty("x", at.x());
            menu->setProperty("y", at.y() + 4);
            menu->setProperty("visible", true);
        }
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
