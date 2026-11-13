// SPDX-License-Identifier: GPL-3.0-or-later
//
// LoopBrowserModel on the contract's fakes: filters, tempo match, audition
// toggle and refusal, insert and its refusals held as a notice, and the QML
// panel loading against the model.
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QVariantMap>
#include <cstdio>

#include "loop_browser_model.h"
#include "register_types.h"
#include "sonora/loops/fake_loops.h"

using namespace sonora;
using namespace sonora::ui;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace {

loops::LoopEntry loop(const char* id, const char* cat, double bpm, const char* key,
                      const char* path = "x.wav") {
    loops::LoopEntry e;
    e.id = id;
    e.name = std::string("Loop ") + id;
    e.path = path;
    e.category = cat;
    e.bpm = bpm;
    e.key = key;
    return e;
}

struct Rig {
    loops::FakeLoopSource source{{loop("a", "Drums", 120, ""), loop("b", "Bass", 121, "A min"),
                                  loop("c", "Drums", 90, "", ""), loop("d", "Bass", 0, "E min")}};
    loops::FakeLoopAudition audition;
    loops::FakeLoopInserter inserter;
    int track = 1;
    LoopBrowserModel model{source, audition, inserter,
                           {[] { return 120.0; }, [this] { return track; }, [] { return 480; }}};
};

int filters() {
    Rig r;
    CHECK(r.model.filtered().size() == 4);
    r.model.setCategory("Drums");
    CHECK(r.model.filtered().size() == 2);
    r.model.setCategory("");
    r.model.setKey("A min");
    CHECK(r.model.filtered().size() == 1 && r.model.filtered()[0].id == "b");
    r.model.setKey("");
    r.model.setMatchTempo(true);  // 120 +-5 %: a and b; c (90) and d (unknown) drop
    CHECK(r.model.filtered().size() == 2);
    r.model.setTolerance(0);
    CHECK(r.model.filtered().size() == 1 && r.model.filtered()[0].id == "a");
    CHECK(r.model.summary() == "1 of 4 loops");
    return 0;
}

int audition_and_refusal() {
    Rig r;
    CHECK(r.model.audition("a"));
    CHECK(r.model.auditioningId() == "a" && r.audition.session_bpm_ == 120.0);
    CHECK(r.model.audition("a"));  // second press stops
    CHECK(r.model.auditioningId().isEmpty());
    // c has no file: refused, held, never shown as playing.
    CHECK(!r.model.audition("c"));
    CHECK(r.model.auditioningId().isEmpty());
    CHECK(!r.model.notice().isEmpty());
    r.model.dismissNotice();
    CHECK(r.model.notice().isEmpty());
    return 0;
}

int insert() {
    Rig r;
    CHECK(!r.model.insertSelected());  // nothing selected
    CHECK(!r.model.notice().isEmpty());
    r.model.select("b");
    CHECK(r.model.insertSelected());
    CHECK(r.inserter.calls_.size() == 1 && r.inserter.calls_[0].id == "b" &&
          r.inserter.calls_[0].track == 1 && r.inserter.calls_[0].position == 480);
    r.track = -1;
    CHECK(!r.model.insertEntry("b") && r.inserter.calls_.size() == 1);
    r.track = 9;  // the inserter refuses an unknown track
    r.model.dismissNotice();
    CHECK(!r.model.insertEntry("b") && !r.model.notice().isEmpty());
    // A selection the filter dropped is no selection.
    r.track = 1;
    r.model.setCategory("Drums");
    CHECK(r.model.selectedId().isEmpty());
    return 0;
}

int qml() {
    Rig r;
    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/"));
    QQmlComponent comp(&engine, QUrl(QStringLiteral("qrc:/sonora/qml/LoopBrowser.qml")));
    QVariantMap props;
    props.insert("browser", QVariant::fromValue(static_cast<QObject*>(&r.model)));
    QObject* o = comp.createWithInitialProperties(props);
    if (o == nullptr) std::fprintf(stderr, "%s\n", qPrintable(comp.errorString()));
    CHECK(o != nullptr);
    CHECK(o->findChild<QObject*>("loopList") != nullptr);
    CHECK(o->findChild<QObject*>("loopInsert") != nullptr);
    delete o;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    int failed = 0;
    failed += filters();
    failed += audition_and_refusal();
    failed += insert();
    failed += qml();
    if (failed == 0) std::printf("test_loop_browser: all passed\n");
    return failed == 0 ? 0 : 1;
}
