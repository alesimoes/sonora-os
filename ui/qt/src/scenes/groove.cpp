// SPDX-License-Identifier: GPL-3.0-or-later
//
// Groove (ADR-0115). Bars 5 and 6 of the snare are cut into eighth-note
// hits, as a beat-sliced loop leaves them, and zoomed to. "edit-groove" has
// the Groove panel open over them with a preset chosen and a saved groove
// listed; "edit-groove-applied" is the same hits after an eighth shuffle at
// 66%, every second one late by a third of its slot.
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QUrl>
#include <QVariantMap>

#include "screenshot_scene.h"
// Only where ui/groove is configured (the Linux build), as in edit_tempo.cpp.
#if __has_include("sonora/ui_groove/groove_command.h")
#include "sonora/ui_groove/groove_command.h"

namespace sonora::ui {
namespace {

// The snare region under bar 5, cut into eighths: each keeps its first half
// and the rest is removed, right to left so the left part keeps the id.
void slice_snare(ShotContext& c) {
    auto& ctl = c.session.controller();
    const model::TrackEdit* t = ctl.snapshot().edit->tracks[static_cast<std::size_t>(c.track)].get();
    const model::Frames bar = 2 * static_cast<model::Frames>(ctl.snapshot().settings.sample_rate);
    const model::Frames from = 4 * bar, eighth = bar / 8;
    const model::Region* r = nullptr;
    for (const model::Region& x : c.playlist->regions) {
        if (x.position <= from && x.end() >= from + 2 * bar) r = &x;
    }
    std::string why;
    if (r == nullptr) {
        std::fprintf(stderr, "sonora-ui: groove scene: no snare region under bars 5-6\n");
        return;
    }
    TimelineDocument& doc = ctl.document();
    std::vector<model::Member> ops;
    for (int k = 15; k >= 0; --k) {
        const model::Frames gap_from = from + k * eighth + eighth / 2, gap_to = from + (k + 1) * eighth;
        const model::Id gap = doc.reserve_id();
        ops.push_back(model::SplitRegion{t->active_playlist, r->id, gap_to, doc.reserve_id()});
        ops.push_back(model::SplitRegion{t->active_playlist, r->id, gap_from, gap});
        ops.push_back(model::RemoveRegion{t->active_playlist, gap});
    }
    ops.push_back(model::SplitRegion{t->active_playlist, r->id, from, doc.reserve_id()});
    if (!doc.submit(model::Operation{model::Compound{std::move(ops), "Slice"}}, why)) {
        std::fprintf(stderr, "sonora-ui: groove scene: slicing: %s\n", why.c_str());
        return;
    }
    ctl.select_range(c.track, from, from + 2 * bar);
    if (!ctl.zoom_fit_selection(why)) std::fprintf(stderr, "sonora-ui: groove scene: %s\n", why.c_str());
}

void open_panel(ShotContext& c) {
    QQmlComponent comp(c.view.engine(), QUrl(QStringLiteral("qrc:/sonora/qml/GroovePanel.qml")));
    QVariantMap props;
    props.insert(QStringLiteral("shell"), QVariant::fromValue(static_cast<QObject*>(&c.model)));
    props.insert(QStringLiteral("saved"), QStringList{QStringLiteral("Late pocket")});
    props.insert(QStringLiteral("current"), 4);
    props.insert(QStringLiteral("strength"), 80);
    QObject* o = comp.createWithInitialProperties(props);
    auto* panel = qobject_cast<QQuickItem*>(o);
    if (panel == nullptr) {
        std::fprintf(stderr, "sonora-ui: groove scene: %s\n", qPrintable(comp.errorString()));
        delete o;
        return;
    }
    panel->setParent(c.view.rootObject());
    panel->setParentItem(c.view.rootObject());
    QMetaObject::invokeMethod(panel, "show");
}

const ShotRegistrar panel_scene{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-groove");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            slice_snare(c);
            open_panel(c);
        });
    };
    return s;
}()};

const ShotRegistrar applied_scene{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-groove-applied");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            slice_snare(c);
            std::string why;
            if (!groove_command(c.session.controller(), "groove swing 66 8 100", why)) {
                std::fprintf(stderr, "sonora-ui: groove scene: %s\n", why.c_str());
            }
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
#endif
