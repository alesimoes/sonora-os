// SPDX-License-Identifier: GPL-3.0-or-later
//
// The side-chain key (ADR-0058) in the Mix window: the compressor on track 9
// is keyed by the Delay bus and shows its name, the other compressors show
// the plain "K" button, and the key chooser is open on the first track's
// compressor, listing None and every aux bus.
#include <QMetaObject>
#include <QQuickItem>

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
    if (QQuickItem* slot =
            c.view.rootObject()->findChild<QQuickItem*>(QStringLiteral("insert_0_0"))) {
        QMetaObject::invokeMethod(slot, "openKey");
    }
}

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("mix-sidechain");
    s.window = QStringLiteral("mix");
    s.take = false;
    s.apply = [](ShotContext& c) { when_open(c, key_and_open); };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
