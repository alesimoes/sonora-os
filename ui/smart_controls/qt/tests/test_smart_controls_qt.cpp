// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Smart Controls panel's model (ADR-0117) on a held document and a
// catalog of two test plug-ins, through the insert controller's real
// gesture and its ParamSender, as the engine would receive it:
//
//   - a macro move reaches the sender once for every mapped parameter, at
//     lo + m * (hi - lo), inverted where asked, and is not taken for a touch;
//   - an unresolved mapping is skipped, kept, flagged and counted;
//   - learn makes the next touched parameter a mapping, its range from the
//     value before the touch to the last one; a click maps the whole range;
//   - a corrupt file is reported, held, and never written over;
//   - opening another session resets what is per session;
//   - the panel loads from the application's resources and drives the model.
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "insert_controller.h"
#include "register_types.h"
#include "smart_controls_model.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;
namespace sc = sonora::smart_controls;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            return 1;                                                                  \
        }                                                                              \
    } while (0)

namespace {

api::PluginParam param(uint32_t index, const char* key, const char* name, float lo, float hi, float def) {
    api::PluginParam p;
    p.index = index;
    p.key = key;
    p.name = name;
    p.min = lo;
    p.max = hi;
    p.default_value = def;
    p.flags = api::kParamAutomatable;
    return p;
}

api::PluginInfo plugin(const char* id, const char* name, std::vector<api::PluginParam> params) {
    api::PluginInfo p;
    p.id = id;
    p.name = name;
    p.vendor = "Sonora";
    p.version = "1";
    p.inputs = 1;
    p.outputs = 1;
    p.params = std::move(params);
    return p;
}

struct Sent {
    model::Id insert;
    std::string key;
    float value;
};

struct Rig {
    ModelDocument doc{make_empty_session(2, 48000)};
    InsertController ctl{doc};
    SmartControlsModel smart{ctl};
    std::vector<Sent> sent;
    model::Id drive = 0, comp = 0;

    Rig() {
        api::PluginCatalog c;
        c.plugins.push_back(plugin("urn:test:drive", "Test Drive",
                                   {param(0, "drive", "Drive", 0, 10, 0), param(1, "tone", "Tone", 0, 1, 0.5f)}));
        c.plugins.push_back(plugin("urn:test:comp", "Test Comp",
                                   {param(0, "threshold", "Threshold", -60, 0, 0), param(1, "ratio", "Ratio", 1, 20, 2)}));
        ctl.set_catalog(c);
        std::string why;
        if (!ctl.add(0, 0, 0, why) || !ctl.add(1, 0, 1, why)) std::fprintf(stderr, "add: %s\n", why.c_str());
        drive = ctl.slot(0, 0).insert;
        comp = ctl.slot(1, 0).insert;
        ctl.set_param_sender(smart.learnTap([this](model::Id i, const std::string& k, float v) {
            sent.push_back({i, k, v});
        }));
    }
    const Sent* find(model::Id insert, const std::string& key) const {
        const Sent* last = nullptr;
        for (const Sent& s : sent)
            if (s.insert == insert && s.key == key) last = &s;
        return last;
    }
    float stored(model::Id insert, const std::string& key) const {
        if (const model::InsertMix* m = doc.snapshot().mix.find_insert(insert))
            for (const auto& v : m->params)
                if (v.key == key) return v.value;
        return NAN;
    }
};

sc::Mapping mapping(model::Id insert, const char* key, float lo, float hi, bool invert = false) {
    sc::Mapping m;
    m.insert = insert;
    m.key = key;
    m.lo = lo;
    m.hi = hi;
    m.invert = invert;
    return m;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int macro_reaches_every_mapped_parameter() {
    Rig r;
    CHECK(r.drive != 0 && r.comp != 0);
    std::string why;
    CHECK(r.smart.addMapping(0, mapping(r.drive, "drive", 2, 8), why));
    CHECK(r.smart.addMapping(0, mapping(r.drive, "tone", 0, 1, true), why));
    CHECK(r.smart.addMapping(0, mapping(r.comp, "threshold", -10, -40), why));
    r.smart.learn(1);   // armed on another macro: a macro move is not a touch
    r.smart.setMacroValue(0, 0.25);
    CHECK(r.sent.size() == 3);
    CHECK(r.find(r.drive, "drive") && near(r.find(r.drive, "drive")->value, 3.5f));
    CHECK(r.find(r.drive, "tone") && near(r.find(r.drive, "tone")->value, 0.75f));
    CHECK(r.find(r.comp, "threshold") && near(r.find(r.comp, "threshold")->value, -17.5f));
    // Each insert's gesture ended: the document holds the values.
    CHECK(near(r.stored(r.drive, "drive"), 3.5f));
    CHECK(near(r.stored(r.comp, "threshold"), -17.5f));
    CHECK(r.smart.set().macros[1].mappings.empty());
    CHECK(r.smart.notice().isEmpty());
    // The knob reads back from its parameter.
    CHECK(near(r.smart.macros()[0].toMap()[QStringLiteral("value")].toFloat(), 0.25f));
    std::printf("  macro reaches every mapped parameter: ok\n");
    return 0;
}

int unresolved_skipped_and_shown() {
    Rig r;
    std::string why;
    CHECK(r.smart.addMapping(2, mapping(r.drive, "drive", 0, 10), why));
    CHECK(r.smart.addMapping(2, mapping(987654, "gain", 0, 1), why));    // no such insert
    CHECK(r.smart.addMapping(2, mapping(r.comp, "knee", 0, 1), why));    // no such parameter
    r.smart.setMacroValue(2, 0.5);
    CHECK(r.sent.size() == 1 && r.sent[0].key == "drive" && near(r.sent[0].value, 5.0f));
    CHECK(r.smart.set().macros[2].mappings.size() == 3);   // kept, never dropped
    const QVariantMap knob = r.smart.macros()[2].toMap();
    CHECK(knob[QStringLiteral("mappings")].toInt() == 3);
    CHECK(knob[QStringLiteral("unresolved")].toInt() == 2);
    r.smart.select(2);
    const QVariantList rows = r.smart.mappings();
    CHECK(rows.size() == 3);
    CHECK(!rows[0].toMap()[QStringLiteral("unresolved")].toBool());
    CHECK(rows[0].toMap()[QStringLiteral("plugin")].toString() == QStringLiteral("Test Drive"));
    CHECK(rows[1].toMap()[QStringLiteral("unresolved")].toBool());
    CHECK(rows[2].toMap()[QStringLiteral("unresolved")].toBool());
    std::printf("  unresolved skipped and shown: ok\n");
    return 0;
}

int learn_adds_a_mapping() {
    Rig r;
    std::string why;
    r.smart.learn(3);
    CHECK(r.smart.learning() == 3 && r.smart.selected() == 3);
    // A drag in the plug-in panel: the gesture the panel makes.
    CHECK(r.ctl.param_begin(r.comp, why));
    CHECK(r.ctl.param_set(r.comp, "threshold", -5, why));
    CHECK(r.ctl.param_set(r.comp, "threshold", -24, why));
    CHECK(r.smart.mappings().size() == 1);
    CHECK(r.smart.mappings()[0].toMap()[QStringLiteral("capturing")].toBool());
    CHECK(r.ctl.param_end(why));
    r.smart.learn(3);   // Done
    CHECK(r.smart.learning() == -1);
    CHECK(r.sent.size() == 2);   // the engine still got the drag
    const auto& m = r.smart.set().macros[3].mappings;
    CHECK(m.size() == 1 && m[0].insert == r.comp && m[0].key == "threshold");
    CHECK(near(m[0].lo, 0.0f) && near(m[0].hi, -24.0f));   // from before the touch to its end
    // The learned mapping drives its parameter.
    r.smart.setMacroValue(3, 0.5);
    CHECK(near(r.sent.back().value, -12.0f));

    // A click without travel maps the whole range; another parameter
    // touched after the first one ends learn and maps nothing more.
    r.smart.learn(4);
    CHECK(r.ctl.param_begin(r.drive, why));
    CHECK(r.ctl.param_set(r.drive, "drive", 0, why));
    CHECK(r.ctl.param_set(r.drive, "tone", 0.9f, why));
    CHECK(r.ctl.param_end(why));
    CHECK(r.smart.learning() == -1);
    const auto& m4 = r.smart.set().macros[4].mappings;
    CHECK(m4.size() == 1 && m4[0].key == "drive" && near(m4[0].lo, 0) && near(m4[0].hi, 10));

    // Learning a parameter the macro already drives is refused, held.
    r.smart.learn(4);
    CHECK(r.ctl.param_set(r.drive, "drive", 4, why));
    CHECK(r.ctl.param_end(why));
    CHECK(r.smart.set().macros[4].mappings.size() == 1);
    CHECK(r.smart.notice().contains(QStringLiteral("duplicate")));
    CHECK(r.smart.learning() == -1);
    std::printf("  learn adds a mapping: ok\n");
    return 0;
}

int store_round_trip_and_corrupt() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QString::fromLatin1(SmartControlsModel::kFileName));
    {
        Rig r;
        r.smart.setSessionFolder(dir.path());   // no file yet: eight defaults, no notice
        CHECK(r.smart.notice().isEmpty() && r.smart.set().macros.size() == 8);
        std::string why;
        CHECK(r.smart.addMapping(0, mapping(r.drive, "drive", 1, 9), why));
        r.smart.renameMacro(0, QStringLiteral("Grit"));
        r.smart.select(0);
        r.smart.setInvert(0, true);
        CHECK(QFile::exists(path));
    }
    {
        Rig r;
        r.smart.setSessionFolder(dir.path());
        CHECK(r.smart.notice().isEmpty());
        const auto& m = r.smart.set().macros[0];
        CHECK(m.name == "Grit" && m.mappings.size() == 1 && m.mappings[0].invert);
        CHECK(r.smart.set().macros.size() == 8);
    }
    {
        QFile f(path);
        CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("{ not json");
    }
    Rig r;
    r.smart.setSessionFolder(dir.path());
    CHECK(!r.smart.notice().isEmpty());
    CHECK(r.smart.notice().contains(QStringLiteral("could not be read")));
    CHECK(r.smart.set().macros.size() == 8 && r.smart.set().macros[0].mappings.empty());
    std::string why;
    CHECK(r.smart.addMapping(0, mapping(r.drive, "drive", 0, 1), why));   // in memory only
    QFile f(path);
    CHECK(f.open(QIODevice::ReadOnly) && f.readAll() == "{ not json");
    // The same through the injected store.
    auto mem = std::make_unique<sc::MemoryStore>();
    mem->text = "[1,2";
    r.smart.open(std::move(mem));
    CHECK(!r.smart.notice().isEmpty());
    r.smart.dismissNotice();
    CHECK(r.smart.notice().isEmpty());
    std::printf("  store round trip and corrupt file: ok\n");
    return 0;
}

int session_open_resets() {
    Rig r;
    std::string why;
    CHECK(r.smart.addMapping(5, mapping(r.drive, "drive", 0, 10), why));
    r.smart.setMacroValue(1, 0.7);
    r.smart.learn(6);
    r.smart.open(std::make_unique<sc::MemoryStore>(), false);
    CHECK(r.smart.learning() == -1 && r.smart.selected() == 0);
    CHECK(r.smart.set().macros[5].mappings.empty());
    CHECK(near(r.smart.macros()[1].toMap()[QStringLiteral("value")].toFloat(), 0.0f));
    std::printf("  session open resets: ok\n");
    return 0;
}

int panel_loads_and_drives() {
    Rig r;
    std::string why;
    CHECK(r.smart.addMapping(0, mapping(r.drive, "drive", 0, 10), why));
    QQmlEngine engine;
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/sonora/qml/SmartControlsPanel.qml")));
    std::unique_ptr<QObject> panel(component.createWithInitialProperties(
        {{QStringLiteral("smart"), QVariant::fromValue(static_cast<QObject*>(&r.smart))}}));
    if (!panel) std::fprintf(stderr, "%s\n", qPrintable(component.errorString()));
    CHECK(panel != nullptr);
    CHECK(panel->findChild<QObject*>(QStringLiteral("macro7")) != nullptr);
    QObject* learn = panel->findChild<QObject*>(QStringLiteral("learnButton"));
    CHECK(learn != nullptr);
    CHECK(learn->property("text").toString() == QStringLiteral("Learn"));
    QMetaObject::invokeMethod(learn, "clicked", Q_ARG(QVariant, QVariant()));
    CHECK(r.smart.learning() == 0);
    CHECK(learn->property("text").toString() == QStringLiteral("Done"));
    QObject* notice = panel->findChild<QObject*>(QStringLiteral("smartNotice"));
    CHECK(notice != nullptr && !notice->property("visible").toBool());
    r.smart.renameMacro(1, QString());   // refused: a macro needs a name
    CHECK(notice->property("visible").toBool());
    std::printf("  panel loads and drives the model: ok\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    std::printf("test_ui_smart_controls\n");
    int failed = 0;
    failed += macro_reaches_every_mapped_parameter();
    failed += unresolved_skipped_and_shown();
    failed += learn_adds_a_mapping();
    failed += store_round_trip_and_corrupt();
    failed += session_open_resets();
    failed += panel_loads_and_drives();
    if (failed != 0) {
        std::printf("%d failed\n", failed);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
