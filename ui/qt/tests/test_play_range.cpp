// SPDX-License-Identifier: GPL-3.0-or-later
//
// Play of the Edit selection (Pro Tools' play of a timeline selection, with
// pre- and post-roll):
//
//   - transport.play and transport.toggle_play carry the selection as their
//     "A-B" argument to an engine whose Hello announces
//     api::kCapabilityPlayRange, through FakeEngine's wire, and the fake
//     accepts the range;
//   - an engine that does not announce it gets no argument, as before: it
//     would ignore one and play on past the selection;
//   - no selection, or an empty one, sends no argument;
//   - an argument the caller gave is left alone;
//   - a reconnect to another build follows its Hello's capabilities;
//   - transport.toggle_loop (Pro Tools' Ctrl+Shift+L) and
//     transport.toggle_loop_record (Pro Tools' Option+L) carry the selection
//     too, with no capability asked, and the fake makes it the loop range as
//     it turns the mode on;
//   - Logic's C is transport.toggle_cycle, both modes together, which
//     carries the selection too: it becomes the cycle area when none is set
//     and is ignored when one is; Cmd+U, transport.cycle_selection, always
//     makes it the area and turns Cycle on;
//   - transport.punch_selection carries the selection, which the engine
//     makes the punch range.
#include <QGuiApplication>
#include <cstdio>
#include <string>
#include <vector>

#include "control_adapter.h"
#include "key_router.h"
#include "qt_keys.h"
#include "recording_adapter.h"
#include "shell_model.h"
#include "sonora/api/fake_engine.h"
#include "sonora/model/model.h"

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

KeyMap load(const std::string& file) {
    KeyMap m;
    std::string err;
    if (!KeyMap::load_file(std::string(SONORA_KEYMAP_DIR) + "/" + file, m, err)) {
        std::fprintf(stderr, "FAIL %s: %s\n", file.c_str(), err.c_str());
    }
    return m;
}

KeyChord chord(const std::string& spec) {
    KeyChord c;
    std::string err;
    KeyMap::parse_chord(spec, c, err);
    return c;
}

int test_argument() {
    CHECK(play_range_argument(2000, 3000) == "2000-3000");
    CHECK(play_range_argument(0, 1) == "0-1");
    CHECK(play_range_argument(3000, 3000).empty());
    CHECK(play_range_argument(3000, 2000).empty());
    CHECK(play_range_argument(-1, 2000).empty());
    return 0;
}

// Over FakeEngine's wire, announcing the capability or not.
int on_fake(bool announces) {
    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 4;
    if (!announces) cfg.capabilities.clear();
    api::testing::FakeEngine fake(cfg);
    ControlAdapter engine(fake);
    std::string err;
    CHECK(engine.connect(err));
    std::vector<std::string> args;
    KeyRouter router(load("protools.json"), engine.info().implemented_commands,
                     [&engine, &args](const std::string& c, const CommandContext& ctx) {
                         args.push_back(ctx.argument);
                         return engine.send(c, ctx);
                     });
    router.setEngineCapabilities(engine.info().capabilities);
    std::string range = "2000-3000";
    router.setPlayRange([&range]() { return range; });
    router.prepare(qt_device_chords(true));

    // Space plays the selection; Space again stops, and the fake ignores
    // the argument while rolling, as the engine does.
    const RouteResult r = router.route(chord("space"));
    CHECK(r.kind == RouteResult::Kind::Sent);
    CHECK(r.ctx.argument == (announces ? "2000-3000" : ""));
    CHECK(router.route(chord("space")).kind == RouteResult::Kind::Sent);
    // transport.play from a button, no selection: from the playhead.
    range.clear();
    CommandContext none;
    CHECK(router.invoke("transport.play", none).kind == RouteResult::Kind::Sent);
    // A caller's own argument is not replaced.
    range = "2000-3000";
    CommandContext given;
    given.argument = "10-20";
    CHECK(router.invoke("transport.play", given).ctx.argument == "10-20");
    // Neither is any other command's.
    CHECK(router.invoke("transport.stop", none).ctx.argument.empty());

    CHECK(args.size() == 5);
    CHECK(args[0] == (announces ? "2000-3000" : ""));
    CHECK(args[1] == (announces ? "2000-3000" : ""));
    CHECK(args[2].empty());
    CHECK(args[3] == "10-20");
    CHECK(args[4].empty());
    std::vector<CommandOutcome> replies;
    engine.poll(replies);
    CHECK(replies.size() == 5);
    for (const auto& o : replies) CHECK(o.result == DispatchResult::Ok);
    return 0;
}

int test_fake() {
    if (on_fake(true) != 0) return 1;
    return on_fake(false);
}

int test_toggle_loop(const char* profile, const char* key) {
    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 4;
    cfg.capabilities.clear();
    api::testing::FakeEngine fake(cfg);
    ControlAdapter engine(fake);
    std::string err;
    CHECK(engine.connect(err));
    std::vector<std::string> args;
    KeyRouter router(load(profile), engine.info().implemented_commands,
                     [&engine, &args](const std::string& c, const CommandContext& ctx) {
                         args.push_back(ctx.argument);
                         return engine.send(c, ctx);
                     });
    std::string range = "2000-3000";
    router.setPlayRange([&range]() { return range; });
    router.prepare(qt_device_chords(true));
    CHECK(!fake.document().settings.transport.loop_playback);

    RouteResult r = router.route(chord(key));
    CHECK(r.kind == RouteResult::Kind::Sent && r.command == "transport.toggle_loop");
    CHECK(r.ctx.argument == "2000-3000");
    std::vector<CommandOutcome> replies;
    engine.poll(replies);
    CHECK(replies.size() == 1 && replies[0].result == DispatchResult::Ok);
    const auto& tr = fake.document().settings.transport;
    CHECK(tr.loop_playback && tr.loop[0] == 2000 && tr.loop[1] == 3000);

    // Off with no selection: the range stays.
    range.clear();
    CHECK(router.route(chord(key)).ctx.argument.empty());
    replies.clear();
    engine.poll(replies);
    CHECK(replies.size() == 1 && replies[0].result == DispatchResult::Ok);
    CHECK(!fake.document().settings.transport.loop_playback);
    CHECK(fake.document().settings.transport.loop[1] == 3000);
    CHECK(args.size() == 2);
    return 0;
}

// Pro Tools' Loop Record: Option+L, a record mode apart from Loop Playback,
// over the selection as Loop Playback's.
int test_toggle_loop_record() {
    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 4;
    api::testing::FakeEngine fake(cfg);
    ControlAdapter engine(fake);
    std::string err;
    CHECK(engine.connect(err));
    KeyRouter router(load("protools.json"), engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    router.setPlayRange([]() { return std::string("2000-3000"); });
    router.prepare(qt_device_chords(true));
    RouteResult r = router.route(chord("alt+l"));
    CHECK(r.kind == RouteResult::Kind::Sent && r.command == "transport.toggle_loop_record");
    CHECK(r.ctx.argument == "2000-3000");
    std::vector<CommandOutcome> replies;
    engine.poll(replies);
    CHECK(replies.size() == 1 && replies[0].result == DispatchResult::Ok);
    const auto& tr = fake.document().settings.transport;
    CHECK(tr.loop_record && !tr.loop_playback && tr.loop[0] == 2000 && tr.loop[1] == 3000);
    return 0;
}

// Logic's C keeps a cycle area already set: the selection goes with it, and
// the engine leaves the area alone. The area is set as Logic sets it, by
// Cmd+U (Set Locators by Selection and Enable Cycle), with no Pro Tools
// command on the way.
int test_cycle_keeps_the_range() {
    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 4;
    api::testing::FakeEngine fake(cfg);
    ControlAdapter engine(fake);
    std::string err;
    CHECK(engine.connect(err));
    KeyRouter router(load("logic.json"), engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    std::string range = "9000-12000";
    router.setPlayRange([&range]() { return range; });
    router.prepare(qt_device_chords(true));
    const auto& tr = fake.document().settings.transport;
    std::vector<CommandOutcome> replies;

    RouteResult r = router.route(chord("cmd+u"));
    CHECK(r.kind == RouteResult::Kind::Sent && r.command == "transport.cycle_selection");
    CHECK(r.ctx.argument == "9000-12000");
    engine.poll(replies);
    CHECK(replies.size() == 1 && replies[0].result == DispatchResult::Ok);
    CHECK(tr.loop_playback && tr.loop_record && tr.loop[0] == 9000 && tr.loop[1] == 12000);
    // Cmd+U again with Cycle on moves the area and leaves Cycle on.
    range = "6000-8000";
    router.route(chord("cmd+u"));
    replies.clear();
    engine.poll(replies);
    CHECK(replies.size() == 1 && replies[0].result == DispatchResult::Ok);
    CHECK(tr.loop_playback && tr.loop_record && tr.loop[0] == 6000 && tr.loop[1] == 8000);

    // C off, then on over another selection: the area stays.
    range = "2000-3000";
    router.route(chord("c"));
    replies.clear();
    engine.poll(replies);
    CHECK(replies.size() == 1 && replies[0].result == DispatchResult::Ok);
    CHECK(!tr.loop_playback && !tr.loop_record);
    r = router.route(chord("c"));
    CHECK(r.kind == RouteResult::Kind::Sent && r.command == "transport.toggle_cycle");
    CHECK(r.ctx.argument == "2000-3000");
    replies.clear();
    engine.poll(replies);
    CHECK(replies.size() == 1 && replies[0].result == DispatchResult::Ok);
    CHECK(tr.loop_playback && tr.loop_record);
    CHECK(tr.loop[0] == 6000 && tr.loop[1] == 8000);
    // The Transport window shows one Cycle button for this profile.
    ShellModel shell(engine, router);
    CHECK(shell.cycleMode());
    return 0;
}

// #149 review: in a new session the Logic profile has no cycle area, and C
// once turned Cycle on over [0, 0], so R recorded a straight take. Select,
// press C: the selection is the area. With no selection either, C is refused
// and nothing turns on; Cmd+U with no selection is refused too.
int test_logic_cycle_from_no_range() {
    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 4;
    api::testing::FakeEngine fake(cfg);
    ControlAdapter engine(fake);
    std::string err;
    CHECK(engine.connect(err));
    KeyRouter router(load("logic.json"), engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    std::string range;
    router.setPlayRange([&range]() { return range; });
    router.prepare(qt_device_chords(true));
    const auto& tr = fake.document().settings.transport;
    CHECK(tr.loop[0] == 0 && tr.loop[1] == 0);
    std::vector<CommandOutcome> replies;

    router.route(chord("c"));
    engine.poll(replies);
    CHECK(replies.size() == 1 && replies[0].result != DispatchResult::Ok);
    CHECK(!tr.loop_playback && !tr.loop_record);
    router.route(chord("cmd+u"));
    replies.clear();
    engine.poll(replies);
    CHECK(replies.size() == 1 && replies[0].result != DispatchResult::Ok);
    CHECK(!tr.loop_playback && !tr.loop_record);

    range = "48000-96000";
    router.route(chord("c"));
    replies.clear();
    engine.poll(replies);
    CHECK(replies.size() == 1 && replies[0].result == DispatchResult::Ok);
    CHECK(tr.loop_playback && tr.loop_record);
    CHECK(tr.loop[0] == 48000 && tr.loop[1] == 96000);
    return 0;
}

// ShellModel hands each new Hello's capabilities to the router.
int test_follows_reconnect() {
    test::RecordingAdapter engine(4, api::testing::FakeEngine::engine_commands());
    KeyRouter router(load("protools.json"), engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    router.setPlayRange([]() { return std::string("2000-3000"); });
    router.prepare(qt_device_chords(true));
    ShellModel shell(engine, router);
    // Loop and Loop Rec, two buttons, in Pro Tools' Transport window.
    CHECK(!shell.cycleMode());
    CHECK(router.engineCapabilities().empty());
    CHECK(router.route(chord("space")).ctx.argument.empty());

    engine.set_capabilities({api::kCapabilityPlayRange});
    shell.tick();
    CHECK(router.route(chord("space")).ctx.argument == "2000-3000");

    engine.set_capabilities({});
    shell.tick();
    CHECK(router.route(chord("space")).ctx.argument.empty());
    return 0;
}

}  // namespace

// #156 review N1: the punch range was set in this window and sent as a copy
// of its settings, so an edit through a stale overlay could move the range
// a take was using. transport.punch_selection now goes to the engine with
// the selection, and the engine sets the one field.
int test_punch_selection_goes_to_the_engine() {
    api::testing::FakeEngine::Config cfg;
    cfg.tracks = 4;
    api::testing::FakeEngine fake(cfg);
    ControlAdapter engine(fake);
    std::string err;
    CHECK(engine.connect(err));
    KeyRouter router(load("protools.json"), engine.info().implemented_commands,
                     [&engine](const std::string& c, const CommandContext& ctx) {
                         return engine.send(c, ctx);
                     });
    router.setPlayRange([]() { return std::string("48000-96000"); });
    router.prepare(qt_device_chords(true));
    RouteResult r = router.invoke("transport.punch_selection", CommandContext{});
    CHECK(r.kind == RouteResult::Kind::Sent);
    CHECK(r.ctx.argument == "48000-96000");
    std::vector<CommandOutcome> replies;
    engine.poll(replies);
    CHECK(replies.size() == 1 && replies[0].result == DispatchResult::Ok);
    const auto& tr = fake.document().settings.transport;
    CHECK(tr.punch[0] == 48000 && tr.punch[1] == 96000);
    r = router.invoke("transport.clear_punch", CommandContext{});
    CHECK(r.kind == RouteResult::Kind::Sent && r.ctx.argument.empty());
    replies.clear();
    engine.poll(replies);
    CHECK(replies.size() == 1 && replies[0].result == DispatchResult::Ok);
    CHECK(tr.punch[0] == 0 && tr.punch[1] == 0);
    return 0;
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (test_argument() != 0) return 1;
    if (test_fake() != 0) return 1;
    if (test_follows_reconnect() != 0) return 1;
    if (test_toggle_loop("protools.json", "ctrl+shift+l") != 0) return 1;
    if (test_toggle_loop("protools.json", "numpad_4") != 0) return 1;
    if (test_toggle_loop_record() != 0) return 1;
    if (test_logic_cycle_from_no_range() != 0) return 1;
    if (test_cycle_keeps_the_range() != 0) return 1;
    if (test_punch_selection_goes_to_the_engine() != 0) return 1;
    std::printf("ui play range: ok\n");
    return 0;
}
