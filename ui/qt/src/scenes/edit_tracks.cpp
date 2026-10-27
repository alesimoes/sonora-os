// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Track menu (ADR-0023): the last two tracks deleted to make room on
// the engine, the snare duplicated below itself with its regions, a new
// audio track below the copy, and the Track menu open on it. Then, as a
// second scene, the New Tracks dialog asking how many.
#include <QQuickItem>
#include <QTimer>
#include <cstdio>

#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

void open_track_menu(ShotContext& c) {
    QQuickItem* root = c.view.rootObject();
    auto* button = root->findChild<QQuickItem*>(QStringLiteral("trackMenuButton"));
    auto* menu = root->findChild<QQuickItem*>(QStringLiteral("trackMenu"));
    if (button == nullptr || menu == nullptr) return;
    const QPointF at = button->mapToScene(QPointF(0, button->height()));
    menu->setProperty("x", at.x());
    menu->setProperty("y", at.y() + 4);
    menu->setProperty("visible", true);
}

bool step(ShotContext& c, const char* command, int target, const QString& argument = {}) {
    if (c.model.invokeWith(QString::fromLatin1(command), target, argument)) return true;
    std::fprintf(stderr, "sonora-ui: %s: %s\n", command, qPrintable(c.model.statusText()));
    return false;
}

// The demo session may still be opening on the engine, which refuses edits
// until it has answered: the first one is asked again until it goes,
// within the capture's wait.
void build(ShotContext c, int tries) {
    const int last = c.session.trackCount() - 1;
    if (!c.model.invokeWith(QStringLiteral("track.delete"), last, QString())) {
        if (tries < 30) {
            QTimer::singleShot(100, [c, tries]() { build(c, tries + 1); });
        } else {
            std::fprintf(stderr, "sonora-ui: track.delete: %s\n", qPrintable(c.model.statusText()));
        }
        return;
    }
    // The demo fills every engine track: a second row makes room for two.
    if (!step(c, "track.delete", c.session.trackCount() - 1)) return;
    if (!step(c, "track.duplicate", c.track)) return;
    if (!step(c, "track.new", -1, QStringLiteral("1"))) return;
    c.model.invoke(QStringLiteral("zoom.fit_session"), -1);
    open_track_menu(c);
}

const ShotRegistrar tracks{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-tracks");
    s.take = false;
    s.apply = [](ShotContext& c) {
        if (c.session.trackCount() <= c.track + 2) return;
        build(c, 0);
    };
    return s;
}()};

const ShotRegistrar new_tracks{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-new-tracks");
    s.take = false;
    s.apply = [](ShotContext& c) {
        c.model.invoke(QStringLiteral("track.select"), c.track);
        c.model.invoke(QStringLiteral("track.new"), -1);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
