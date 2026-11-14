// SPDX-License-Identifier: GPL-3.0-or-later
//
// Elastic Audio (ADR-0136). The Warp panel is open over the snare region the
// demo selects: "edit-elastic" with three warp markers from the editor's
// marker model, "edit-elastic-quantize" on the Quantize page, and
// "edit-elastic-unavailable" as an engine built without the stretcher shows
// it, the reason and nothing to press. The first two picture the panel as
// on an engine announcing audio.elastic; the demo's fake engine does not,
// which is the third picture's state.
#if __has_include("sonora/ui_elastic/elastic_command.h")
#include "sonora/ui_elastic/elastic_command.h"
#include "sonora/ui_elastic/warp_markers.h"
#include "sonora/ui_elastic/warp_panel.h"
#define SONORA_UI_ELASTIC_SCENE 1
#endif

#include <cstdio>

#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include "screenshot_scene.h"

#ifdef SONORA_UI_ELASTIC_SCENE
namespace sonora::ui {
namespace {

// The region the demo selects (the snare's second), which the panel acts
// on; null, said on stderr, when there is none.
const model::Region* selected_snare(ShotContext& c) {
    std::string why;
    const model::Region* r = c.session.controller().selected_region(why);
    if (r == nullptr) std::fprintf(stderr, "sonora-ui: elastic scene: %s\n", why.c_str());
    return r;
}

// Three markers: the first quarter holds, the middle is pulled late, the
// last lands a little later still; what the operator would have placed.
void place_markers(WarpMarkers& m, const model::Region& r) {
    m.reset(r.id, r.length);
    std::string why;
    const model::Frames q = r.length / 4;
    if (!m.add(q, q, why) || !m.add(2 * q, 2 * q + q / 4, why) || !m.add(3 * q, 3 * q + q / 8, why)) {
        std::fprintf(stderr, "sonora-ui: elastic scene: %s\n", why.c_str());
    }
}

QVariantList marker_rows(const WarpMarkers& m, std::uint32_t rate) {
    QVariantList out;
    for (const WarpMarkerRow& row : m.rows(rate)) {
        QVariantMap line;
        line.insert(QStringLiteral("from"), QString::fromStdString(row.from));
        line.insert(QStringLiteral("to"), QString::fromStdString(row.to));
        line.insert(QStringLiteral("speed"), QString::fromStdString(row.speed));
        out.push_back(line);
    }
    return out;
}

QString warp_list(const WarpMarkers& m) {
    QStringList parts;
    for (const api::WarpMarker& x : m.markers()) {
        parts << QString::number(x.source) + QLatin1Char(':') + QString::number(x.target);
    }
    return parts.join(QLatin1Char(','));
}

void open_panel(ShotContext& c, QVariantMap props) {
    QQmlComponent comp(c.view.engine());
    comp.setData(QByteArray(warp_panel_qml()), QUrl(QString::fromLatin1(kWarpPanelUrl)));
    props.insert(QStringLiteral("shell"), QVariant::fromValue(static_cast<QObject*>(&c.model)));
    QObject* o = comp.createWithInitialProperties(props);
    auto* panel = qobject_cast<QQuickItem*>(o);
    if (panel == nullptr) {
        std::fprintf(stderr, "sonora-ui: elastic scene: %s\n", qPrintable(comp.errorString()));
        delete o;
        return;
    }
    panel->setParent(c.view.rootObject());
    panel->setParentItem(c.view.rootObject());
    QMetaObject::invokeMethod(panel, "show");
}

QVariantMap region_props(ShotContext& c, const model::Region& r) {
    const std::uint32_t rate = c.session.controller().snapshot().settings.sample_rate;
    QVariantMap props;
    props.insert(QStringLiteral("regionName"), QString::fromStdString(r.name));
    props.insert(QStringLiteral("regionLength"),
                 QString::number(static_cast<double>(r.length) / rate, 'f', 3) + QStringLiteral(" s"));
    return props;
}

const ShotRegistrar markers_scene{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-elastic");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            const model::Region* r = selected_snare(c);
            if (r == nullptr) return;
            WarpMarkers markers;
            place_markers(markers, *r);
            QVariantMap props = region_props(c, *r);
            props.insert(QStringLiteral("mode"), 1);
            props.insert(QStringLiteral("algorithm"), 1);
            props.insert(QStringLiteral("markers"),
                         marker_rows(markers, c.session.controller().snapshot().settings.sample_rate));
            props.insert(QStringLiteral("warpList"), warp_list(markers));
            open_panel(c, props);
        });
    };
    return s;
}()};

const ShotRegistrar quantize_scene{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-elastic-quantize");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            const model::Region* r = selected_snare(c);
            if (r == nullptr) return;
            QVariantMap props = region_props(c, *r);
            props.insert(QStringLiteral("mode"), 2);
            props.insert(QStringLiteral("algorithm"), 1);
            props.insert(QStringLiteral("grid"), 2);
            props.insert(QStringLiteral("strength"), 75);
            open_panel(c, props);
        });
    };
    return s;
}()};

const ShotRegistrar unavailable_scene{[] {
    ShotScene s;
    s.name = QStringLiteral("edit-elastic-unavailable");
    s.take = false;
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            const model::Region* r = selected_snare(c);
            if (r == nullptr) return;
            // The refusal itself, as elastic_command gives it on this engine.
            std::string why;
            elastic_available(ElasticDeps{}, why);
            QVariantMap props = region_props(c, *r);
            props.insert(QStringLiteral("available"), false);
            props.insert(QStringLiteral("unavailableReason"), QString::fromStdString(why));
            open_panel(c, props);
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
#endif
