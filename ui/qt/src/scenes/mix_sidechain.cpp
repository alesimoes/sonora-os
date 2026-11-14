// SPDX-License-Identifier: GPL-3.0-or-later
//
// The side-chain key (ADR-0058) in the Mix window: the compressor on track 9
// is keyed by the Delay bus and shows its name, the other compressors show
// the plain "K" button, a stale key shows "K!", and the key chooser is open
// on that stale one ("Key: (missing)"), listing None and every aux bus.
#include <QMetaObject>
#include <QQmlProperty>
#include <QQuickItem>
#include <QTimer>
#include <QVariantMap>
#include <cstdio>

#include "sonora/model/insert_key.h"
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

model::Id bus_named(const model::Snapshot& s, const std::string& name) {
    for (const auto& b : s.edit->buses) {
        if (b.name == name) return b.id;
    }
    return 0;
}

// The Mix page is built a moment after the scene starts, so the slots are
// looked for again until they exist. A bus that is gone clears the keys on it
// in the model, so the "missing" state only arrives with a session file
// saved elsewhere: the second slot is given that state here, in the view.
// Delegates of a Repeater hang off the visual tree, not the QObject one that
// findChild walks.
QQuickItem* find_item(QQuickItem* from, const QString& name) {
    if (from->objectName() == name) return from;
    for (QQuickItem* child : from->childItems()) {
        if (QQuickItem* hit = find_item(child, name)) return hit;
    }
    return nullptr;
}

void open_chooser_when_drawn(ShotContext c, int tries) {
    QQuickItem* root = c.view.rootObject();
    QQuickItem* stale = find_item(root, QStringLiteral("insert_14_0"));
    if (stale == nullptr) {
        if (tries < 40)
            QTimer::singleShot(100, [c, tries]() mutable { open_chooser_when_drawn(c, tries + 1); });
        else
            std::fprintf(stderr, "sonora-ui: scene: the insert slots never appeared\n");
        return;
    }
    QVariantMap info = stale->property("info").toMap();
    info.insert(QStringLiteral("keyMissing"), true);
    info.insert(QStringLiteral("keyName"), QString());
    // QQmlProperty drops the strip's binding; setProperty leaves it to win again.
    QQmlProperty(stale, QStringLiteral("info")).write(info);
    QMetaObject::invokeMethod(stale, "openKey");
    std::fprintf(stderr, "sonora-ui: scene: key chooser open=%d missing=%d\n",
                 stale->property("keyOpen").toBool(), stale->property("keyMissing").toBool());
}

void key_and_open(ShotContext& c) {
    TimelineDocument& doc = c.inserts.controller().document();
    const model::Snapshot& snap = doc.snapshot();
    constexpr std::size_t kKeyedTrack = 8;
    const model::Id delay = bus_named(snap, "Delay");
    if (delay != 0 && kKeyedTrack < snap.edit->tracks.size() &&
        !snap.edit->tracks[kKeyedTrack]->inserts.empty()) {
        const auto& t = *snap.edit->tracks[kKeyedTrack];
        std::string why;
        if (!doc.submit(model::SetInsertKey{t.id, t.inserts.front().id, delay}, why)) {
            std::fprintf(stderr, "sonora-ui: scene: the key was refused: %s\n", why.c_str());
        }
    }
    // The accepted key rebuilds the strips; the slot is touched after that.
    QTimer::singleShot(1500, [c] { open_chooser_when_drawn(c, 0); });
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-sidechain");
    s.window = QStringLiteral("mix");
    s.take = false;
    // The key is accepted, the strips rebuild, then the chooser opens: more
    // than the default 5 s on a loaded CPU rasteriser.
    s.args = QStringLiteral("--after 12");
    s.apply = [](ShotContext& c) { when_open(c, key_and_open); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
