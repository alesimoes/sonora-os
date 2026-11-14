// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sections in Memory Locations (ADR-0116): arrangement_apply submits the
// planner's Compound as ONE edit (one undo step), a refusal changes nothing
// and says so, and the window's Copy to / Move to act on the destination
// field's point and hold the message.
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <cstdio>
#include <string>

#include "edit_arrangement.h"
#include "edit_controller.h"
#include "key_router.h"
#include "model_access.h"
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

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            return 1;                                                                    \
        }                                                                                \
    } while (0)

namespace {

constexpr uint32_t kRate = 48000;

std::size_t region_count(const TimelineDocument& d) {
    std::size_t n = 0;
    for (const auto& t : d.snapshot().edit->tracks) {
        for (const auto& p : t->playlists) n += p->regions.size();
    }
    return n;
}

// A section over the first region of the first track; returns its id.
Id add_section(EditController& c, Frames* from, Frames* to) {
    const auto& track = *c.snapshot().edit->tracks.front();
    const Playlist* p = active_playlist(&track);
    *from = p->regions.front().position;
    *to = p->regions.front().end();
    std::string why;
    if (!c.add_marker("Verse", EditController::MarkerPlace{*from, *to - *from}, why)) return kNoId;
    return c.arrangement_sections().front().id;
}

int test_controller() {
    ModelDocument doc(make_demo_session(2, kRate));
    EditController c(doc);
    Frames from = 0, to = 0;
    const Id section = add_section(c, &from, &to);
    CHECK(section != kNoId);
    std::string message;

    // A destination inside the section is refused: nothing changes, and the
    // message says what did not happen.
    const std::size_t before = region_count(doc);
    const std::size_t steps = doc.history().step_count();
    CHECK(!c.arrangement_apply(section, from + 1, SectionMode::Copy, message));
    CHECK(message.find("Nothing was copied") != std::string::npos);
    CHECK(region_count(doc) == before && doc.history().step_count() == steps);

    // A section that is gone.
    CHECK(!c.arrangement_apply(section + 1000, to, SectionMode::Move, message));
    CHECK(message.find("no longer in the session") != std::string::npos);

    // Copy after itself: one undo step, the message names what stays.
    CHECK(c.arrangement_apply(section, to, SectionMode::Copy, message));
    CHECK(region_count(doc) > before);
    CHECK(doc.history().step_count() == steps + 1);
    CHECK(message.find("Copied") == 0 && message.find(model::kSectionEditLimits) != std::string::npos);
    std::string why;
    CHECK(doc.undo(why));
    CHECK(region_count(doc) == before);
    return 0;
}

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* ch : item->childItems()) {
        if (QQuickItem* f = named(ch, name)) return f;
    }
    return nullptr;
}

int test_window() {
    KeyMap map;
    std::string err;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));
    test::RecordingAdapter engine(2, api::testing::FakeEngine::engine_commands());
    ModelDocument doc(make_demo_session(2, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    KeyRouter router(map, engine.info().implemented_commands,
                     [&engine](const std::string& cmd, const CommandContext& ctx) {
                         return engine.send(cmd, ctx);
                     });
    session.installCommands(router.local());
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);

    Frames from = 0, to = 0;
    const Id section = add_section(session.controller(), &from, &to);
    CHECK(section != kNoId);

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)},
                               {QStringLiteral("session"), QVariant::fromValue(&session)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    CHECK(view.status() == QQuickView::Ready);
    view.show();
    CHECK(QTest::qWaitForWindowExposed(&view));
    session.setMemoryLocationsOpen(true);
    QQuickItem* win = named(view.rootObject(), QStringLiteral("memoryLocationsWindow"));
    CHECK(win != nullptr);
    QQuickItem* copy = nullptr;
    CHECK(QTest::qWaitFor([&]() {
        copy = named(win, QStringLiteral("arrangementCopy"));
        return copy != nullptr;
    }));

    // Destination inside the section: refused, held in the window.
    session.controller().set_insertion(from + 1);
    session.arrangementApply(0, false);
    QQuickItem* notice = named(win, QStringLiteral("arrangementNotice"));
    CHECK(notice != nullptr && notice->isVisible());
    CHECK(notice->property("text").toString().contains(QStringLiteral("Nothing was copied")));

    // At the section's end: copied, and the held text now says what stays.
    const std::size_t before = region_count(doc);
    session.controller().set_insertion(to);
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                      copy->mapToScene(QPointF(copy->width() / 2, copy->height() / 2)).toPoint());
    CHECK(region_count(doc) > before);
    CHECK(notice->property("text").toString().startsWith(QStringLiteral("Copied")));
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (test_controller() || test_window()) return 1;
    std::printf("ui arrangement: ok\n");
    return 0;
}
