// SPDX-License-Identifier: GPL-3.0-or-later
//
// Editing on the Tempo and Meter rulers (ADR-0140), on the demo's tempo map
// (120 4/4, 96 from bar 17, 3/4 from bar 25). Each picture drives the
// ruler's editor (ConductorRulerEdit.qml) the way the mouse does:
//   edit-conductor-drag     bar 17's mark held 80 px up, the tooltip reading
//                           the tempo the release would set
//   edit-conductor-value    a double-click on that mark: its value typed inline
//   edit-conductor-meter    a double-click on the Meter ruler at bar 9: the
//                           picker, 7/8 chosen
//   edit-conductor-refusal  132 bpm added at bar 9, then Delete on the first
//                           mark: refused, the reason held beside the ruler
// Built into sonora-ui by ui/conductor/qt/CMakeLists.txt, beside the scenes
// under ui/qt/src/scenes, which register the same way.
#include "scene_item.h"
#include "screenshot_scene.h"

namespace sonora::ui {
namespace {

// The rulers are a Repeater's delegates: visual children only, which
// findChild() does not walk.
QQuickItem* item_named(QQuickItem* item, const QString& name) {
    if (item == nullptr) return nullptr;
    if (item->objectName() == name) return item;
    for (QQuickItem* child : item->childItems()) {
        if (QQuickItem* f = item_named(child, name)) return f;
    }
    return nullptr;
}

ConductorRulerItem* ruler_of(ShotContext& c, const char* name) {
    return qobject_cast<ConductorRulerItem*>(item_named(c.view.rootObject(), QString::fromLatin1(name)));
}

// The editor EditWindow laid over the ruler.
QQuickItem* editor_of(ShotContext& c, const char* name) {
    ConductorRulerItem* r = ruler_of(c, name);
    if (r == nullptr) return nullptr;
    const QString want = QString::fromLatin1(name) + QStringLiteral("Edit");
    for (QQuickItem* child : r->childItems()) {
        if (child->objectName() == want) return child;
    }
    return nullptr;
}

QQuickItem* tempo_editor(ShotContext& c) {
    QQuickItem* e = editor_of(c, "tempoRuler");
    if (e == nullptr) std::fprintf(stderr, "sonora-ui: conductor scene: no tempo ruler editor\n");
    return e;
}

void call(QQuickItem* e, const char* fn, QVariant a = {}, QVariant b = {}) {
    if (e == nullptr) return;
    const bool ok = !b.isValid() ? (!a.isValid() ? QMetaObject::invokeMethod(e, fn)
                                                 : QMetaObject::invokeMethod(e, fn, Q_ARG(QVariant, a)))
                                 : QMetaObject::invokeMethod(e, fn, Q_ARG(QVariant, a), Q_ARG(QVariant, b));
    if (!ok) std::fprintf(stderr, "sonora-ui: conductor scene: %s failed\n", fn);
}

// Bar `n`'s x on the rulers, at 120 4/4 (the map up to bar 17).
qreal bar_x(ShotContext& c, int n) {
    const EditController& ctl = c.session.controller();
    const auto bar = static_cast<model::Frames>(2 * ctl.snapshot().settings.sample_rate);
    return ctl.view().x_of((n - 1) * bar);
}

ShotScene conductor_scene(const char* name, std::function<void(ShotContext&)> then) {
    ShotScene s;
    s.name = QString::fromLatin1(name);
    s.tempo_map = true;
    s.take = false;
    s.apply = [then = std::move(then)](ShotContext& c) { when_open(c, then); };
    return s;
}

const ShotRegistrar drag_scene{conductor_scene("edit-conductor-drag", [](ShotContext& c) {
    QQuickItem* e = tempo_editor(c);
    ConductorRulerItem* r = ruler_of(c, "tempoRuler");
    if (e == nullptr || r == nullptr) return;
    const qreal x = r->markX(1), y = r->height() / 2;
    call(e, "pressAt", x, y);
    call(e, "moveTo", y - 80, false);
})};

const ShotRegistrar value_scene{conductor_scene("edit-conductor-value", [](ShotContext& c) {
    QQuickItem* e = tempo_editor(c);
    ConductorRulerItem* r = ruler_of(c, "tempoRuler");
    if (e == nullptr || r == nullptr) return;
    call(e, "openField", 1, r->markX(1));
})};

const ShotRegistrar meter_scene{conductor_scene("edit-conductor-meter", [](ShotContext& c) {
    QQuickItem* e = editor_of(c, "meterRuler");
    if (e == nullptr) {
        std::fprintf(stderr, "sonora-ui: conductor scene: no meter ruler editor\n");
        return;
    }
    call(e, "openPicker", bar_x(c, 9) + 3);
    e->setProperty("pickBeats", 7);
    e->setProperty("pickNote", 8);
})};

const ShotRegistrar refusal_scene{conductor_scene("edit-conductor-refusal", [](ShotContext& c) {
    QQuickItem* e = tempo_editor(c);
    ConductorRulerItem* r = ruler_of(c, "tempoRuler");
    if (e == nullptr || r == nullptr) return;
    c.session.rulerClick(bar_x(c, 9));
    const QString why = c.session.conductorTypeTempo(-1, QStringLiteral("132"));
    if (!why.isEmpty()) std::fprintf(stderr, "sonora-ui: conductor scene: %s\n", qPrintable(why));
    call(e, "pressAt", r->markX(0) + 1, 4.0);
    call(e, "release");
    call(e, "removeSelected");
})};

}  // namespace
}  // namespace sonora::ui
