// SPDX-License-Identifier: GPL-3.0-or-later
//
// File > Bounce Mix (ui.mixer.bounce), in the real QML over FakeEngine's
// wire: the File menu opens the window through window.bounce; the choices
// reach the engine as mix.bounce's argument; progress shows while it runs;
// how it ended is a held notice in the footer, not the status line, until the
// window's Dismiss puts it away; a refusal is held in the window.
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <cstdio>
#include <string>

#include "control_adapter.h"
#include "key_router.h"
#include "peak_source.h"
#include "qt_keys.h"
#include "register_types.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/bounce.h"
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

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

void click(QQuickView& view, QQuickItem* item) {
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier,
                      item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
}

QString text_of(QQuickItem* root, const char* name) {
    QQuickItem* item = named(root, QString::fromLatin1(name));
    return item != nullptr ? item->property("text").toString() : QStringLiteral("<none>");
}

int bounce_window_over_the_fake_engine() {
    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 4;
    api::testing::FakeEngine fake(cfg);
    ControlAdapter engine(fake);
    std::string err;
    CHECK(engine.connect(err));

    KeyMap map;
    CHECK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));
    ModelDocument doc(make_empty_session(4, kRate));
    SyntheticPeakSource peaks(kRate, 120.0);
    SessionModel session(doc, peaks, true);
    std::string sent_argument;
    KeyRouter router(map, engine.info().implemented_commands,
                     [&](const std::string& c, const CommandContext& ctx) {
                         if (c == "mix.bounce") sent_argument = ctx.argument;
                         return engine.send(c, ctx);
                     });
    session.installCommands(router.local());
    router.prepare(qt_device_chords(true));
    ShellModel model(engine, router);
    model.tick();
    CHECK(model.bounceAvailable() && model.bounceState() == -1);
    const int base = model.notices().size();   // what the shell holds already

    QQuickView view;
    view.resize(1280, 720);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)},
                               {QStringLiteral("session"), QVariant::fromValue(&session)}});
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
    CHECK(view.status() == QQuickView::Ready);
    view.show();
    CHECK(QTest::qWaitForWindowExposed(&view));
    QQuickItem* root = view.rootObject();

    // File > Bounce Mix… opens it.
    QQuickItem* win = named(root, QStringLiteral("bounceWindow"));
    CHECK(win != nullptr && !win->isVisible());
    click(view, named(root, QStringLiteral("fileMenuButton")));
    QQuickItem* item = named(root, QStringLiteral("fileBounceMix"));
    CHECK(QTest::qWaitFor([&]() { return item->isVisible(); }));
    click(view, item);
    CHECK(session.bounceOpen() && win->isVisible());
    CHECK(text_of(win, "bounceTargetValue") == QStringLiteral("-16 LUFS"));

    // The choices are the argument: real time, 32-bit float, normalise to
    // -14 LUFS, a name.
    click(view, named(win, QStringLiteral("bounceRealTime")));
    click(view, named(win, QStringLiteral("bounceFloat32")));
    CHECK(win->property("bits").toInt() == 32);
    click(view, named(win, QStringLiteral("bounceFile_aiff")));
    // AIFF holds no float: the depth falls back to 24 and the button is off.
    CHECK(win->property("bits").toInt() == 24);
    click(view, named(win, QStringLiteral("bounceInt16")));
    click(view, named(win, QStringLiteral("bounceRate_44100")));
    click(view, named(win, QStringLiteral("bounceDither_shaped")));
    click(view, named(win, QStringLiteral("bounceNormalise")));
    click(view, named(win, QStringLiteral("bounceTargetUp")));
    click(view, named(win, QStringLiteral("bounceTargetUp")));
    CHECK(text_of(win, "bounceTargetValue") == QStringLiteral("-14 LUFS"));
    named(win, QStringLiteral("bounceName"))->setProperty("text", QStringLiteral("Take 2"));
    click(view, named(win, QStringLiteral("bounceStart")));
    api::BounceOptions sent;
    std::string why;
    CHECK(api::parse_bounce_argument(sent_argument, sent, why));
    CHECK(sent.mode == api::BounceMode::RealTime && sent.format == api::BounceFormat::Int16);
    CHECK(sent.file == api::BounceFile::Aiff && sent.sample_rate == 44100);
    CHECK(sent.dither == api::BounceDither::Shaped);
    CHECK(sent.normalise && sent.target_lufs == -14.0 && sent.name == "Take 2");
    CHECK(sent.start == 0 && sent.end == 0);

    // Running: the fake says half, then Done on the next poll.
    model.tick();
    CHECK(model.bounceRunning() && model.bounceProgress() == 0.5);
    CHECK(named(win, QStringLiteral("bounceProgress"))->isVisible());
    CHECK(model.notices().size() == base);
    model.tick();
    CHECK(model.bounceState() == static_cast<int>(api::BounceState::Done));
    CHECK(!model.bounceRunning());

    // Done is a held notice, not the status line, and stays after the
    // status line has cleared.
    CHECK(model.notices().size() == base + 1 && model.notices().last().startsWith(QStringLiteral("Bounced to ")));
    CHECK(!model.statusText().contains(QStringLiteral("Bounced")));
    QQuickItem* result = named(win, QStringLiteral("bounceResult"));
    CHECK(result->isVisible() && text_of(win, "bounceResult").contains(QStringLiteral("Bounce_01.wav")));
    QTest::qWait(2700);
    model.tick();
    CHECK(model.notices().size() == base + 1);

    // A second request while one runs is refused, and the reason is held in
    // the window.
    model.startBounce(false, QStringLiteral("bwf"), 24, 0, QStringLiteral("auto"), false, -16.0, QString());
    model.startBounce(false, QStringLiteral("bwf"), 24, 0, QStringLiteral("auto"), false, -16.0, QString());
    model.tick();
    CHECK(model.bounceRefusal().contains(QStringLiteral("already running")));
    QQuickItem* refusal = named(win, QStringLiteral("bounceRefusal"));
    CHECK(refusal->isVisible());
    // Cancelling ends it with its own held notice.
    click(view, named(win, QStringLiteral("bounceCancel")));
    model.tick();
    CHECK(model.bounceState() == static_cast<int>(api::BounceState::Cancelled));
    CHECK(model.notices().size() == base + 1 && model.notices().last().startsWith(QStringLiteral("Bounce stopped")));

    // Dismiss puts the notice away.
    click(view, named(win, QStringLiteral("bounceDismiss")));
    CHECK(model.notices().size() == base && model.bounceState() == -1);

    click(view, named(win, QStringLiteral("bounceClose")));
    CHECK(!session.bounceOpen() && !win->isVisible());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    if (bounce_window_over_the_fake_engine()) return 1;
    std::printf("ui bounce window: ok\n");
    return 0;
}
