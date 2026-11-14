// SPDX-License-Identifier: GPL-3.0-or-later
//
// Smart Controls (ADR-0117) over the Edit window, on the demo's inserts:
// "smart-controls" with three macros mapped, one of them to the reverb that
// is not installed (unresolved, shown and counted); "smart-controls-learn"
// with a macro armed and a parameter being learned; "smart-controls-corrupt"
// with the held notice a sidecar file that cannot be read leaves.
//
// The panel is not in the Edit window's layout yet, so the scene lays it
// over the bottom of the window, where Logic keeps it.
#if __has_include("smart_controls_model.h")
#include <QDir>
#include <QFile>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTemporaryDir>

#include "screenshot_scene.h"
#include "smart_controls_model.h"

namespace sonora::ui {
namespace {

namespace sc = smart_controls;

sc::Mapping map(model::Id insert, const char* key, float lo, float hi, bool invert = false) {
    sc::Mapping m;
    m.insert = insert;
    m.key = key;
    m.lo = lo;
    m.hi = hi;
    m.invert = invert;
    return m;
}

// The demo's mappings: the kick's compressor and gain, the snare's gain, the
// missing reverb.
SmartControlsModel* make_model(ShotContext& c) {
    InsertController& ctl = c.inserts.controller();
    auto* smart = new SmartControlsModel(ctl, &c.view);
    const char* names[] = {"Squash", "Thump", "Room", "Air", "Width", "Drive", "Tone", "Level"};
    for (int i = 0; i < 8; ++i) smart->renameMacro(i, QString::fromLatin1(names[i]));
    std::string why;
    const model::Id kick_comp = ctl.slot(0, 0).insert, kick_gain = ctl.slot(0, 1).insert;
    const model::Id snare_gain = ctl.slot(1, 0).insert, reverb = ctl.slot(15, 0).insert;
    smart->addMapping(0, map(kick_comp, "0", 0.62f, 0.18f), why);
    smart->addMapping(0, map(kick_comp, "1", 0.20f, 0.75f), why);
    smart->addMapping(0, map(kick_gain, "gain", -3.0f, 4.0f), why);
    smart->addMapping(1, map(snare_gain, "gain", -6.0f, 6.0f), why);
    smart->addMapping(2, map(reverb, "mix", 0.0f, 0.6f), why);
    smart->addMapping(2, map(kick_comp, "1", 0.1f, 0.4f, true), why);
    smart->setMacroValue(0, 0.45);
    smart->setMacroValue(1, 0.6);
    return smart;
}

void show_panel(ShotContext& c, SmartControlsModel* smart) {
    QQuickItem* root = c.view.rootObject();
    if (root == nullptr) return;
    QQmlComponent component(c.view.engine(), QUrl(QStringLiteral("qrc:/sonora/qml/SmartControlsPanel.qml")));
    QObject* made = component.createWithInitialProperties(
        {{QStringLiteral("smart"), QVariant::fromValue(static_cast<QObject*>(smart))}});
    auto* panel = qobject_cast<QQuickItem*>(made);
    if (panel == nullptr) {
        std::fprintf(stderr, "sonora-ui: scene: Smart Controls panel: %s\n",
                     qPrintable(component.errorString()));
        delete made;
        return;
    }
    panel->setParent(root);
    panel->setParentItem(root);
    panel->setZ(1000);
    panel->setX((root->width() - panel->width()) / 2);
    panel->setY(root->height() - panel->height() - 48);
}

const ShotRegistrar panel{[] {
    ShotScene s;
    s.name = QStringLiteral("smart-controls");
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            SmartControlsModel* smart = make_model(c);
            smart->select(2);
            smart->select(0);
            show_panel(c, smart);
        });
    };
    return s;
}()};

const ShotRegistrar learn{[] {
    ShotScene s;
    s.name = QStringLiteral("smart-controls-learn");
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            SmartControlsModel* smart = make_model(c);
            // A drag in the bass's compressor panel while Width is armed:
            // what learnTap() hands on, mid-touch.
            const model::Id bass_comp = c.inserts.controller().slot(8, 0).insert;
            smart->learn(4);
            smart->touched(bass_comp, "1", 0.42f);
            smart->touched(bass_comp, "1", 0.81f);
            show_panel(c, smart);
        });
    };
    return s;
}()};

const ShotRegistrar corrupt{[] {
    ShotScene s;
    s.name = QStringLiteral("smart-controls-corrupt");
    s.apply = [](ShotContext& c) {
        when_open(c, [](ShotContext& c) {
            auto* dir = new QTemporaryDir;
            QFile f(QDir(dir->path()).filePath(QString::fromLatin1(SmartControlsModel::kFileName)));
            if (f.open(QIODevice::WriteOnly)) f.write("{\"schema\": 1, \"macros\": [");
            f.close();
            auto* smart = new SmartControlsModel(c.inserts.controller(), &c.view);
            smart->setSessionFolder(dir->path());
            QObject::connect(smart, &QObject::destroyed, [dir]() { delete dir; });
            show_panel(c, smart);
        });
    };
    return s;
}()};

}  // namespace
}  // namespace sonora::ui
#endif
