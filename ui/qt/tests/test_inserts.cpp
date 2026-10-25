// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Mix window's insert slots (ADR-0012 checklist 6) on FakeEngine, wired
// as main() wires them: ControlAdapter, ReplicaDocument, SessionModel,
// ShellModel's tick and its plugin sinks, InsertModel. Every check goes
// through the engine's answer, never a stand-in that agrees with the UI:
//
//   - the catalog arrives, and the chooser groups, searches and refuses;
//   - add, move, copy (state flushed first), bypass, active, remove;
//   - undo and redo follow owner decision O1's default: structure is an
//     undo step, bypass and values are not;
//   - a fault shows on its slot with the engine's words, and clears;
//   - a parameter drag sends PluginParameterChange, its end one
//     SetInsertParams; value text and positions from the catalog;
//   - the chain's latency and the session's compensation from the block;
//   - File > Save sends FlushPluginState and writes only after its reply;
//   - the QML: slots on the strips, the chooser with typing that does not
//     reach the key map, Escape, a drag between strips, the panel.
#include <QFile>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "control_adapter.h"
#include "insert_controller.h"
#include "insert_model.h"
#include "key_router.h"
#include "peak_source.h"
#include "register_types.h"
#include "replica_document.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/model/session_file.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

const std::string kSha(64, 'a');

// Catalog indices in fake_catalog().
constexpr int kGain = 0, kLimiter = 1, kComp = 2, kRefused = 3;

struct Rig {
    api::testing::FakeEngine fake{config()};
    ControlAdapter engine{fake};
    ReplicaDocument doc{[this](const api::ModelRequest& r) { return engine.submitModel(r); },
                        make_empty_session(4, 48000)};
    SyntheticPeakSource peaks{48000, 120.0};
    SessionModel session{doc, peaks, true};
    KeyRouter router{KeyMap::built_in_default(), api::testing::FakeEngine::engine_commands(),
                     [this](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     }};
    ShellModel shell{engine, router};
    InsertModel inserts{doc, session};
    std::vector<std::string> refused;
    std::vector<std::string> notices;
    QTemporaryDir dir;

    static api::testing::FakeEngine::Config config() {
        api::testing::FakeEngine::Config c;
        c.tracks = 4;
        return c;
    }

    Rig() {
        session.setSessionsDir(dir.path());
        engine.setModelSink([this](const std::vector<api::ModelUpdate>& u,
                                   const std::vector<api::CommandReply>& r) {
            if (doc.receive(u, r)) session.documentUpdated();
        });
        doc.on_refused = [this](const std::string& why) { refused.push_back(why); };
        QObject::connect(&session, &SessionModel::refused,
                         [this](const QString& why) { refused.push_back(why.toStdString()); });
        QObject::connect(&session, &SessionModel::notice,
                         [this](const QString& n) { notices.push_back(n.toStdString()); });
        inserts.controller().set_param_sender(
            [this](model::Id insert, const std::string& key, float value) {
                engine.send_plugin_parameter(insert, key, value);
            });
        ShellModel::PluginSinks sinks;
        sinks.catalog = [this](const api::PluginCatalog& c) { inserts.onCatalog(c); };
        sinks.fault = [this](uint64_t insert, const std::string& text) { inserts.onFault(insert, text); };
        sinks.frame = [this](const EngineFrame& f, bool alive) { inserts.onFrame(f, alive); };
        shell.setPluginSinks(std::move(sinks));
        session.installCommands(router.local());
        router.prepare({});
    }

    bool connect() {
        std::string error;
        if (!engine.connect(error)) {
            std::fprintf(stderr, "connect: %s\n", error.c_str());
            return false;
        }
        settle();
        return doc.synced();
    }
    // Enough ticks for a request, its reply and its update, and one state
    // frame (a quarter of the ticks) after them.
    void settle() {
        for (int i = 0; i < 8; ++i) shell.tick();
    }
    InsertController& ctl() { return inserts.controller(); }
    SlotView slot(int t, int s) { return ctl().slot(t, s); }
    const model::InsertMix* values(model::Id id) { return doc.snapshot().mix.find_insert(id); }
};

int catalog_and_chooser() {
    Rig r;
    CHECK(r.connect());
    CHECK(r.ctl().has_catalog());
    CHECK(r.ctl().plugins().size() == 4);
    const auto cats = r.ctl().categories();
    CHECK(std::find(cats.begin(), cats.end(), "Dynamics") != cats.end());
    CHECK(std::find(cats.begin(), cats.end(), "Utility") != cats.end());
    CHECK(std::find(cats.begin(), cats.end(), "Fx") == cats.end());
    // Search over name and vendor, any case; a category narrows it.
    auto found = r.ctl().browse("", "COMP");
    CHECK(found.size() == 1 && found[0] == kComp);
    found = r.ctl().browse("Dynamics", "");
    CHECK(found.size() == 2);
    CHECK(r.ctl().browse("", "sonora").size() == 4);
    // Shown with the reason, never hidden.
    CHECK(r.ctl().refusal(kLimiter, 0).find("stereo") != std::string::npos);
    CHECK(r.ctl().refusal(kRefused, 0).find("instance-access") != std::string::npos);
    CHECK(r.ctl().refusal(kGain, 0).empty());
    const QVariantList rows = r.inserts.browse(QString(), QString());
    CHECK(rows.size() == 4);
    std::printf("  catalog and chooser: ok\n");
    return 0;
}

int add_bypass_undo() {
    Rig r;
    CHECK(r.connect());
    // Click on an empty slot: the chooser, on that slot.
    r.inserts.slotClicked(0, 0, 0);
    CHECK(r.inserts.browserOpen() && r.inserts.browserTrack() == 0);
    // A plugin that does not fit is refused with its reason; nothing changes.
    CHECK(!r.inserts.choose(kLimiter));
    CHECK(!r.refused.empty() && r.refused.back().find("stereo") != std::string::npos);
    CHECK(!r.slot(0, 0).present && r.inserts.browserOpen());
    CHECK(r.inserts.choose(kGain));
    CHECK(!r.inserts.browserOpen());
    CHECK(r.slot(0, 0).present && r.slot(0, 0).name == "Fake Gain");   // shown at once
    r.settle();
    CHECK(r.doc.in_flight() == 0);
    const SlotView added = r.slot(0, 0);
    CHECK(added.present && added.running && !added.faulted && !added.bypass);
    CHECK(r.doc.can_undo());

    // Ctrl-click bypasses: mix state, the engine's block says so, no undo step.
    r.inserts.slotClicked(0, 0, Qt::ControlModifier);
    CHECK(r.slot(0, 0).bypass);
    r.settle();
    CHECK(r.slot(0, 0).bypass);
    // Ctrl+Start-click takes it out of the chain, an undo step.
    r.inserts.slotClicked(0, 0, Qt::ControlModifier | Qt::MetaModifier);
    r.settle();
    CHECK(!r.slot(0, 0).active && !r.slot(0, 0).running);
    std::string why;
    CHECK(r.session.controller().undo(why));   // edit.undo: back in the chain
    r.settle();
    CHECK(r.slot(0, 0).present && r.slot(0, 0).active && r.slot(0, 0).bypass);
    CHECK(r.session.controller().undo(why));   // the add: gone
    r.settle();
    CHECK(!r.slot(0, 0).present);
    CHECK(r.session.controller().redo(why));   // back, still bypassed: values outlive it
    r.settle();
    CHECK(r.slot(0, 0).present && r.slot(0, 0).bypass && r.slot(0, 0).insert == added.insert);
    std::printf("  add, bypass, active, undo: ok\n");
    return 0;
}

int move_copy_remove() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.ctl().add(0, 1, kComp, why));
    CHECK(r.ctl().add(1, 3, kGain, why));
    r.settle();
    const model::Id comp = r.slot(0, 1).insert;

    // Onto a full slot: refused with what holds it.
    r.inserts.dropInsert(0, 1, 1, 3, false);
    CHECK(!r.refused.empty() && r.refused.back().find("holds Fake Gain") != std::string::npos);
    CHECK(r.slot(0, 1).insert == comp);
    // Within a strip, then across strips; the id stays.
    r.inserts.dropInsert(0, 1, 0, 4, false);
    r.settle();
    CHECK(!r.slot(0, 1).present && r.slot(0, 4).insert == comp);
    r.inserts.dropInsert(0, 4, 2, 0, false);
    r.settle();
    CHECK(!r.slot(0, 4).present && r.slot(2, 0).insert == comp);
    CHECK(r.session.controller().undo(why));
    r.settle();
    CHECK(r.slot(0, 4).insert == comp && !r.slot(2, 0).present);

    // Alt-drag copies with its values, and its state flushed from the
    // running plugin first: nothing is added until the engine answers.
    CHECK(r.ctl().param_set(comp, "0", 0.8f, why) && r.ctl().param_end(why));
    r.inserts.toggleBypass(0, 4);
    r.settle();
    r.fake.script_plugin_state(comp, kSha, 4096);
    r.inserts.dropInsert(0, 4, 3, 2, true);
    CHECK(!r.slot(3, 2).present);
    r.settle();
    const SlotView copy = r.slot(3, 2);
    CHECK(copy.present && copy.insert != comp && copy.name == "Fake Compressor" && copy.bypass);
    const model::InsertMix* v = r.values(copy.insert);
    CHECK(v != nullptr && v->state.has_value() && v->state->sha256 == kSha);
    CHECK(v != nullptr && v->params.size() == 1 && v->params[0].key == "0" &&
          v->params[0].value == 0.8f);
    // The source holds the flushed state too.
    CHECK(r.values(comp) != nullptr && r.values(comp)->state.has_value());

    // Remove, and undo brings it back with its values.
    r.inserts.remove(3, 2);
    r.settle();
    CHECK(!r.slot(3, 2).present);
    CHECK(r.session.controller().undo(why));
    r.settle();
    CHECK(r.slot(3, 2).insert == copy.insert);
    CHECK(r.values(copy.insert)->params[0].value == 0.8f);
    std::printf("  move, copy, remove: ok\n");
    return 0;
}

// The current behaviour, pinned until AddInsert carries `active` (BACKLOG,
// review of PR #50): an inactive insert's copy is two undo steps, so one
// Undo leaves the copy in place and active.
int copy_of_an_inactive_insert() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.ctl().add(0, 0, kComp, why));
    r.settle();
    r.inserts.setActive(0, 0, false);
    r.settle();
    r.inserts.dropInsert(0, 0, 1, 0, true);
    r.settle();
    const SlotView copy = r.slot(1, 0);
    CHECK(copy.present && !copy.active);
    CHECK(r.session.controller().undo(why));
    r.settle();
    CHECK(r.slot(1, 0).present && r.slot(1, 0).active);   // the defect, as it stands
    CHECK(r.session.controller().undo(why));
    r.settle();
    CHECK(!r.slot(1, 0).present);
    CHECK(!r.slot(0, 0).active);   // the original is untouched throughout
    std::printf("  copy of an inactive insert: ok\n");
    return 0;
}

// A copy that waited for the plugin's state across another session's
// arrival copies nothing: the ids it held name other things now.
int copy_across_an_open() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.ctl().add(0, 0, kComp, why));
    r.settle();
    r.inserts.dropInsert(0, 0, 1, 0, true);   // the flush is on its way
    model::Snapshot other = make_empty_session(4, 48000);
    CHECK(r.doc.reset(other, why));
    r.settle();
    for (int t = 0; t < 4; ++t) CHECK(!r.slot(t, 0).present);
    CHECK(!r.refused.empty() && r.refused.back().find("replaced") != std::string::npos);
    std::printf("  copy across an open: ok\n");
    return 0;
}

int short_names() {
    CHECK(short_plugin_name("Fake Gain", 9) == "Fake Gain");
    CHECK(short_plugin_name("Fake Compressor", 8) == "FkCmprss");
    CHECK(short_plugin_name("Fake Compressor", 12) == "FakeComprssr");
    CHECK(short_plugin_name("Plate Reverb", 9) == "PlateRvrb");
    CHECK(short_plugin_name("Pro-Q 3", 6) == "ProQ3");
    CHECK(short_plugin_name("Échelle", 3).size() <= 3);
    std::printf("  short names: ok\n");
    return 0;
}

int faults() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.ctl().add(0, 0, kComp, why));
    r.settle();
    const model::Id id = r.slot(0, 0).insert;
    const int notices_before = static_cast<int>(r.shell.notices().size());

    r.fake.script_insert_fault(id, "its host crashed (SIGSEGV)");
    r.settle();
    SlotView s = r.slot(0, 0);
    CHECK(s.faulted && s.fault.find("crashed") != std::string::npos);
    // On the slot, not in the footer's notices.
    CHECK(static_cast<int>(r.shell.notices().size()) == notices_before);
    const QVariantMap m = r.inserts.strips()[0].toMap()[QStringLiteral("slots")].toList()[0].toMap();
    CHECK(m[QStringLiteral("faulted")].toBool());
    CHECK(m[QStringLiteral("fault")].toString().contains(QStringLiteral("SIGSEGV")));
    // The panel says it too.
    r.inserts.openPanel(0, 0);
    CHECK(r.inserts.panelSlotInfo()[QStringLiteral("faulted")].toBool());

    r.fake.script_insert_fault(id, "");
    r.settle();
    CHECK(!r.slot(0, 0).faulted && r.slot(0, 0).fault.empty());

    // A rescan without the plugin: missing, said on the slot.
    auto catalog = api::testing::fake_catalog();
    catalog.erase(catalog.begin() + kComp);
    r.fake.script_catalog(catalog);
    r.settle();
    s = r.slot(0, 0);
    CHECK(s.faulted && s.missing && s.fault.find("not installed") != std::string::npos);
    // Inactive: out of the chain, so not down.
    r.inserts.setActive(0, 0, false);
    r.settle();
    CHECK(!r.slot(0, 0).faulted && r.slot(0, 0).missing);
    std::printf("  faults: ok\n");
    return 0;
}

int parameters_and_delay() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.ctl().add(0, 0, kComp, why));
    CHECK(r.ctl().add(1, 0, kGain, why));
    r.settle();
    const model::Id comp = r.slot(0, 0).insert;
    const model::Id gain = r.slot(1, 0).insert;

    r.inserts.openPanel(0, 0);
    CHECK(r.inserts.panelOpen() && r.inserts.panelTitle() == QStringLiteral("Fake Compressor"));
    CHECK(r.inserts.panelParams().size() == 3);
    // A drag: changes go to the plugin while it runs, the document at the end.
    r.inserts.paramBegin();
    r.inserts.paramPosition(QStringLiteral("0"), 0.25);
    r.inserts.paramPosition(QStringLiteral("0"), 0.75);
    r.settle();
    CHECK(std::fabs(r.fake.plugin_value(comp, "0") - 0.75f) < 1e-6f);
    CHECK(r.values(comp) == nullptr || r.values(comp)->params.empty());
    CHECK(r.inserts.panelValues()[QStringLiteral("0")].toMap()[QStringLiteral("text")].toString() ==
          QStringLiteral("75 %"));
    r.inserts.paramEnd();
    r.settle();
    CHECK(r.values(comp) != nullptr && r.values(comp)->params.size() == 1 &&
          r.values(comp)->params[0].value == 0.75f);
    // A choice is a gesture of one, recorded at once; not an undo step.
    const bool could_undo = r.doc.can_undo();
    r.inserts.paramStep(QStringLiteral("7"), 1);
    r.settle();
    CHECK(r.inserts.panelValues()[QStringLiteral("7")].toMap()[QStringLiteral("text")].toString() ==
          QStringLiteral("RMS"));
    CHECK(std::fabs(r.fake.plugin_value(comp, "7") - 0.5f) < 1e-6f);
    CHECK(r.values(comp)->params.size() == 2);
    CHECK(r.doc.can_undo() == could_undo);
    // Alt-click: the default.
    r.inserts.paramReset(QStringLiteral("0"));
    r.settle();
    CHECK(r.inserts.panelValues()[QStringLiteral("0")].toMap()[QStringLiteral("text")].toString() ==
          QStringLiteral("50 %"));
    // Values the plugin's own editor moved arrive as the engine's
    // SetInsertParams and show in the panel.
    r.fake.script_editor_change(comp, "1", 0.9f);
    r.fake.script_editor_gesture_end(comp);
    r.settle();
    CHECK(r.inserts.panelValues()[QStringLiteral("1")].toMap()[QStringLiteral("text")].toString() ==
          QStringLiteral("90 %"));
    // A removed insert closes its panel.
    r.inserts.remove(0, 0);
    CHECK(!r.inserts.panelOpen());

    // Value text and positions from the catalog: units, steps, labels, log.
    const auto cat = api::testing::fake_catalog();
    const api::PluginParam& g = cat[kGain].params[0];
    const api::PluginParam& inv = cat[kGain].params[1];
    const api::PluginParam& rel = cat[kLimiter].params[1];
    CHECK(param_text(cat[kGain], g, -3.0f) == "-3.00 dB");
    CHECK(param_text(cat[kGain], g, 12.0f) == "12.0 dB");
    CHECK(param_text(cat[kGain], g, -0.0001f) == "0.000 dB");
    CHECK(param_text(cat[kGain], inv, 1.0f) == "On" && param_text(cat[kGain], inv, 0.0f) == "Off");
    CHECK(std::fabs(param_position(g, 0.0f) - 0.5f) < 1e-6f);
    CHECK(param_value_at(inv, 0.7f) == 1.0f);
    const float mid = param_value_at(rel, 0.5f);   // log: the geometric mean
    CHECK(std::fabs(mid - std::sqrt(0.001f * 1.0f)) < 1e-4f);
    CHECK(std::fabs(param_position(rel, mid) - 0.5f) < 1e-4f);
    CHECK(param_step(cat[kComp].params[2], 1.0f) == 2);
    CHECK(!r.ctl().param_set(gain, "nope", 1.0f, why) && why.find("no parameter") != std::string::npos);
    CHECK(r.ctl().param_set(gain, "gain", 99.0f, why));   // clamped to the range
    CHECK(r.ctl().param_end(why));
    r.settle();
    CHECK(r.values(gain)->params[0].value == 24.0f);

    // The chain's latency and the session's compensation, from the block.
    r.fake.script_plugin_latency(cat[kGain].id, 64);
    r.settle();
    CHECK(r.ctl().track_latency(1) == 64 + 128);   // its plugin and one IPC block
    CHECK(r.ctl().track_latency(0) == 0);
    CHECK(r.ctl().compensation() == 192);
    CHECK(r.inserts.compensationText().contains(QStringLiteral("192 samples")));
    CHECK(r.inserts.strips()[1].toMap()[QStringLiteral("latencyText")].toString() ==
          QStringLiteral("dly 192"));
    std::printf("  parameters and delay: ok\n");
    return 0;
}

int save_flushes_first() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    // No insert: nothing to wait for but the engine, which writes it.
    CHECK(r.session.saveSession(why));
    r.settle();
    CHECK(r.fake.saves() == 1 && !r.session.saveWaiting());
    const QString file = r.session.sessionFile();
    CHECK(!file.isEmpty());

    CHECK(r.ctl().add(0, 0, kComp, why));
    r.settle();
    const model::Id comp = r.slot(0, 0).insert;
    r.fake.script_plugin_state(comp, kSha, 1024);
    r.fake.script_editor_change(comp, "1", 0.4f);   // moved, gesture not ended
    CHECK(r.session.saveSession(why));
    // Waiting for the engine: nothing written yet, and a second save waits too.
    CHECK(r.fake.saves() == 1 && r.session.saveWaiting());
    CHECK(!r.session.saveSession(why) && why.find("waiting") != std::string::npos);
    CHECK(!r.session.openSessionFile(file, true, why));
    r.settle();
    CHECK(!r.session.saveWaiting());
    CHECK(r.fake.saves() == 2);
    // What was written is what the flush brought: the state file and the
    // editor's value.
    model::Loaded loaded;
    model::LoadError error;
    QFile in(file);
    CHECK(in.open(QIODevice::ReadOnly));
    CHECK(model::deserialize(in.readAll().toStdString(), loaded, error));
    const model::InsertMix* m = loaded.snapshot.mix.find_insert(comp);
    CHECK(m != nullptr && m->state.has_value() && m->state->sha256 == kSha);
    CHECK(m != nullptr && m->params.size() == 1 && m->params[0].key == "1");
    CHECK(!r.notices.empty() && r.notices.back().find("Saved") == 0);

    // A plugin whose state cannot be had: the save still happens, and says so.
    r.fake.script_insert_fault(comp, "its host is not answering");
    r.fake.script_plugin_state(comp, std::string(64, 'b'), 2048);
    CHECK(r.ctl().add(1, 0, kGain, why));   // something new to write
    r.settle();
    CHECK(r.session.saveSession(why));
    r.settle();
    CHECK(r.fake.saves() == 3);
    CHECK(r.notices.back().find("as last recorded") != std::string::npos);

    // A save the engine refuses is said, and the session stays modified.
    r.fake.script_save_failure("writing the session file failed: no space left on the device");
    CHECK(r.ctl().add(3, 0, kGain, why));
    r.settle();
    const std::size_t refusals = r.refused.size();
    CHECK(r.session.saveSession(why));
    r.settle();
    CHECK(!r.session.saveWaiting() && r.session.dirty());
    CHECK(r.refused.size() == refusals + 1 &&
          r.refused.back().find("no space left") != std::string::npos);
    r.fake.script_save_failure({});

    // An engine that never answers: the operator is told once the patience
    // is spent, and a second Save then asks for the file without the
    // plugins' state. A dead engine writes nothing, so that save waits in
    // turn, and once the patience is spent again a third Save asks again.
    r.session.setFlushPatienceMs(50);
    CHECK(r.ctl().add(2, 0, kGain, why));
    r.settle();
    r.fake.die();
    const std::size_t refused_before = r.refused.size();
    CHECK(r.session.saveSession(why) && r.session.saveWaiting());
    QTest::qWait(120);
    CHECK(r.refused.size() == refused_before + 1 &&
          r.refused.back().find("Save again") != std::string::npos);
    CHECK(r.session.saveSession(why) && r.session.saveWaiting());
    CHECK(!r.session.saveSession(why) && why.find("being written") != std::string::npos);
    QTest::qWait(120);
    CHECK(r.session.saveSession(why) && r.session.saveWaiting());
    CHECK(r.fake.saves() == 3);
    std::printf("  save flushes the plugins first: ok\n");
    return 0;
}

// --- the QML -----------------------------------------------------------------

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

QPoint centre(QQuickItem* item) {
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

int qml() {
    Rig r;
    CHECK(r.connect());
    std::string why;
    CHECK(r.ctl().add(0, 0, kComp, why));
    r.settle();

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&r.shell)},
                               {QStringLiteral("session"), QVariant::fromValue(&r.session)},
                               {QStringLiteral("inserts"), QVariant::fromValue(&r.inserts)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    r.shell.routeKeysFrom(&view);
    r.session.setWindow(QStringLiteral("mix"));
    view.show();
    view.requestActivate();
    CHECK(QTest::qWaitForWindowExposed(&view));
    CHECK(QTest::qWaitForWindowActive(&view));
    QQuickItem* root = view.rootObject();

    // Ten slots a strip; A on track 1 holds the compressor.
    for (int s = 0; s < 10; ++s) CHECK(named(root, QStringLiteral("insert_3_%1").arg(s)) != nullptr);
    QQuickItem* a = named(root, QStringLiteral("insert_0_0"));
    CHECK(a != nullptr && a->property("present").toBool());
    CHECK(named(root, QStringLiteral("compensation")) != nullptr);

    // A click on an empty slot opens the chooser, and its search field has
    // the keyboard: "s" types, it does not solo a track.
    QQuickItem* empty = named(root, QStringLiteral("insert_1_2"));
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(empty));
    CHECK(r.inserts.browserOpen());
    QQuickItem* browser = named(root, QStringLiteral("pluginBrowser"));
    CHECK(browser != nullptr && browser->isVisible());
    CHECK(r.shell.textEntry());
    const bool soloed = r.session.solos().value(0);
    for (const char c : std::string("gain")) QTest::keyClick(&view, c);
    QQuickItem* search = named(root, QStringLiteral("pluginSearch"));
    CHECK(search != nullptr && search->property("text").toString() == QStringLiteral("gain"));
    CHECK(r.session.solos().value(0) == soloed);
    CHECK(browser->property("entries").toList().size() == 1);
    // Return takes the first that fits.
    QTest::keyClick(&view, Qt::Key_Return);
    CHECK(!r.inserts.browserOpen() && !r.shell.textEntry());
    CHECK(r.slot(1, 2).present && r.slot(1, 2).name == "Fake Gain");
    // Escape closes without choosing, and gives the keys back.
    r.inserts.openBrowser(2, 0);
    QCoreApplication::processEvents();
    CHECK(r.shell.textEntry());
    QTest::keyClick(&view, Qt::Key_Escape);
    CHECK(!r.inserts.browserOpen() && !r.shell.textEntry());
    CHECK(!r.slot(2, 0).present);
    r.settle();

    // A drag from track 1's slot A onto track 4's slot C moves it.
    const model::Id comp = r.slot(0, 0).insert;
    const QPoint from = centre(named(root, QStringLiteral("insert_0_0")));
    const QPoint to = centre(named(root, QStringLiteral("insert_3_2")));
    QTest::mousePress(&view, Qt::LeftButton, {}, from);
    QTest::mouseMove(&view, from + QPoint(10, 3));
    QTest::mouseMove(&view, (from + to) / 2);
    QTest::mouseMove(&view, to);
    QTest::mouseRelease(&view, Qt::LeftButton, {}, to);
    CHECK(!r.slot(0, 0).present && r.slot(3, 2).insert == comp);
    CHECK(!r.inserts.browserOpen() && !r.inserts.panelOpen());   // a drag is not a click
    r.settle();

    // A plain click on it opens its parameters.
    QTest::mouseClick(&view, Qt::LeftButton, {}, centre(named(root, QStringLiteral("insert_3_2"))));
    CHECK(r.inserts.panelOpen() && r.inserts.panelTrack() == 3 && r.inserts.panelSlot() == 2);
    CHECK(named(root, QStringLiteral("param_7")) != nullptr);
    std::printf("  qml: ok\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (catalog_and_chooser() != 0) return 1;
    if (add_bypass_undo() != 0) return 1;
    if (move_copy_remove() != 0) return 1;
    if (copy_of_an_inactive_insert() != 0) return 1;
    if (copy_across_an_open() != 0) return 1;
    if (short_names() != 0) return 1;
    if (faults() != 0) return 1;
    if (parameters_and_delay() != 0) return 1;
    if (save_flushes_first() != 0) return 1;
    if (qml() != 0) return 1;
    std::printf("ui inserts: all checks passed\n");
    return 0;
}
