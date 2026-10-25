// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-ui: the Qt 6 interface, a separate process from the engine
// (ADR-0007 §3). It reaches the engine through EngineAdapter on the control
// API (control_adapter.h): the real engine on its socket when one is
// listening (ui/client's SocketEngine), FakeEngine, the provider's fake with
// a synthetic signal on every input, when none is or with --fake-engine.
// The session it edits is the engine's document, replicated here
// (ReplicaDocument behind TimelineDocument, the model seam).
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QImage>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>

#include "control_adapter.h"
#include "key_router.h"
#include "keymap_profiles.h"
#include "measure.h"
#include "peak_source.h"
#include "qt_keys.h"
#include "refusal_page.h"
#include "register_types.h"
#include "render_path.h"
#include "replica_document.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/client/socket_engine.h"
#include "sonora/platform/ipc.h"
#include "theme.h"
#include "timeline_document.h"
#include "transport_model.h"

using namespace sonora;
using namespace sonora::ui;

namespace {

struct Options {
    QString keymap;
    QString keymap_dir;
    QString keys_view;
    bool transport_view = false;
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
    QString window = QStringLiteral("edit");
    QString sessions_dir;
    QString engine_socket;
    bool fake_engine = false;
    QString insert_view;
    QString edit_view;
    QString mix_view;
    QString open_file;
};

// The demo session's inserts, on the fake engine's catalog (fake_catalog()):
// a few plugins on the drums, the bass and the vocal; one bypassed, one
// inactive, one whose plugin is not installed. Returns the insert the demo
// scripts a crash for, so the Mix window shows a fault.
model::Id add_demo_inserts(model::Snapshot& s) {
    const auto catalog = api::testing::fake_catalog();
    const auto ref = [&catalog](std::size_t i) {
        model::PluginRef r;
        r.format = catalog[i].format == api::PluginFormat::Vst3 ? model::PluginFormat::Vst3
                                                                : model::PluginFormat::Lv2;
        r.id = catalog[i].id;
        r.name = catalog[i].name;
        r.version = catalog[i].version;
        return r;
    };
    constexpr std::size_t kGain = 0, kComp = 2;
    model::Id crashed = 0;
    const auto apply = [&s](const model::Operation& op) {
        model::Snapshot after;
        std::string why;
        if (model::apply(s, op, after, why)) {
            s = std::move(after);
        } else {
            std::fprintf(stderr, "sonora-ui: the demo inserts: %s\n", why.c_str());
        }
    };
    const auto add = [&](std::size_t track, uint8_t slot, model::PluginRef plugin,
                         std::vector<model::ParamValue> params = {}) {
        if (track >= s.edit->tracks.size()) return model::Id{0};
        model::AddInsert op;
        op.track = s.edit->tracks[track]->id;
        op.insert = s.next_id;
        op.slot = slot;
        op.plugin = std::move(plugin);
        op.params = std::move(params);
        apply(op);
        return op.insert;
    };
    add(0, 0, ref(kComp), {{"0", 0.32f}, {"1", 0.6f}, {"7", 0.5f}});
    add(0, 1, ref(kGain), {{"gain", -3.0f}});
    add(1, 0, ref(kGain), {{"gain", 2.5f}});
    apply(model::SetInsertBypass{add(1, 1, ref(kComp)), true});
    apply(model::SetInsertActive{add(2, 0, ref(kGain)), false});
    crashed = add(3, 0, ref(kComp));
    add(8, 0, ref(kComp), {{"0", 0.4f}, {"1", 0.3f}});
    add(9, 0, ref(kGain));
    add(14, 0, ref(kComp), {{"0", 0.28f}, {"1", 0.45f}, {"7", 1.0f}});
    add(14, 1, ref(kGain), {{"gain", 1.5f}});
    apply(model::SetInsertBypass{add(14, 2, ref(kGain)), true});
    model::PluginRef missing;
    missing.format = model::PluginFormat::Lv2;
    missing.id = "urn:example:plate-reverb";
    missing.name = "Plate Reverb";
    missing.version = "1.4.0";
    add(15, 0, missing);
    return crashed;
}

// A tempo map for the demo, for the Transport window's screenshot: 120 bpm
// 4/4 to bar 17, 96 bpm from there, 3/4 at 96 from bar 25. Regions stay
// where they are in samples (session-format.md, "tempo"); only the bar
// lines move.
void add_demo_tempo_map(model::Snapshot& s) {
    const model::Frames rate = s.settings.sample_rate;
    const model::Frames bar120 = rate * 2;       // 4 beats at 120 bpm
    const model::Frames bar96 = rate * 5 / 2;    // 4 beats at 96 bpm
    model::SetSettings op;
    op.settings = s.settings;
    op.settings.tempo = {model::TempoEntry{0, 120.0f, {{4, 4}}},
                         model::TempoEntry{16 * bar120, 96.0f, {{4, 4}}},
                         model::TempoEntry{16 * bar120 + 8 * bar96, 96.0f, {{3, 4}}}};
    model::Snapshot after;
    std::string why;
    if (model::apply(s, op, after, why)) {
        s = std::move(after);
        s.revision = 0;
    } else {
        std::fprintf(stderr, "sonora-ui: the demo tempo map: %s\n", why.c_str());
    }
}

bool parse(QGuiApplication& app, Options& o) {
    QCommandLineParser p;
    p.setApplicationDescription(QStringLiteral("SonoraOS interface"));
    p.addHelpOption();
    const QCommandLineOption keymap({QStringLiteral("keymap")},
                                    QStringLiteral("Key map profile (JSON)."),
                                    QStringLiteral("path"));
    const QCommandLineOption keymap_dir_opt(
        {QStringLiteral("keymap-dir")},
        QStringLiteral("Folder of key map profiles the Keys menu offers (default "
                       "$SONORA_KEYMAP_DIR, else --keymap's folder, else "
                       "share/sonora/keymaps)."),
        QStringLiteral("path"));
    const QCommandLineOption keys_view({QStringLiteral("keys-view")},
                                       QStringLiteral("Open the Keys menu (profiles), for "
                                                      "screenshots."),
                                       QStringLiteral("profiles"));
    p.addOptions({keymap_dir_opt, keys_view});
    const QCommandLineOption transport_view(
        {QStringLiteral("transport-view")},
        QStringLiteral("With --demo, a tempo map in the demo session and the Transport "
                       "window open, for screenshots."));
    p.addOption(transport_view);
    const QCommandLineOption tracks({QStringLiteral("tracks")},
                                    QStringLiteral("Tracks the fake engine offers (default 18)."),
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
                                  QStringLiteral("Stop the fake engine's heartbeat after N s."),
                                  QStringLiteral("seconds"));
    const QCommandLineOption nometers({QStringLiteral("no-meters")},
                                      QStringLiteral("Hide the meters (baseline for --measure)."));
    const QCommandLineOption demo({QStringLiteral("demo")},
                                  QStringLiteral("Open a demo session of synthetic regions, arm "
                                                 "tracks 1-8 and start a take, through the same "
                                                 "commands their keys send."));
    const QCommandLineOption window({QStringLiteral("window")},
                                    QStringLiteral("Window to open on: edit (default) or mix."),
                                    QStringLiteral("name"));
    const QCommandLineOption sessions({QStringLiteral("sessions-dir")},
                                      QStringLiteral("Folder File > Save and Open use "
                                                     "(default $SONORA_SESSIONS_DIR, else ~/Sessions)."),
                                      QStringLiteral("path"));
    const QCommandLineOption socket({QStringLiteral("engine-socket")},
                                    QStringLiteral("The engine's control socket (default "
                                                   "$XDG_RUNTIME_DIR/sonora/engine.sock)."),
                                    QStringLiteral("path"));
    const QCommandLineOption fake({QStringLiteral("fake-engine")},
                                  QStringLiteral("Run on the built-in fake engine even when a real "
                                                 "one is listening."));
    const QCommandLineOption insert_view({QStringLiteral("insert-view")},
                                         QStringLiteral("With --demo on the fake engine, open the "
                                                        "plugin chooser (browser) or a parameter "
                                                        "panel (panel), for screenshots."),
                                         QStringLiteral("browser|panel"));
    const QCommandLineOption edit_view({QStringLiteral("edit-view")},
                                       QStringLiteral("With --demo, zoom the Edit window in on "
                                                      "the snare's crossfade and the fades "
                                                      "around it (fades), or copy the snare's "
                                                      "crossfaded edit, paste it on the next "
                                                      "track and open the Edit menu "
                                                      "(clipboard), or repeat the snare's "
                                                      "region three times and open the Repeat "
                                                      "dialog (repeat), or copy a range on the "
                                                      "first three tracks and paste it later "
                                                      "on them (multitrack), or copy the snare's "
                                                      "region and paste it on four tracks (fill), "
                                                      "or add a tempo map "
                                                      "and nudge a region past its change "
                                                      "(nudge), or make a few edits, undo the last "
                                                      "and open the Undo History window "
                                                      "(undo-history), or add markers and a "
                                                      "selection marker and open the Memory "
                                                      "Locations window (markers), for "
                                                      "screenshots."),
                                       QStringLiteral("fades|clipboard|repeat|multitrack|fill|nudge|undo-history|markers"));
    const QCommandLineOption mix_view({QStringLiteral("mix-view")},
                                      QStringLiteral("With --demo, solo Lead Vox and make Room "
                                                     "and Talkback solo safe (solo-safe), for "
                                                     "screenshots."),
                                      QStringLiteral("solo-safe"));
    const QCommandLineOption open({QStringLiteral("open")},
                                  QStringLiteral("Open this session file once the window is up, "
                                                 "as File > Open does (with --demo, instead of "
                                                 "starting a take)."),
                                  QStringLiteral("file"));
    p.addOptions({keymap, tracks, hz, kiosk, measure, label, shot, after, dies, nometers, demo,
                  window, sessions, socket, fake, insert_view, edit_view, mix_view, open});
    p.process(app);

    o.keymap = p.value(keymap);
    o.keymap_dir = p.value(keymap_dir_opt);
    o.keys_view = p.value(keys_view);
    if (!o.keys_view.isEmpty() && o.keys_view != QStringLiteral("profiles")) {
        std::fprintf(stderr, "sonora-ui: --keys-view must be profiles\n");
        return false;
    }
    if (p.isSet(tracks)) o.tracks = p.value(tracks).toInt();
    o.transport_view = p.isSet(transport_view);
    if (p.isSet(hz)) o.meter_hz = p.value(hz).toInt();
    o.kiosk = p.isSet(kiosk);
    if (p.isSet(measure)) o.measure_s = p.value(measure).toDouble();
    o.measure_label = p.value(label);
    o.screenshot = p.value(shot);
    if (p.isSet(after)) o.after_s = p.value(after).toDouble();
    if (p.isSet(dies)) o.engine_dies_after_s = p.value(dies).toDouble();
    o.no_meters = p.isSet(nometers);
    o.demo = p.isSet(demo);
    if (p.isSet(window)) o.window = p.value(window);
    o.sessions_dir = p.value(sessions);
    o.engine_socket = p.value(socket);
    o.fake_engine = p.isSet(fake);
    o.insert_view = p.value(insert_view);
    if (!o.insert_view.isEmpty() && o.insert_view != QStringLiteral("browser") &&
        o.insert_view != QStringLiteral("panel")) {
        std::fprintf(stderr, "sonora-ui: --insert-view must be browser or panel\n");
        return false;
    }
    o.mix_view = p.value(mix_view);
    o.open_file = p.value(open);
    if (!o.mix_view.isEmpty() && o.mix_view != QStringLiteral("solo-safe")) {
        std::fprintf(stderr, "sonora-ui: --mix-view must be solo-safe\n");
        return false;
    }
    o.edit_view = p.value(edit_view);
    if (!o.edit_view.isEmpty() && o.edit_view != QStringLiteral("fades") &&
        o.edit_view != QStringLiteral("clipboard") && o.edit_view != QStringLiteral("repeat") &&
        o.edit_view != QStringLiteral("multitrack") && o.edit_view != QStringLiteral("fill") &&
        o.edit_view != QStringLiteral("nudge") && o.edit_view != QStringLiteral("undo-history") &&
        o.edit_view != QStringLiteral("markers")) {
        std::fprintf(stderr, "sonora-ui: --edit-view must be fades, clipboard, repeat, "
                             "multitrack, fill, nudge, undo-history or markers\n");
        return false;
    }
    if (o.window != QStringLiteral("edit") && o.window != QStringLiteral("mix")) {
        std::fprintf(stderr, "sonora-ui: --window must be edit or mix\n");
        return false;
    }
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

// The refusal page in place of the windows, until the window is closed (not
// on the appliance, where nothing closes it). Exits 1 then: the interface
// never ran.
int show_refusal(QGuiApplication& app, const Options& opt, const QString& reason) {
    register_types();   // the page's Theme
    QQuickView view;
    view.setTitle(QStringLiteral("Sonora"));
    QString why;
    if (!load_refusal_page(view, reason, why)) {
        std::fprintf(stderr, "sonora-ui: %s\n", qPrintable(why));
        return 1;
    }
    QObject::connect(view.engine(), &QQmlEngine::quit, &app, &QCoreApplication::quit);
    if (opt.kiosk) {
        view.showFullScreen();
    } else {
        view.resize(1280, 720);
        view.show();
    }
    app.exec();
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("sonora-ui"));

    App a;
    if (!parse(app, a.opt)) return 2;

    // --keymap wins; else the profile chosen in the Keys menu last time;
    // else the built-in default.
    const std::string profiles_dir =
        keymap_dir(a.opt.keymap_dir.toStdString(), a.opt.keymap.toStdString(),
                   QCoreApplication::applicationDirPath().toStdString());
    const std::string remembered_at = remembered_profile_path();
    StartupProfile start =
        startup_profile(a.opt.keymap.toStdString(), remembered_at, profiles_dir);
    std::string profile = start.file;
    const std::string refusal = start.refusal;
    const KeyMap keymap = std::move(start.map);
    if (!refusal.empty()) std::fprintf(stderr, "sonora-ui: %s\n", refusal.c_str());
    std::string keymap_dir_notice;
    if (!a.opt.keymap_dir.isEmpty() && profiles_dir.empty()) {
        keymap_dir_notice = "no key map folder at " + a.opt.keymap_dir.toStdString() +
                            ": the Keys menu offers the built-in keys only";
        std::fprintf(stderr, "sonora-ui: %s\n", keymap_dir_notice.c_str());
    }

    // The real engine when one listens; the fake otherwise, said on screen,
    // so nobody mistakes the fake's synthetic tones for their inputs.
    const std::string endpoint = a.opt.engine_socket.isEmpty()
                                     ? platform::default_control_endpoint()
                                     : a.opt.engine_socket.toStdString();
    std::unique_ptr<api::EngineControl> backend;
    api::testing::FakeEngine* fake = nullptr;
    client::SocketEngine* socket = nullptr;
    std::string engine_notice;
    if (!a.opt.fake_engine && !endpoint.empty() && platform::endpoint_live(endpoint)) {
        client::SocketEngine::Options so;
        so.endpoint = endpoint;
        auto s = std::make_unique<client::SocketEngine>(so);
        socket = s.get();
        backend = std::move(s);
        std::fprintf(stderr, "sonora-ui: on the engine at %s\n", endpoint.c_str());
    } else {
        api::testing::FakeEngine::Config fake_config;
        fake_config.tracks = static_cast<uint32_t>(a.opt.tracks);
        auto f = std::make_unique<api::testing::FakeEngine>(fake_config);
        fake = f.get();
        backend = std::move(f);
        if (!a.opt.fake_engine) {
            engine_notice = "No engine is running" +
                            (endpoint.empty() ? std::string() : " at " + endpoint) +
                            ": this is the built-in fake engine, with synthetic inputs.";
            std::fprintf(stderr, "sonora-ui: %s\n", engine_notice.c_str());
        }
    }
    ControlAdapter engine(*backend);
    std::string error;
    if (!engine.connect(error)) {
        std::fprintf(stderr, "sonora-ui: %s\n", error.c_str());
        // Any other failure exits, and the unit's restart is the retry that
        // can fix it. A version refusal cannot be fixed by retrying, and an
        // exit would put the appliance on a black screen restarted for ever.
        if (socket != nullptr && socket->refused_version()) {
            return show_refusal(app, a.opt, QString::fromStdString(error));
        }
        return 1;
    }
    if (socket != nullptr) engine.setLinkError([socket]() { return socket->link_problem(); });
    if (a.opt.engine_dies_after_s >= 0) {
        if (fake != nullptr) {
            // Same clock the fake engine reads.
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
            fake->die_at(static_cast<uint64_t>(now) +
                         static_cast<uint64_t>(a.opt.engine_dies_after_s * 1e9));
        } else {
            std::fprintf(stderr, "sonora-ui: --engine-dies-after is the fake engine's; ignored\n");
        }
    }

    // The session is the engine's document. Until its first Snapshot is
    // read (the first tick) empty tracks, one per input, stand in. With
    // --demo, synthetic regions laid out like a tracking session are sent
    // to the engine as the session to open, and the Edit window says the
    // audio is synthetic.
    const uint32_t rate = engine.info().sample_rate;
    ReplicaDocument document(
        [&engine](const api::ModelRequest& r) { return engine.submitModel(r); },
        make_empty_session(engine.info().track_count, rate));
    // On the fake engine the demo has inserts too, from its scripted catalog;
    // a real engine's catalog is its own, so there the demo has none.
    sonora::model::Id demo_crash = 0;
    if (a.opt.demo) {
        std::string why;
        sonora::model::Snapshot demo = make_demo_session(engine.info().track_count, rate);
        if (a.opt.transport_view || a.opt.edit_view == QStringLiteral("nudge"))
            add_demo_tempo_map(demo);
        if (fake != nullptr) {
            demo_crash = add_demo_inserts(demo);
            fake->script_plugin_latency(api::testing::fake_catalog()[2].id, 64);
        }
        if (!document.reset(std::move(demo), why)) {
            std::fprintf(stderr, "sonora-ui: the demo session: %s\n", why.c_str());
        }
    }
    // No peak files exist yet, so every waveform is synthetic whichever
    // session is open; the window says so whenever it draws one.
    SyntheticPeakSource peaks(rate, 120.0);
    SessionModel session(document, peaks, true);
    if (!a.opt.sessions_dir.isEmpty()) session.setSessionsDir(a.opt.sessions_dir);
    document.on_refused = [&session](const std::string& why) {
        emit session.notice(QString::fromStdString(why));
    };
    engine.setModelSink([&document, &session](const std::vector<api::ModelUpdate>& updates,
                                              const std::vector<api::CommandReply>& replies) {
        if (document.receive(updates, replies)) session.documentUpdated();
    });
    document.on_load_abandoned = [&session](const std::string& refused, bool later_load) {
        if (refused.empty()) {
            session.openWasLost();
        } else {
            session.openWasRefused(QString::fromStdString(refused), later_load);
        }
    };
    // Fader rides are heard through ParameterChange and saved at release.
    // The fake engine plays nothing, and the Mix window says so.
    MixModel mixer(
        session,
        [&engine](uint32_t track, float linear) { engine.send_track_gain(track, linear); },
        [&engine]() { return engine.info().track_count; }, fake == nullptr);
    InsertModel inserts(document, session);
    inserts.controller().set_param_sender(
        [&engine](sonora::model::Id insert, const std::string& key, float value) {
            engine.send_plugin_parameter(insert, key, value);
        });
    if (socket != nullptr) {
        engine.watchConnection(
            [socket]() { return socket->connected() ? socket->greetings() : 0; },
            [&document, &session, &inserts](bool up) {
                // A new connection replays the faults that still stand.
                if (up) inserts.onLinkReplaced();
                if (document.set_link(up)) session.documentUpdated();
            });
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
    session.installCommands(router.local());
    // Where the counters are, read from the same session and the same
    // insertion point as SessionModel's.
    TransportModel transport(
        [&session]() -> const sonora::model::SessionSettings& {
            return session.controller().snapshot().settings;
        },
        [&session]() { return session.controller().selection().insertion; });
    transport.installCommands(router.local());
    router.setSelection([&session]() { return session.selectedTrackForCommands(); });
    const std::vector<std::string> notices =
        router.prepare(qt_device_chords(qt_scan_codes_are_xkb(QGuiApplication::platformName())));

    register_types();
    ShellModel model(engine, router);
    a.model = &model;
    QObject::connect(&session, &SessionModel::openNotice, &model, &ShellModel::setOpenNotice);
    link_transport(model, session, transport);
    ShellModel::PluginSinks plugin_sinks;
    plugin_sinks.catalog = [&inserts](const api::PluginCatalog& c) { inserts.onCatalog(c); };
    plugin_sinks.fault = [&inserts](uint64_t insert, const std::string& text) {
        inserts.onFault(insert, text);
    };
    plugin_sinks.frame = [&inserts](const EngineFrame& f, bool alive) { inserts.onFrame(f, alive); };
    model.setPluginSinks(std::move(plugin_sinks));
    session.setWindow(a.opt.window);
    if (a.opt.transport_view) transport.setOpen(true);
    if (!engine_notice.empty()) model.addNotice(QString::fromStdString(engine_notice));
    if (!keymap_dir_notice.empty()) model.addNotice(QString::fromStdString(keymap_dir_notice));
    // A switch in the Keys menu replaces these; the engine's stay.
    QStringList keymap_notices;
    if (!refusal.empty()) keymap_notices << QString::fromStdString(refusal);
    QStringList essentials_notices;
    for (const auto& n : notices) {
        std::fprintf(stderr, "sonora-ui: key map \"%s\": %s\n", keymap.name().c_str(), n.c_str());
        essentials_notices << QString::fromStdString(n);
    }
    model.setKeymapNotices(keymap_notices, essentials_notices);
    {
        std::vector<ProfileEntry> entries = list_profiles(profiles_dir, profile);
        // The built-in keys are listed as keymaps/default.json when that
        // file is there; it is the same map (test_keymap holds them equal).
        if (profile.empty()) {
            for (const ProfileEntry& e : entries) {
                if (e.error.empty() && e.name == KeyMap::built_in_default().name()) profile = e.file;
            }
        }
        model.setProfiles(std::move(entries), profile,
                          [remembered_at, profiles_dir](const std::string& file, std::string& why) {
                              return remember_profile(remembered_at, profiles_dir, file, why);
                          });
    }

    QQuickView view;
    view.setTitle(QStringLiteral("Sonora"));
    view.setColor(QColor::fromRgba(theme::kWindow));
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)},
                               {QStringLiteral("session"), QVariant::fromValue(&session)},
                               {QStringLiteral("inserts"), QVariant::fromValue(&inserts)},
                               {QStringLiteral("mixer"), QVariant::fromValue(&mixer)}});
    QObject::connect(view.engine(), &QQmlEngine::quit, &app, &QCoreApplication::quit);
    probe_render_path(&view, &model, on_render_path, &a);
    view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
    if (view.status() == QQuickView::Error) {
        for (const auto& e : view.errors()) {
            std::fprintf(stderr, "sonora-ui: %s\n", qPrintable(e.toString()));
        }
        return 1;
    }
    view.rootObject()->setProperty("transport", QVariant::fromValue(&transport));
    if (a.opt.no_meters) model.hideMeters();
    model.routeKeysFrom(&view);
    if (a.opt.keys_view == QStringLiteral("profiles")) {
        if (QObject* panel = view.rootObject()->findChild<QObject*>(QStringLiteral("profilePanel"))) {
            panel->setProperty("visible", true);
        }
    }
    model.start(a.opt.meter_hz > 0 ? a.opt.meter_hz : 60);

    if (a.opt.kiosk) {
        view.showFullScreen();
    } else {
        view.resize(1280, 720);
        view.show();
    }

    if (a.opt.demo) {
        for (int t = 0; t < 8 && t < a.opt.tracks; ++t) model.invoke(QStringLiteral("track.toggle_arm"), t);
        // Not under --edit-view: a rolling transport pages the view to the
        // playhead, away from what the view is meant to show.
        // Nor under --open: a session does not open over a take.
        if (a.opt.edit_view.isEmpty() && a.opt.open_file.isEmpty()) {
            model.invoke(QStringLiteral("transport.record"), -1);
        }
        // Once the window has its size: the whole session in view, and the
        // snare's second region selected, so a screenshot shows what a
        // selected region looks like.
        QTimer::singleShot(200, &app, [&session, &model, &inserts, &a, &view, fake, demo_crash]() {
            // The fake engine has the demo session by now; a crash on Tom 1's
            // compressor shows what a fault looks like.
            if (fake != nullptr && demo_crash != 0) {
                fake->script_insert_fault(
                    demo_crash, "its host crashed (SIGSEGV); the chain plays dry until Retry");
            }
            if (a.opt.insert_view == QStringLiteral("browser")) inserts.openBrowser(14, 3);
            if (a.opt.insert_view == QStringLiteral("panel")) inserts.openPanel(0, 0);
            // A vocal soloed over a session whose room mic and talkback are
            // solo safe: those two stay audible and say so, the rest are
            // outlined as the solo silences them.
            if (a.opt.mix_view == QStringLiteral("solo-safe")) {
                EditController& c = session.controller();
                std::string why;
                for (const int t : {7, 17}) {
                    if (t < session.trackCount() && !c.toggle_solo_safe(t, why))
                        std::fprintf(stderr, "sonora-ui: solo safe: %s\n", why.c_str());
                }
                if (14 < session.trackCount()) model.invoke(QStringLiteral("track.toggle_solo"), 14);
            }
            model.invoke(QStringLiteral("zoom.fit_session"), -1);
            const int track = std::min(1, session.trackCount() - 1);
            const auto* p =
                active_playlist(session.controller().snapshot().edit->tracks[track].get());
            if (p && p->regions.size() > 1) session.controller().select_region(p->regions[1].id);
            model.invoke(QStringLiteral("track.select"), track);
            // Zoomed in about the snare's crossfade, through the zoom keys'
            // commands, so the fades are wide enough to read.
            if (a.opt.edit_view == QStringLiteral("fades") && p && p->crossfades.size() == 1) {
                EditController& c = session.controller();
                const auto& x = p->crossfades[0];
                const sonora::model::Region* right = nullptr;
                for (const auto& r : p->regions) {
                    if (r.id == x.right) right = &r;
                }
                if (right != nullptr) {
                    c.set_insertion(right->position);
                    for (int i = 0; i < 2; ++i) model.invoke(QStringLiteral("zoom.in"), -1);
                }
            }
            // The snare's region repeated three times through the dialog's
            // command, then Alt+R's dialog open again, offering 3.
            if (a.opt.edit_view == QStringLiteral("repeat") && p && p->regions.size() > 1) {
                // An id, not `p`: the repeat replaces the snapshot `p` is in.
                const auto snare = p->regions[1].id;
                session.controller().select_region(snare);
                if (!model.invokeWith(QStringLiteral("edit.repeat"), -1, QStringLiteral("3")))
                    std::fprintf(stderr, "sonora-ui: repeat: %s\n", qPrintable(model.statusText()));
                model.invoke(QStringLiteral("zoom.fit_session"), -1);
                session.controller().select_region(snare);
                model.invoke(QStringLiteral("edit.repeat"), -1);
            }
            // A range on the first three tracks copied and pasted after
            // their last region, through the keys' commands: the pasted
            // range stays selected across the three lanes.
            if (a.opt.edit_view == QStringLiteral("multitrack") && p && p->regions.size() > 2 &&
                session.trackCount() >= 3) {
                EditController& c = session.controller();
                const auto from = p->regions[1].position, to = p->regions[2].end();
                c.select_range(0, from, to, 3);
                model.invoke(QStringLiteral("edit.copy"), -1);
                sonora::model::Frames after = to;
                for (int i = 0; i < 3; ++i) {
                    const auto* lane = active_playlist(c.snapshot().edit->tracks[i].get());
                    if (lane && !lane->regions.empty())
                        after = std::max(after, lane->regions.back().end());
                }
                const auto at = after + (to - from) / 4;
                c.select_range(0, at, at, 3);
                model.invoke(QStringLiteral("edit.paste"), -1);
                model.invoke(QStringLiteral("zoom.fit_session"), -1);
                if (c.selection().tracks != 3)
                    std::fprintf(stderr, "sonora-ui: multitrack: %s\n", qPrintable(model.statusText()));
            }
            // The snare's region copied and pasted into a range on four
            // tracks after the session's end: one track's clipboard fills
            // every selected track, as in Pro Tools.
            if (a.opt.edit_view == QStringLiteral("fill") && p && p->regions.size() > 1 &&
                session.trackCount() >= 5) {
                EditController& c = session.controller();
                const auto snare = p->regions[1];
                c.select_region(snare.id);
                model.invoke(QStringLiteral("edit.copy"), -1);
                sonora::model::Frames after = 0;
                for (int i = 1; i < 5; ++i) {
                    const auto* lane = active_playlist(c.snapshot().edit->tracks[i].get());
                    if (lane && !lane->regions.empty())
                        after = std::max(after, lane->regions.back().end());
                }
                const auto at = after + snare.length / 4;
                c.select_range(track, at, at + snare.length, 4);
                model.invoke(QStringLiteral("edit.paste"), -1);
                model.invoke(QStringLiteral("zoom.fit_session"), -1);
                if (c.selection().tracks != 4)
                    std::fprintf(stderr, "sonora-ui: fill: %s\n", qPrintable(model.statusText()));
            }
            // On the demo's tempo map, the snare separated where 96 bpm
            // starts and the part after it nudged forward two beats on the
            // 1/4 grid: the gap is two 96 bpm beats and the region lands on
            // 17|3, not on 120 bpm steps.
            if (a.opt.edit_view == QStringLiteral("nudge")) {
                EditController& c = session.controller();
                const auto& tempo = c.snapshot().settings.tempo;
                const auto change = tempo.size() > 1 ? tempo[1].at : 0;
                std::string why;
                c.select_track(track);
                c.set_insertion(change);
                if (!c.separate(why)) std::fprintf(stderr, "sonora-ui: nudge: %s\n", why.c_str());
                sonora::model::Id pick = kNoId;
                if (const auto* pl = active_playlist(c.snapshot().edit->tracks[track].get())) {
                    for (const auto& r : pl->regions) {
                        if (r.position == change) pick = r.id;
                    }
                }
                if (pick != kNoId && c.select_region(pick)) {
                    c.set_grid(GridValue::Beat);
                    for (int i = 0; i < 2; ++i) model.invoke(QStringLiteral("nudge.forward"), -1);
                    c.set_insertion(change);
                    for (int i = 0; i < 3; ++i) model.invoke(QStringLiteral("zoom.in"), -1);
                } else {
                    std::fprintf(stderr, "sonora-ui: nudge: no region at the tempo change\n");
                }
            }
            // A few edits through the keys' commands, the last one undone, and
            // the Undo History window open on them: done steps, the line, and
            // the undone step in italics below it.
            if (a.opt.edit_view == QStringLiteral("undo-history") && p && p->regions.size() > 1) {
                EditController& c = session.controller();
                const auto snare = p->regions[1].id;
                c.select_region(snare);
                model.invoke(QStringLiteral("edit.duplicate"), -1);
                c.select_region(snare);
                if (!model.invokeWith(QStringLiteral("edit.repeat"), -1, QStringLiteral("2")))
                    std::fprintf(stderr, "sonora-ui: repeat: %s\n", qPrintable(model.statusText()));
                const auto* first = active_playlist(c.snapshot().edit->tracks[0].get());
                if (first && !first->regions.empty()) {
                    const auto kick = first->regions[0].id;
                    c.select_region(kick);
                    model.invoke(QStringLiteral("edit.duplicate"), -1);
                    c.select_region(kick);
                    model.invoke(QStringLiteral("edit.clear"), -1);
                }
                model.invoke(QStringLiteral("edit.undo"), -1);
                model.invoke(QStringLiteral("zoom.fit_session"), -1);
                model.invoke(QStringLiteral("window.undo_history"), -1);
            }
            // Markers through marker.add, as the New Marker dialog sends them,
            // one of them a selection marker over a range, then the Memory
            // Locations window open on them, the selection marker recalled.
            if (a.opt.edit_view == QStringLiteral("markers")) {
                EditController& c = session.controller();
                const double end = static_cast<double>(c.session_end());
                const struct {
                    const char* name;
                    double at, length;
                } marks[] = {{"Intro", 0.0, 0.0},
                             {"Verse 1", 0.12, 0.0},
                             {"Chorus", 0.36, 0.18},
                             {"Verse 2", 0.62, 0.0},
                             {"Outro", 0.86, 0.0}};
                for (const auto& m : marks) {
                    const auto at = static_cast<sonora::model::Frames>(end * m.at);
                    if (m.length > 0) {
                        c.select_range(std::max(0, c.selection().track), at,
                                       at + static_cast<sonora::model::Frames>(end * m.length));
                    } else {
                        c.set_insertion(at);
                    }
                    if (!model.invokeWith(QStringLiteral("marker.add"), -1, QString::fromUtf8(m.name)))
                        std::fprintf(stderr, "sonora-ui: marker: %s\n", qPrintable(model.statusText()));
                }
                session.recallMarker(2);
                model.invoke(QStringLiteral("window.memory_locations"), -1);
            }
            // The snare's two crossfaded regions copied and pasted after the
            // next track's last region, through the keys' commands, with the
            // Edit menu open on "Undo Paste".
            if (a.opt.edit_view == QStringLiteral("clipboard") && p && p->regions.size() > 2 &&
                session.trackCount() > track + 1) {
                EditController& c = session.controller();
                const auto from = p->regions[1].position, to = p->regions[2].end();
                c.select_range(track, from, to);
                model.invoke(QStringLiteral("edit.copy"), -1);
                const auto* next =
                    active_playlist(c.snapshot().edit->tracks[track + 1].get());
                const auto after = next && !next->regions.empty() ? next->regions.back().end() : to;
                model.invoke(QStringLiteral("track.select"), track + 1);
                c.set_insertion(after + (to - from) / 10);
                model.invoke(QStringLiteral("edit.paste"), -1);
                model.invoke(QStringLiteral("zoom.fit_session"), -1);
                QQuickItem* root = view.rootObject();
                auto* button = root->findChild<QQuickItem*>(QStringLiteral("editMenuButton"));
                auto* menu = root->findChild<QQuickItem*>(QStringLiteral("editMenu"));
                if (button != nullptr && menu != nullptr) {
                    const QPointF at = button->mapToScene(QPointF(0, button->height()));
                    menu->setProperty("x", at.x());
                    menu->setProperty("y", at.y() + 4);
                    menu->setProperty("visible", true);
                }
            }
        });
    }
    // After the demo has set its view, so what is shown is the file's.
    if (!a.opt.open_file.isEmpty()) {
        QTimer::singleShot(400, &app, [&session, &model, &a]() {
            if (!session.openSession(a.opt.open_file, true)) {
                std::fprintf(stderr, "sonora-ui: --open %s was refused\n",
                             qPrintable(a.opt.open_file));
            }
            model.invoke(QStringLiteral("zoom.fit_session"), -1);
        });
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
