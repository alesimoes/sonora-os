// SPDX-License-Identifier: GPL-3.0-or-later
//
// The three record-mode switches (ADR-0043, ADR-0129) in the real QML, on
// the offscreen platform and a fake engine: each one sends its command, the
// lit state follows the replica's fields and never the press, a refusal
// during a take is held where the operator looks, and the Pro Tools chords
// resolve to the same commands the buttons send.
#include <QCoreApplication>
#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QTest>
#include <algorithm>
#include <cstdio>
#include <string>

#include "key_router.h"
#include "peak_source.h"
#include "qt_keys.h"
#include "recording_adapter.h"
#include "register_types.h"
#include "session_model.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/keymap.h"
#include "timeline_document.h"
#include "transport_model.h"

using namespace sonora;
using namespace sonora::ui;
using model::Frames;
using model::SessionSettings;

namespace {

int failures = 0;

#define OK(cond)                                                                    \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

constexpr Frames kRate = 48000;
const char* const kTakeRefusal = "a take is recording; the record mode changes after Stop";

QQuickItem* named(QQuickItem* item, const QString& name) {
    if (item->objectName() == name) return item;
    for (QQuickItem* c : item->childItems()) {
        if (QQuickItem* f = named(c, name)) return f;
    }
    return nullptr;
}

// Pressed where it is drawn; a chip that just appeared is polished first.
void click(QQuickView& view, QQuickItem* item) {
    view.grabWindow();
    const QPointF c = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, c.toPoint());
}

bool has_command(const std::vector<std::string>& cmds, const std::string& name) {
    return std::find(cmds.begin(), cmds.end(), name) != cmds.end();
}

// The Transport bar and window over a fake engine, as the operator sees them.
struct Rig {
    test::RecordingAdapter engine{18, api::testing::FakeEngine::engine_commands()};
    KeyMap map;
    ModelDocument document;
    SyntheticPeakSource peaks{kRate, 120.0};
    SessionModel session{document, peaks, true};
    KeyRouter router;
    TransportModel transport{
        [this]() -> const SessionSettings& { return session.controller().snapshot().settings; },
        [this]() { return session.controller().selection().insertion; }};
    ShellModel model;
    QQuickView view;
    QQuickItem* root = nullptr;

    static KeyMap load(const char* file) {
        KeyMap m;
        std::string err;
        OK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/" + file, m, err));
        return m;
    }

    explicit Rig(model::Snapshot demo)
        : map(load("protools.json")),
          document(std::move(demo)),
          router(map, engine.info().implemented_commands,
                 [this](const std::string& c, const CommandContext& ctx) {
                     return engine.send(c, ctx);
                 }),
          model(engine, router) {
        session.installCommands(router.local());
        transport.installCommands(router.local());
        router.setPlayRange([this]() {
            const auto& sel = session.controller().selection();
            return play_range_argument(sel.start, sel.end);
        });
        router.prepare(qt_device_chords(true));
        link_transport(model, session, transport);
        view.resize(1280, 720);
        view.setInitialProperties({{QStringLiteral("shell"), QVariant::fromValue(&model)},
                                   {QStringLiteral("session"), QVariant::fromValue(&session)}});
        view.setSource(QUrl(QStringLiteral("qrc:/sonora/qml/Main.qml")));
        for (const auto& e : view.errors()) std::fprintf(stderr, "%s\n", qPrintable(e.toString()));
        OK(view.status() == QQuickView::Ready);
        if (view.status() != QQuickView::Ready) return;
        root = view.rootObject();
        root->setProperty("transport", QVariant::fromValue(&transport));
        view.show();
        model.tick();
    }

    QQuickItem* item(const char* name) const {
        return root ? named(root, QString::fromLatin1(name)) : nullptr;
    }
    bool active(const char* name) const {
        QQuickItem* i = item(name);
        return i != nullptr && i->isVisible() && i->property("active").toBool();
    }
    // The engine answers: the replica's settings change, as the engine's do.
    void engine_sets(void (*change)(model::Transport&)) {
        SessionSettings st = session.controller().snapshot().settings;
        change(st.transport);
        std::string why;
        OK(document.submit(model::SetSettings{st}, why));
        session.documentUpdated();
        model.tick();
    }
    void refuse_next(const char* why) {
        engine.next_result = DispatchResult::Failed;
        engine.next_message = why;
    }
    void accept_next() {
        engine.next_result = DispatchResult::Ok;
        engine.next_message.clear();
    }
    const std::string& last_command() const { return engine.sent.back().command; }
};

model::Snapshot quiet_session() {
    model::Snapshot demo = make_demo_session(4, kRate);
    demo.settings.transport.input_only = false;
    demo.settings.transport.quick_punch = false;
    demo.settings.transport.rolls_on = true;
    return demo;
}

// --- each switch: sends its command, lights from the replica ----------------

struct Switch {
    const char* item;
    const char* command;
    void (*flip)(model::Transport&);
};

void flip_input_only(model::Transport& t) { t.input_only = !t.input_only; }
void flip_pre_post(model::Transport& t) { t.rolls_on = !t.rolls_on; }
void flip_quick_punch(model::Transport& t) { t.quick_punch = !t.quick_punch; }

const Switch kSwitches[] = {
    {"transportInputOnly", "transport.toggle_input_only", flip_input_only},
    {"transportPrePostRoll", "transport.toggle_pre_post_roll", flip_pre_post},
    {"transportQuickPunch", "transport.toggle_quick_punch", flip_quick_punch},
};

void test_switches_send_and_reflect() {
    Rig r(quiet_session());
    if (r.root == nullptr) return;
    r.transport.setOpen(true);
    r.model.tick();
    for (const Switch& s : kSwitches) {
        QQuickItem* it = r.item(s.item);
        OK(it != nullptr);
        if (it == nullptr) continue;
        const bool lit = r.active(s.item);
        r.engine.sent.clear();
        click(r.view, it);
        r.model.tick();
        OK(r.engine.sent.size() == 1 && r.last_command() == s.command);
        // The press alone does not light it: only the engine's answer does.
        OK(r.active(s.item) == lit);
        r.engine_sets(s.flip);
        OK(r.active(s.item) == !lit);
        r.engine_sets(s.flip);   // back, for the next switch
        OK(r.active(s.item) == lit);
    }
}

// The bar's chips show only while their switch is on, and press the same command.
void test_bar_chips_follow_the_replica() {
    Rig r(quiet_session());
    if (r.root == nullptr) return;
    OK(!r.active("barQuickPunch") && !r.active("barInputOnly"));
    r.engine_sets([](model::Transport& t) {
        t.input_only = true;
        t.quick_punch = true;
    });
    OK(r.active("barQuickPunch") && r.active("barInputOnly"));
    r.engine.sent.clear();
    click(r.view, r.item("barQuickPunch"));
    OK(r.engine.sent.size() == 1 && r.last_command() == "transport.toggle_quick_punch");
    click(r.view, r.item("barInputOnly"));
    OK(r.engine.sent.size() == 2 && r.last_command() == "transport.toggle_input_only");
}

// --- a refusal during a take is shown, not hidden ----------------------------

void test_refusal_during_take_is_held() {
    Rig r(quiet_session());
    if (r.root == nullptr) return;
    r.transport.setOpen(true);
    r.engine.snapshot.state = TransportMode::Recording;
    for (int i = 0; i < 4; ++i) r.model.tick();
    const bool before = r.active("transportQuickPunch");

    r.refuse_next(kTakeRefusal);
    click(r.view, r.item("transportQuickPunch"));
    r.accept_next();
    r.model.tick();
    OK(r.last_command() == "transport.toggle_quick_punch");
    OK(r.active("transportQuickPunch") == before);   // the refused switch did not move
    // Held where the operator looks, not on the status line that clears.
    OK(r.model.recordRefusal() == QString::fromLatin1(kTakeRefusal));
    QQuickItem* held = r.item("recordRefusal");
    OK(held != nullptr && held->isVisible());

    // Stop is the way out of the take; it clears the notice.
    r.engine.snapshot.state = TransportMode::Stopped;
    for (int i = 0; i < 4; ++i) r.model.tick();
    r.model.invoke(QStringLiteral("transport.stop"), -1);
    r.model.tick();
    OK(r.model.recordRefusal().isEmpty());
}

// --- the Pro Tools chords resolve to the commands the buttons send ------------

void test_chords_resolve_to_commands() {
    KeyMap map;
    std::string err;
    OK(KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/protools.json", map, err));
    const auto cmds = api::testing::FakeEngine::engine_commands();
    struct Chord { const char* spec; const char* command; };
    const Chord chords[] = {
        {"alt+k", "transport.toggle_input_only"},
        {"ctrl+k", "transport.toggle_pre_post_roll"},
        {"ctrl+shift+p", "transport.toggle_quick_punch"},
    };
    for (const Chord& c : chords) {
        const Binding* b = map.resolve(std::string(c.spec));
        OK(b != nullptr);
        if (b == nullptr) continue;
        OK(b->command == c.command);
        OK(has_command(cmds, c.command));
    }
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    register_types();
    test_switches_send_and_reflect();
    test_bar_chips_follow_the_replica();
    test_refusal_during_take_is_held();
    test_chords_resolve_to_commands();
    if (failures != 0) {
        std::fprintf(stderr, "test_ui_transport_modes: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_ui_transport_modes: all passed\n");
    return 0;
}
