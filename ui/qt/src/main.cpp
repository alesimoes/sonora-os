// SPDX-License-Identifier: GPL-3.0-or-later
//
// sonora-ui: the Qt 6 interface, a separate process from the engine
// (ADR-0007 ยง3). It reaches the engine through EngineAdapter on the control
// API (control_adapter.h): the real engine on its socket when one is
// listening (ui/client's SocketEngine), FakeEngine, the provider's fake with
// a synthetic signal on every input, when none is or with --fake-engine.
// The session it edits is the engine's document, replicated here
// (ReplicaDocument behind TimelineDocument, the model seam).
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImage>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
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
#include "scenes/screenshot_scene.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/client/socket_engine.h"
#include "sonora/platform/ipc.h"
#include "theme.h"
#include "capture.h"
#include "timeline_document.h"
#include "transport_model.h"

using namespace sonora;
using namespace sonora::ui;

namespace {

struct Options {
    QString keymap;
    QString keymap_dir;
    const ShotScene* scene = nullptr;
    int tracks = 18;
    int meter_hz = 0;  // 0: 60, or 30 on a software rasteriser
    bool kiosk = false;
    QSize capture_size;  // valid: screenshot mode at this many pixels
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
    QString open_file;
    // ADR-0015 ยง4: the most a Snapshot's MIDI chunk table, and a Load's
    // sequence files, may hold; this interface's own cap, not the engine's.
    uint64_t max_snapshot_mib = 512;
};

// The demo session's aux buses and sends (ADR-0022), on any engine: a
// reverb and a delay bus, most tracks sending to the reverb post-fader,
// one pre-fader send to the delay, and the delay feeding the reverb, so the
// Mix window shows each kind of send; and a Drums bus its first two tracks
// output to.
void add_demo_sends(model::Snapshot& s) {
    const auto apply = [&s](const model::Operation& op) {
        model::Snapshot after;
        std::string why;
        if (model::apply(s, op, after, why)) {
            s = std::move(after);
        } else {
            std::fprintf(stderr, "sonora-ui: the demo sends: %s\n", why.c_str());
        }
    };
    const model::Id verb = s.next_id;
    apply(model::AddBus{verb, "Verb", model::BusMix{0, -4.0f, 0.0f, false}});
    const model::Id delay = s.next_id;
    apply(model::AddBus{delay, "Delay", model::BusMix{0, -8.0f, 0.0f, false}});
    const auto send = [&](model::Id strip, uint8_t slot, model::Id bus, float db, bool pre) {
        model::AddSend op;
        op.strip = strip;
        op.send = s.next_id;
        op.slot = slot;
        op.bus = bus;
        op.mix.level_db = db;
        op.mix.pre = pre;
        apply(op);
    };
    const auto& tracks = s.edit->tracks;
    std::vector<model::Id> audio;
    for (const auto& t : tracks) {
        if (t->kind != model::TrackKind::Midi) audio.push_back(t->id);
    }
    for (std::size_t i = 0; i < audio.size() && i < 6; ++i) {
        send(audio[i], 0, verb, -12.0f + static_cast<float>(i), false);
    }
    if (audio.size() > 3) send(audio[3], 1, delay, -10.0f, true);
    send(delay, 0, verb, -6.0f, false);
    // A subgroup (ADR-0024): the first two tracks output to a Drums bus,
    // which plays on the Master.
    const model::Id drums = s.next_id;
    apply(model::AddBus{drums, "Drums", model::BusMix{}});
    for (std::size_t i = 0; i < audio.size() && i < 2; ++i) apply(model::SetOutput{audio[i], drums});
}

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
    const QCommandLineOption scene({QStringLiteral("scene")},
                                   QStringLiteral("With --demo, set up this screenshot scene "
                                                  "(ui/qt/src/scenes/)."),
                                   QStringLiteral("name"));
    const QCommandLineOption list_scenes(
        {QStringLiteral("list-scenes")},
        QStringLiteral("Print each scene's name and its other arguments, and exit."));
    p.addOptions({keymap_dir_opt, scene, list_scenes});
    const QCommandLineOption tracks({QStringLiteral("tracks")},
                                    QStringLiteral("Tracks the fake engine offers (default 18)."),
                                    QStringLiteral("n"));
    const QCommandLineOption hz({QStringLiteral("meter-hz")},
                                QStringLiteral("Meter rate; default 60, 30 on a software rasteriser."),
                                QStringLiteral("hz"));
    const QCommandLineOption kiosk({QStringLiteral("kiosk")},
                                   QStringLiteral("As under the kiosk compositor: no fixed size; the compositor maximizes the window."));
    const QCommandLineOption capture(
        {QStringLiteral("capture-size")},
        QStringLiteral("Screenshot mode: a window of WxH pixels (under QT_SCALE_FACTOR), "
                       "without the software-rendering banner."),
        QStringLiteral("WxH"));
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
    const QCommandLineOption open({QStringLiteral("open")},
                                  QStringLiteral("Open this session file once the window is up, "
                                                 "as File > Open does (with --demo, instead of "
                                                 "starting a take)."),
                                  QStringLiteral("file"));
    const QCommandLineOption max_snapshot(
        {QStringLiteral("max-snapshot-mib")},
        QStringLiteral("The most MIDI a session the engine sends, or one File > Open sends it, "
                       "may hold, in MiB (default 512)."),
        QStringLiteral("mib"));
    p.addOptions({keymap, tracks, hz, kiosk, capture, measure, label, shot, after, dies, nometers, demo,
                  window, sessions, socket, fake, open, max_snapshot});
    p.process(app);

    o.keymap = p.value(keymap);
    o.keymap_dir = p.value(keymap_dir_opt);
    if (p.isSet(tracks)) o.tracks = p.value(tracks).toInt();
    if (p.isSet(hz)) o.meter_hz = p.value(hz).toInt();
    o.kiosk = p.isSet(kiosk);
    if (p.isSet(capture) && !parse_capture_size(p.value(capture), o.capture_size)) {
        std::fprintf(stderr, "sonora-ui: --capture-size must be WxH, 640x360 or more\n");
        return false;
    }
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
    o.open_file = p.value(open);
    if (p.isSet(scene)) {
        o.scene = find_scene(p.value(scene));
        if (o.scene == nullptr) {
            std::fprintf(stderr, "sonora-ui: no scene %s; --list-scenes lists them\n",
                         qPrintable(p.value(scene)));
            return false;
        }
        if (!o.demo) {
            std::fprintf(stderr, "sonora-ui: --scene needs --demo\n");
            return false;
        }
        if (!p.isSet(window)) o.window = o.scene->window;
    }
    if (p.isSet(max_snapshot)) {
        bool ok = false;
        o.max_snapshot_mib = p.value(max_snapshot).toULongLong(&ok);
        if (!ok || o.max_snapshot_mib == 0) {
            std::fprintf(stderr, "sonora-ui: --max-snapshot-mib must be a positive number\n");
            return false;
        }
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

// Exit status for "this display cannot run the interface". The unit lists it
// in RestartPreventExitStatus=, so the failure goes to OnFailure= (tty2, the
// terminal UI) instead of a restart loop. Keep in step with sonora-ui.service.
constexpr int kExitUnfitDisplay = 3;

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
    // Under --kiosk the compositor's window rule maximizes us (ADR-0010 ง2);
    // asking for full screen as well would fight it.
    if (!opt.kiosk) view.resize(1280, 720);
    view.show();
    app.exec();
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    // Before the application, which would want a display the screenshot
    // script's caller does not have.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list-scenes") == 0) {
            for (const ShotScene& s : registered_scenes()) {
                std::printf("%s%s%s\n", qPrintable(s.name), s.args.isEmpty() ? "" : " ",
                            qPrintable(s.args));
            }
            return 0;
        }
    }
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("sonora-ui"));
    // The Wayland app_id the compositor's window rule matches (ADR-0010 ง2).
    QGuiApplication::setDesktopFileName(QStringLiteral("sonora-ui"));
    QElapsedTimer since_start;
    since_start.start();

    App a;
    if (!parse(app, a.opt)) return 2;
    // The kiosk has no fallback path inside the UI: a Qt that came up on xcb
    // or offscreen means the compositor is not there, and retrying cannot
    // help. The unit goes to the terminal UI instead (ADR-0010 ง4).
    if (a.opt.kiosk && app.platformName() != QLatin1String("wayland")) {
        std::fprintf(stderr, "sonora-ui: unfit display: platform is %s, not wayland
",
                     qPrintable(app.platformName()));
        return kExitUnfitDisplay;
    }

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
        // A loopback cable, out_2 into the last input, so the Latency window
        // measures something here as on a patched interface.
        fake_config.latency_cable = {2, fake_config.tracks, 287};
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
    document.set_max_snapshot_bytes(a.opt.max_snapshot_mib << 20);
    // On the fake engine the demo has inserts too, from its scripted catalog;
    // a real engine's catalog is its own, so there the demo has none.
    sonora::model::Id demo_crash = 0;
    if (a.opt.demo) {
        std::string why;
        sonora::model::Snapshot demo = make_demo_session(engine.info().track_count, rate);
        if (a.opt.scene != nullptr && a.opt.scene->tempo_map) add_demo_tempo_map(demo);
        add_demo_sends(demo);
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
        [&engine](uint32_t track, sonora::api::ParamId param, float value,
                  sonora::api::Gesture gesture) {
            engine.send_track_param(track, param, value, gesture);
        },
        [&engine]() { return engine.info().track_count; }, fake == nullptr);
    InsertModel inserts(document, session);
    SendModel sends(document, session);
    sends.setLiveSender([&engine](bool aux, model::Id id, float linear) {
        if (aux) engine.send_aux_gain(id, linear);
        else engine.send_send_level(id, linear);
    });
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
    // app.quit is the interface's own under ADR-0007 ยง3/ยง5: it closes this
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
    // Pro Tools plays a timeline selection, with pre- and post-roll.
    router.setPlayRange([&session]() {
        const auto& sel = session.controller().selection();
        return play_range_argument(sel.start, sel.end);
    });
    const std::vector<std::string> notices =
        router.prepare(qt_device_chords(qt_scan_codes_are_xkb(QGuiApplication::platformName())));

    register_types();
    ShellModel model(engine, router);
    a.model = &model;
    QObject::connect(&session, &SessionModel::openNotice, &model, &ShellModel::setOpenNotice);
    session.setEngineTracks(
        [&model]() {
            const QList<bool> armed = model.armed();
            return std::vector<bool>(armed.begin(), armed.end());
        },
        [&model]() { return model.engineAlive() ? model.trackCount() : 0; });
    QObject::connect(&session, &SessionModel::heldNotice, &model, &ShellModel::addNotice);
    QObject::connect(&model, &ShellModel::sessionFolder, &session, &SessionModel::setEngineFolder);
    link_transport(model, session, transport);
    link_marker_locate(model, session);
    ShellModel::PluginSinks plugin_sinks;
    plugin_sinks.catalog = [&inserts](const api::PluginCatalog& c) { inserts.onCatalog(c); };
    plugin_sinks.fault = [&inserts](uint64_t insert, const std::string& text) {
        inserts.onFault(insert, text);
    };
    plugin_sinks.frame = [&inserts](const EngineFrame& f, bool alive) { inserts.onFrame(f, alive); };
    model.setPluginSinks(std::move(plugin_sinks));
    session.setWindow(a.opt.window);
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
                               {QStringLiteral("sends"), QVariant::fromValue(&sends)},
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
    // One line, for the boot-time check (ADR-0010 migration, stage 1).
    QObject::connect(&view, &QQuickWindow::frameSwapped, &view,
                     [&since_start]() {
                         std::fprintf(stderr, "sonora-ui: first frame after %lld ms
",
                                      static_cast<long long>(since_start.elapsed()));
                         std::fflush(stderr);
                     },
                     Qt::SingleShotConnection);
    model.start(a.opt.meter_hz > 0 ? a.opt.meter_hz : 60);

    if (a.opt.capture_size.isValid()) {
        const bool keep = a.opt.scene != nullptr && a.opt.scene->software_banner;
        model.setCaptureMode(!keep);
        view.resize(capture_window_size(a.opt.capture_size,
                                        qEnvironmentVariable("QT_SCALE_FACTOR", "1").toDouble()));
        view.show();
    } else {
        // Under --kiosk no full-screen request: the compositor's window rule
        // for app_id sonora-ui maximizes us (ADR-0010 ง2).
        if (!a.opt.kiosk) view.resize(1280, 720);
        view.show();
    }

    if (a.opt.demo) {
        for (int t = 0; t < 8 && t < a.opt.tracks; ++t) model.invoke(QStringLiteral("track.toggle_arm"), t);
        // Not in a scene that says so (ShotScene::take), nor under --open: a
        // session does not open over a take.
        if ((a.opt.scene == nullptr || a.opt.scene->take) && a.opt.open_file.isEmpty()) {
            model.invoke(QStringLiteral("transport.record"), -1);
        }
        // Once the window has its size: the whole session in view, and the
        // snare's second region selected, so a screenshot shows what a
        // selected region looks like.
        QTimer::singleShot(200, &app, [&session, &model, &inserts, &sends, &transport, &a, &view,
                                       fake, demo_crash]() {
            // The fake engine has the demo session by now; a crash on Tom 1's
            // compressor shows what a fault looks like.
            if (fake != nullptr && demo_crash != 0) {
                fake->script_insert_fault(
                    demo_crash, "its host crashed (SIGSEGV); the chain plays dry until Retry");
            }
            ShotContext scene{session, model, inserts, sends, transport, view};
            if (a.opt.scene != nullptr && a.opt.scene->setup) a.opt.scene->setup(scene);
            model.invoke(QStringLiteral("zoom.fit_session"), -1);
            const int track = std::min(1, session.trackCount() - 1);
            const auto* p =
                active_playlist(session.controller().snapshot().edit->tracks[track].get());
            if (p && p->regions.size() > 1) session.controller().select_region(p->regions[1].id);
            model.invoke(QStringLiteral("track.select"), track);
            scene.track = track;
            scene.playlist = p;
            if (a.opt.scene != nullptr && a.opt.scene->apply) a.opt.scene->apply(scene);
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
