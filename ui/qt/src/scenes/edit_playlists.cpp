// SPDX-License-Identifier: GPL-3.0-or-later
//
// The playlist selector (ADR-0026): the selected track's playlist
// duplicated, the copy renamed "Verse comp" and active, and the playlist
// menu open on its header, listing both with the active one ticked.
#include <QQuickItem>
#include <QTimer>
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

// A Repeater's delegates are visual children only, which findChild()
// does not walk.
QQuickItem* item_named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = item_named(c, name)) return f;
    }
    return nullptr;
}

void open_playlist_menu(ShotContext& c) {
    QQuickItem* root = c.view.rootObject();
    const QString name = QStringLiteral("playlistSelector") + QString::number(c.track);
    QQuickItem* selector = item_named(root, name);
    auto* menu = root->findChild<QQuickItem*>(QStringLiteral("playlistMenu"));
    if (selector == nullptr || menu == nullptr) return;
    const QPointF at = selector->mapToScene(QPointF(0, selector->height()));
    QMetaObject::invokeMethod(menu, "show", Q_ARG(QVariant, c.track), Q_ARG(QVariant, at.x()),
                              Q_ARG(QVariant, at.y()));
}

// The demo session may still be opening on the engine, which refuses edits
// until it has answered: the first one is asked again until it goes.
void build(ShotContext c, int tries) {
    if (!c.model.invokeWith(QStringLiteral("track.playlist_duplicate"), c.track, QString())) {
        if (tries < 30) {
            QTimer::singleShot(100, [c, tries]() { build(c, tries + 1); });
        } else {
            std::fprintf(stderr, "sonora-ui: track.playlist_duplicate: %s\n",
                         qPrintable(c.model.statusText()));
        }
        return;
    }
    if (!c.model.invokeWith(QStringLiteral("track.playlist_rename"), c.track,
                            QStringLiteral("Verse comp"))) {
        std::fprintf(stderr, "sonora-ui: track.playlist_rename: %s\n",
                     qPrintable(c.model.statusText()));
        return;
    }
    open_playlist_menu(c);
}

const ShotRegistrar playlists{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-playlists");
    s.take = false;
    s.apply = [](ShotContext& c) {
        c.model.invoke(QStringLiteral("track.select"), c.track);
        build(c, 0);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
