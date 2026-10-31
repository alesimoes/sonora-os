// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Track menu (ADR-0023): the last two tracks deleted to make room on
// the engine, the snare duplicated below itself with its regions, a new
// audio track below the copy, and the Track menu open on it. Then, as a
// second scene, the New Tracks dialog asking how many.
#include <QQuickItem>
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

void build(ShotContext& c) {
    const int last = c.session.trackCount() - 1;
    if (!step(c, "track.delete", last)) return;
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
        when_open(c, build);
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
