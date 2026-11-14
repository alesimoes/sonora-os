// SPDX-License-Identifier: GPL-3.0-or-later
//
// The loop browser over the edit window: a small catalog on fakes, the
// drums category chosen, one loop auditioning and another selected.
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QVariantMap>
#include <memory>

#include "loop_browser_model.h"
#include "screenshot_scene.h"
#include "sonora/loops/fake_loops.h"

namespace sonora::ui {
namespace {

loops::LoopEntry loop(const char* id, const char* name, const char* cat, double bpm, const char* key,
                      std::vector<std::string> tags) {
    loops::LoopEntry e;
    e.id = id;
    e.name = name;
    e.path = std::string(id) + ".wav";
    e.category = cat;
    e.bpm = bpm;
    e.key = key;
    e.beats = 8;
    e.tags = std::move(tags);
    return e;
}

struct Host {
    loops::FakeLoopSource source{{
        loop("d1", "Dusty Kit 92", "Drums", 92, "", {"dusty", "breaks"}),
        loop("d2", "Tight Pocket 120", "Drums", 120, "", {"tight"}),
        loop("d3", "Half-Time Snare 120", "Drums", 120, "", {"snare"}),
        loop("d4", "Brush Swing 120", "Drums", 120, "", {"brush", "jazz"}),
        loop("b1", "Round Sub 120", "Bass", 120, "A min", {"sub"}),
        loop("g1", "Muted Chops 100", "Guitar", 100, "E min", {"funk"}),
    }};
    loops::FakeLoopAudition audition;
    loops::FakeLoopInserter inserter;
    std::unique_ptr<LoopBrowserModel> model;
};

const ShotRegistrar registrar{[] {
    ShotScene s;
    s.name = QStringLiteral("loop-browser");
    s.take = false;
    s.apply = [](ShotContext& c) {
        auto* host = new Host;
        host->model = std::make_unique<LoopBrowserModel>(
            host->source, host->audition, host->inserter,
            LoopBrowserModel::Context{[] { return 120.0; }, [] { return 0; }, [] { return 0; }},
            &c.view);
        host->model->setCategory(QStringLiteral("Drums"));
        host->model->audition(QStringLiteral("d2"));
        host->model->select(QStringLiteral("d3"));
        QQmlComponent comp(c.view.engine(), QUrl(QStringLiteral("qrc:/sonora/qml/LoopBrowser.qml")));
        QVariantMap props;
        props.insert(QStringLiteral("browser"), QVariant::fromValue(static_cast<QObject*>(host->model.get())));
        QObject* o = comp.createWithInitialProperties(props);
        auto* panel = qobject_cast<QQuickItem*>(o);
        if (panel == nullptr) {
            std::fprintf(stderr, "sonora-ui: loop scene: %s\n", qPrintable(comp.errorString()));
            delete o;
            return;
        }
        panel->setParent(c.view.rootObject());
        panel->setParentItem(c.view.rootObject());
        panel->setX((c.view.rootObject()->width() - panel->width()) / 2);
        panel->setY((c.view.rootObject()->height() - panel->height()) / 2);
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
