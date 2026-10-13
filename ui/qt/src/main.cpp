// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-ui: the Qt 6 interface, a separate process from the engine
// (ADR-0007 §3). Until M4 it draws a synthetic engine through EngineAdapter;
// see engine_adapter.h for exactly what M4 replaces.
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QImage>
#include <QQmlEngine>
#include <QQuickView>
#include <QTimer>
#include <chrono>
#include <cstdio>
#include <memory>

#include "key_router.h"
#include "measure.h"
#include "qt_keys.h"
#include "register_types.h"
#include "render_path.h"
#include "shell_model.h"
#include "synthetic_engine.h"

using namespace sonora;
using namespace sonora::ui;

namespace {

struct Options {
    QString keymap;
    int tracks = 18;
    int meter_hz = 0;  // 0: 60, or 30 on a software rasteriser
    bool kiosk = false;
    double measure_s = 0;
    double warmup_s = 2;
    QString measure_label;
    QString screenshot;
    double after_s = 3;
    double engine_dies_after_s = -1;
    bool no_meters = false;
    bool demo = false;
};

bool parse(QGuiApplication& app, Options& o) {
    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("SonoraOS interface"));
    p.addHelpOption();
    const QCommandLineOption keymap({QStringLiteral("keymap")},
                                    QStringLiteral("Key map profile (JSON)."),
                                    QStringLiteral("path"));
    const QCommandLineOption tracks({QStringLiteral("tracks")},
                                    QStringLiteral("Tracks the synthetic engine offers (default 18)."),
                                    QStringLiteral("n"));
    const QCommandLineOption hz({QStringLiteral("meter-hz")},
                                QStringLiteral("Meter rate; default 60, 30 on a software rasteriser."),
                                QStringLiteral("hz"));
    const QCommandLineOption kiosk({QStringLiteral("kiosk")},
                                   QStringLiteral("Full screen, as under the kiosk compositor."));
    const QCommandLineOption measure({QStringLiteral("measure")},
                                     QStringLiteral("Measure for N seconds after warm-up, print, exit."),
                                     QStringLiteral("seconds"));
    const QCommandLineOption label({QStringLiteral("measure-label")},
                                   QStringLiteral("Label printed with the measurement."),
                                   QStringLiteral("text"));
    const QCommandLineOption shot({QStringLiteral("screenshot")},
                                  QStringLiteral("Save the window to a PNG and exit."),
                                  QStringLiteral("path"));
    const QCommandLineOption after({QStringLiteral("after")},
                                   QStringLiteral("Seconds before --screenshot (default 3)."),
                                   QStringLiteral("seconds"));
    const QCommandLineOption dies({QStringLiteral("engine-dies-after")},
                                  QStringLiteral("Stop the synthetic engine's heartbeat after N s."),
                                  QStringLiteral("seconds"));
    const QCommandLineOption nometers({QStringLiteral("no-meters")},
                                      QStringLiteral("Hide the meters (baseline for --measure)."));
    const QCommandLineOption demo({QStringLiteral("demo")},
                                  QStringLiteral("Arm tracks 1-8 and start a take, through the "
                                                 "same commands their keys send."));
    p.addOptions({keymap, tracks, hz, kiosk, measure, label, shot, after, dies, nometers, demo});
    p.process(app);

    o.keymap = p.value(keymap);
    if (p.isSet(tracks)) o.tracks = p.value(tracks).toInt();
    if (p.isSet(hz)) o.meter_hz = p.value(hz).toInt();
    o.kiosk = p.isSet(kiosk);
    if (p.isSet(measure)) o.measure_s = p.value(measure).toDouble();
    o.measure_label = p.value(label);
    o.screenshot = p.value(shot);
    if (p.isSet(after)) o.after_s = p.value(after).toDouble();
    if (p.isSet(dies)) o.engine_dies_after_s = p.value(dies).toDouble();
    o.no_meters = p.isSet(nometers);
    o.demo = p.isSet(demo);
    if (o.tracks < 1 || o.tracks > 64) {
        std::fprintf(stderr, "sonora-ui: --tracks must be 1..64\n");
        return false;
    }
    return true;
}

struct App {
    Options opt;
    ShellModel* model = nullptr;
};

void on_render_path(const RenderPath& p, void* ctx) {
    auto* app = static_cast<App*>(ctx);
    // ADR-0006: lowering the meter rate to protect the callback is always
    // acceptable. A CPU rasteriser gets half the rate unless told otherwise.
    if (app->opt.meter_hz == 0 && p.software) app->model->setMeterHz(30);
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("sonora-ui"));

    App a;
    if (!parse(app, a.opt)) return 2;

    std::string refusal;
    const KeyMap keymap = keymap_or_built_in(a.opt.keymap.toStdString(), refusal);
    if (!refusal.empty()) std::fprintf(stderr, "sonora-ui: %s\n", refusal.c_str());

    SyntheticEngine engine(static_cast<uint32_t>(a.opt.tracks));
    std::string error;
    if (!engine.connect(error)) {
        std::fprintf(stderr, "sonora-ui: %s\n", error.c_str());
        return 1;
    }
    if (a.opt.engine_dies_after_s >= 0) {
        // Same clock the synthetic engine reads.
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        engine.die_at(static_cast<uint64_t>(now) +
                      static_cast<uint64_t>(a.opt.engine_dies_after_s * 1e9));
    }

    KeyRouter router(keymap, engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    // app.quit is the interface's own under ADR-0007 §3/§5: it closes this
    // process and nothing else. The engine, and any take, carry on; on the
    // appliance the UI's unit starts it again. Whether it should instead
    // close the session or power the machine off is not decided here.
    router.local().install("app.quit", [](const CommandContext&) {
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        return true;
    });
    const std::vector<std::string> notices =
        router.prepare(qt_device_chords(qt_scan_codes_are_xkb(QGuiApplication::platformName())));

    register_types();
    ShellModel model(engine, router);
    a.model = &model;
    if (!refusal.empty()) model.addNotice(QString::fromStdString(refusal));
    for (const auto& n : notices) {
        std::fprintf(stderr, "sonora-ui: key map \"%s\": %s\n", keymap.name().c_str(), n.c_str());
        model.addNotice(QString::fromStdString(n));
    }

    QQuickView view;
    view.setTitle(QStringLiteral("Sonora"));
    view.setColor(QColor(0x0f, 0x11, 0x14));
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)}});
    QObject::connect(view.engine(), &QQmlEngine::quit, &app, &QCoreApplication::quit);
    probe_render_path(&view, &model, on_render_path, &a);
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    if (view.status() == QQuickView::Error) {
        for (const auto& e : view.errors()) {
            std::fprintf(stderr, "sonora-ui: %s\n", qPrintable(e.toString()));
        }
        return 1;
    }
    if (a.opt.no_meters) model.hideMeters();
    model.routeKeysFrom(&view);
    model.start(a.opt.meter_hz > 0 ? a.opt.meter_hz : 60);

    if (a.opt.kiosk) {
        view.showFullScreen();
    } else {
        view.resize(1280, 720);
        view.show();
    }

    if (a.opt.demo) {
        for (int t = 0; t < 8 && t < a.opt.tracks; ++t) model.invoke(QStringLiteral("track.toggle_arm"), t);
        model.invoke(QStringLiteral("transport.record"), -1);
    }

    std::unique_ptr<FrameMeter> meter;
    TickStats ticks;
    if (a.opt.measure_s > 0) {
        const auto frames = static_cast<std::size_t>(a.opt.measure_s * 240) + 64;
        meter = std::make_unique<FrameMeter>(frames);
        meter->attach(&view);
        QTimer::singleShot(static_cast<int>(a.opt.warmup_s * 1000), &app, [&]() {
            ticks.tick_ns.reserve(frames);
            ticks.interval_ns.reserve(frames);
            model.setTickStats(&ticks);
            meter->begin();
        });
        QTimer::singleShot(static_cast<int>((a.opt.warmup_s + a.opt.measure_s) * 1000), &app,
                           [&]() {
                               model.setTickStats(nullptr);
                               const QString r = meter->report(
                                   ticks, a.opt.measure_label, a.opt.no_meters ? 0 : model.attachedMeters(),
                                   model.meterHz(), model.renderPath());
                               std::fputs(qPrintable(r), stdout);
                               std::fflush(stdout);
                               if (a.opt.screenshot.isEmpty()) app.quit();
                           });
    }
    if (!a.opt.screenshot.isEmpty()) {
        const double at = a.opt.measure_s > 0 ? a.opt.warmup_s + a.opt.measure_s + 0.2 : a.opt.after_s;
        QTimer::singleShot(static_cast<int>(at * 1000), &app, [&]() {
            const QImage img = view.grabWindow();
            if (img.isNull() || !img.save(a.opt.screenshot)) {
                std::fprintf(stderr, "sonora-ui: could not save %s\n", qPrintable(a.opt.screenshot));
                app.exit(1);
                return;
            }
            std::fprintf(stderr, "sonora-ui: saved %s (%dx%d)\n", qPrintable(a.opt.screenshot),
                         img.width(), img.height());
            app.quit();
        });
    }
    return app.exec();
}
