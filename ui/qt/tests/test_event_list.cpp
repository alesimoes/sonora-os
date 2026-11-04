// SPDX-License-Identifier: GPL-3.0-or-later
//
// The automation Event List and Snapshot: a lane's points listed in time
// order, edited by time and value, deleted, each one undo step; Snapshot
// writing the strip's value at the insertion point. Then the real QML: the
// Edit menu's entry, the window's rows and buttons, typing into a field.
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <algorithm>
#include <cstdio>
#include <string>

#include "edit_controller.h"
#include "key_router.h"
#include "peak_source.h"
#include "qt_keys.h"
#include "recording_adapter.h"
#include "register_types.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "timeline_document.h"

using namespace sonora;
using namespace sonora::ui;
using namespace sonora::ui::model;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

constexpr uint32_t kRate = 48000;

const AutomationLane* lane(const TimelineDocument& d, int track, AutomationParam p) {
    return d.snapshot().edit->tracks[static_cast<std::size_t>(track)]->lane(p);
}

bool seed(ModelDocument& doc, int track, AutomationParam p, std::vector<AutomationPoint> pts) {
    EditAutomation op;
    op.track = doc.snapshot().edit->tracks[static_cast<std::size_t>(track)]->id;
    op.param = p;
    op.from = 0;
    op.to = kMaxInteger;
    op.points = std::move(pts);
    std::string why;
    return doc.submit(op, why);
}

// --- the controller and the model -------------------------------------------

int test_event_list() {
    ModelDocument doc(make_empty_session(2, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    CommandRegistry local;
    session.installCommands(local);
    session.controller().select_track(0);

    // Nothing selected on a lane yet: an empty list, a title naming the lane.
    CHECK(session.eventList().isEmpty());
    CHECK(session.eventListTitle().endsWith(QStringLiteral("Volume")));

    CHECK(seed(doc, 0, AutomationParam::Volume, {{0, -40.0f}, {48000, -10.0f}, {96000, -10.0f}, {96000, -3.0f}}));
    session.controller().select_track(0);   // as a click would, to refresh the list
    QVariantList rows = session.eventList();
    CHECK(rows.size() == 4);
    CHECK(rows[0].toMap()[QStringLiteral("number")].toInt() == 1);
    CHECK(rows[1].toMap()[QStringLiteral("value")].toString() == QStringLiteral("-10.0 dB"));
    CHECK(!rows[2].toMap()[QStringLiteral("step")].toBool());
    CHECK(rows[3].toMap()[QStringLiteral("step")].toBool());   // the second point of a step

    // A value typed into a row: one undo step, the point where it was.
    const uint64_t r0 = doc.snapshot().revision;
    CHECK(session.eventEdit(1, QString(), QStringLiteral("-6.5")));
    CHECK(doc.snapshot().revision == r0 + 1);
    CHECK(lane(doc, 0, AutomationParam::Volume)->points[1] == (AutomationPoint{48000, -6.5f}));
    CHECK(session.canUndo());

    // A time typed in the main counter's format (samples, so the frame is plain).
    session.cycleMainCounter();
    session.cycleMainCounter();
    session.cycleMainCounter();
    CHECK(session.mainCounterFormat() == QStringLiteral("Samples"));
    CHECK(session.eventEdit(1, QStringLiteral("60000"), QString()));
    CHECK(lane(doc, 0, AutomationParam::Volume)->points[1].at == 60000);
    // A point stays between its neighbours: asked for 200000 it stops short
    // of the next one (96000), and the lane is still sorted.
    CHECK(session.eventEdit(1, QStringLiteral("200000"), QString()));
    CHECK(lane(doc, 0, AutomationParam::Volume)->points[1].at == 95999);

    // Typing back what is there is no edit.
    const uint64_t r1 = doc.snapshot().revision;
    CHECK(session.eventEdit(0, QStringLiteral("0"), QStringLiteral("-40")));
    CHECK(doc.snapshot().revision == r1);

    // Text that is not a time or a value is refused and the lane untouched.
    CHECK(!session.eventEdit(0, QStringLiteral("abc"), QString()));
    CHECK(!session.eventEdit(0, QString(), QStringLiteral("loud")));
    CHECK(!session.eventEdit(9, QString(), QStringLiteral("0")));
    CHECK(doc.snapshot().revision == r1);

    // Delete: one step; the other point of a step stays; undo brings it back.
    const std::size_t n = lane(doc, 0, AutomationParam::Volume)->points.size();
    CHECK(session.eventDelete(3));
    CHECK(lane(doc, 0, AutomationParam::Volume)->points.size() == n - 1);
    CHECK(!session.eventDelete(40));
    std::string why;
    CHECK(session.controller().undo(why));
    CHECK(lane(doc, 0, AutomationParam::Volume)->points.size() == n);

    // The lane picker: Pan has no points on this track.
    session.setEventLane(1);
    CHECK(session.eventLane() == 1 && session.eventList().isEmpty());
    CHECK(session.eventListTitle().endsWith(QStringLiteral("Pan")));
    session.setEventLane(7);   // out of range: unchanged
    CHECK(session.eventLane() == 1);
    session.setEventLane(0);

    // The window command toggles.
    CHECK(!session.eventListOpen());
    CHECK(local.dispatch("window.event_list", CommandContext{}) == DispatchResult::Ok);
    CHECK(session.eventListOpen());
    return 0;
}

int test_snapshot() {
    ModelDocument doc(make_empty_session(2, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    CommandRegistry local;
    session.installCommands(local);
    EditController& c = session.controller();
    std::string why;

    // No track selected: refused with a reason.
    c.clear_selection();
    CHECK(!c.snapshot_automation(why) && why.find("select a track") != std::string::npos);

    // The fader as it stands, at the insertion point, on a lane that did not exist.
    CHECK(c.set_gain(0, -7.0f, why));
    c.select_track(0);
    c.set_insertion(24000);
    const uint64_t r = doc.snapshot().revision;
    CHECK(local.dispatch("automation.snapshot", CommandContext{}) == DispatchResult::Ok);
    CHECK(doc.snapshot().revision == r + 1);
    const AutomationLane* l = lane(doc, 0, AutomationParam::Volume);
    CHECK(l != nullptr && l->points.size() == 1 && l->points[0] == (AutomationPoint{24000, -7.0f}));
    // Again at another frame, and over the same frame: replaced, not doubled.
    c.set_insertion(48000);
    CHECK(c.snapshot_automation(why));
    CHECK(c.snapshot_automation(why));
    CHECK(lane(doc, 0, AutomationParam::Volume)->points.size() == 2);
    // One undo step each.
    CHECK(c.undo(why));
    CHECK(c.undo(why) && c.undo(why));
    CHECK(lane(doc, 0, AutomationParam::Volume) == nullptr);

    // On Pan, over two selected tracks: one compound step.
    c.set_event_lane(1);
    c.select_range(0, 1000, 5000, 2);
    c.set_insertion(3000);
    const uint64_t r2 = doc.snapshot().revision;
    CHECK(c.snapshot_automation(why));
    CHECK(doc.snapshot().revision == r2 + 1);
    CHECK(lane(doc, 0, AutomationParam::Pan) != nullptr && lane(doc, 1, AutomationParam::Pan) != nullptr);
    return 0;
}

// --- the real QML -------------------------------------------------------------

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

QList<QQuickItem*> all_named(QQuickItem* item, const QString& name) {
    QList<QQuickItem*> out;
    if (item->objectName() == name) out.push_back(item);
    for (QQuickItem* c : item->childItems()) out += all_named(c, name);
    return out;
}

void click(QQuickView& view, QQuickItem* item) {
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                      item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
}

int test_window() {
    KeyMap map;
    std::string err;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/logic.json", map, err));
    test::RecordingAdapter engine(2, api::testing::FakeEngine::engine_commands());
    ModelDocument doc(make_empty_session(2, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    KeyRouter router(map, engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    session.installCommands(router.local());
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);
    CHECK(seed(doc, 0, AutomationParam::Volume, {{0, -20.0f}, {48000, -10.0f}, {96000, 0.0f}}));
    session.controller().select_track(0);

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)},
                               {QStringLiteral("session"), QVariant::fromValue(&session)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    model.routeKeysFrom(&view);
    view.show();
    CHECK(QTest::qWaitForWindowExposed(&view));
    QQuickItem* root = view.rootObject();

    // Edit > Automation Event List opens it.
    click(view, named(root, QStringLiteral("editMenuButton")));
    QQuickItem* entry = nullptr;
    CHECK(QTest::qWaitFor([&]() {
        entry = named(root, QStringLiteral("editEventList"));
        return entry != nullptr && entry->parentItem() != nullptr && entry->parentItem()->y() > 0;
    }));
    click(view, entry);
    QQuickItem* win = named(root, QStringLiteral("eventListWindow"));
    CHECK(win != nullptr && session.eventListOpen() && win->isVisible());
    QList<QQuickItem*> rows;
    CHECK(QTest::qWaitFor([&]() {
        rows = all_named(win, QStringLiteral("eventListRow"));
        return rows.size() == 3;
    }));

    // Typing a value into the second row's field sets it.
    std::sort(rows.begin(), rows.end(), [](QQuickItem* a, QQuickItem* b) { return a->y() < b->y(); });
    QQuickItem* value = named(rows[1], QStringLiteral("eventValue"));
    CHECK(value != nullptr);
    click(view, value);
    QQuickItem* input = named(value, QStringLiteral("eventValueInput"));
    CHECK(input != nullptr && QTest::qWaitFor([&]() { return input->hasActiveFocus(); }));
    input->setProperty("text", QStringLiteral("-4"));
    QTest::keyClick(&view, Qt::Key_Return);
    CHECK(QTest::qWaitFor([&]() {
        return lane(doc, 0, AutomationParam::Volume)->points[1].value == -4.0f;
    }));

    // The row's x deletes it; Snapshot adds one at the insertion point.
    click(view, named(rows[0], QStringLiteral("eventDelete")));
    CHECK(lane(doc, 0, AutomationParam::Volume)->points.size() == 2);
    session.controller().set_insertion(10000);
    click(view, named(win, QStringLiteral("eventListSnapshot")));
    CHECK(lane(doc, 0, AutomationParam::Volume)->points.size() == 3);

    // The lane picker lists Pan, which is empty.
    click(view, named(win, QStringLiteral("eventListLane1")));
    CHECK(session.eventLane() == 1);
    CHECK(QTest::qWaitFor([&]() { return all_named(win, QStringLiteral("eventListRow")).isEmpty(); }));

    // Logic's E key toggles the window.
    QTest::keyClick(&view, Qt::Key_E);
    CHECK(QTest::qWaitFor([&]() { return !session.eventListOpen(); }));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (test_event_list() || test_snapshot() || test_window()) return 1;
    std::printf("ui event list: ok\n");
    return 0;
}
