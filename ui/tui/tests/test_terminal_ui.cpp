// SPDX-License-Identifier: GPL-3.0-or-later
//
// The terminal interface as the control API's first client (ADR-0007 M4),
// against FakeEngine: keys reach the engine as commands, the engine's
// answers and refusals reach the status line, its state reaches the
// screen, and a dead or failed engine is said, not hidden.
#include "sonora/terminal_ui.h"

#include <cstdio>
#include <string>

#include "sonora/api/fake_engine.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using sonora::KeyChord;
using sonora::KeyMap;
using sonora::api::testing::FakeEngine;
using sonora::tui::TerminalView;

bool mentions(const std::string& haystack, const char* needle) {
    if (haystack.find(needle) != std::string::npos) return true;
    std::fprintf(stderr, "  expected \"%s\" in:\n%s\n", needle, haystack.c_str());
    return false;
}

KeyChord key(const char* spec) {
    KeyChord c;
    std::string why;
    KeyMap::parse_chord(spec, c, why);
    return c;
}

struct Rig {
    uint64_t now = 1'000'000'000;
    FakeEngine engine;
    TerminalView view;

    static FakeEngine::Config config(uint64_t* now) {
        FakeEngine::Config cfg;
        cfg.tracks = 4;
        cfg.clock = [now]() { return *now; };
        return cfg;
    }
    Rig() : engine(config(&now)), view(engine, KeyMap::built_in_default()) {}

    bool connect() {
        std::string error;
        if (!engine.connect(error)) return false;
        view.prepare(KeyMap::terminal_chords());
        return true;
    }
    // One frame of the loop: 50 ms pass, the engine is polled, the screen drawn.
    std::string frame() {
        now += 50'000'000;
        view.tick(now);
        return view.render(now);
    }
};

int test_keys_drive_the_engine() {
    Rig r;
    CHECK(r.connect());
    // The default profile's footer, built from what both sides implement.
    std::string help;
    for (const auto& l : r.view.help()) help += l + "\n";
    CHECK(mentions(help, "space=toggle_record"));
    CHECK(mentions(help, "1=toggle_arm1"));
    CHECK(mentions(help, "q=quit"));

    std::string screen = r.frame();
    CHECK(mentions(screen, "STOP"));
    CHECK(mentions(screen, "Audio 1"));
    CHECK(mentions(screen, "48000 Hz / 128 frames"));

    // Nothing armed: the engine refuses, and the reason is on screen.
    r.view.on_chord(key("space"));
    screen = r.frame();
    CHECK(mentions(screen, "transport.toggle_record: no track is armed"));

    r.view.on_chord(key("1"));
    screen = r.frame();
    CHECK(r.engine.state().track(0).armed());
    CHECK(mentions(screen, "\033[31m[R]\033[0m"));
    CHECK(r.view.status().empty());

    r.view.on_chord(key("space"));
    screen = r.frame();
    CHECK(mentions(screen, "REC "));
    CHECK(mentions(screen, "take 1"));

    // Arming is fixed during a take, and the engine says so.
    r.view.on_chord(key("a"));
    screen = r.frame();
    CHECK(mentions(screen, "track.arm_all: arming cannot change during a take"));

    r.view.on_chord(key("space"));
    screen = r.frame();
    CHECK(mentions(screen, "STOP"));
    CHECK(mentions(screen, "take 2"));

    // An unbound key and an unknown sequence are said, not swallowed.
    r.view.on_chord(key("z"));
    CHECK(mentions(r.frame(), "z is not bound"));
    r.view.on_unrecognised();
    CHECK(mentions(r.frame(), "that key means nothing here"));

    // app.quit is the interface's own; it never reaches the engine.
    CHECK(!r.view.quit());
    r.view.on_chord(key("q"));
    CHECK(r.view.quit());
    return 0;
}

int test_engine_events_and_death() {
    Rig r;
    CHECK(r.connect());
    sonora::api::EngineEvent e;
    e.kind = sonora::api::EventKind::Notice;
    e.text = "recording from Built-in Audio Pro (onboard): no USB interface found";
    r.engine.post_event(e);
    e.kind = sonora::api::EventKind::InputsMissing;
    e.text = "nothing is connected to the inputs";
    r.engine.post_event(e);
    std::string screen = r.frame();
    CHECK(mentions(screen, "(onboard): no USB interface found"));
    CHECK(mentions(screen, "no inputs: nothing is connected"));
    e.text.clear();   // connected again
    r.engine.post_event(e);
    screen = r.frame();
    CHECK(screen.find("no inputs:") == std::string::npos);
    CHECK(mentions(screen, "(onboard)"));   // a notice stays for the session

    // Meters show the engine's peaks while it lives...
    r.engine.script_peak(0, 0.5f);
    screen = r.frame();
    CHECK(mentions(screen, "-6.0 dB"));
    // ...and "no data" once its heartbeat stops, never a frozen bar.
    r.engine.die();
    for (int i = 0; i < 10; ++i) screen = r.frame();
    CHECK(mentions(screen, "no data"));
    CHECK(mentions(screen, "the engine is not answering"));
    CHECK(screen.find("-6.0 dB") == std::string::npos);
    return 0;
}

int test_backend_failure_ends_the_interface() {
    Rig r;
    CHECK(r.connect());
    sonora::api::EngineEvent e;
    e.kind = sonora::api::EventKind::Fatal;
    e.text = "the audio server went away";
    r.engine.post_event(e);
    CHECK(!r.view.engine_failed());
    const std::string screen = r.frame();
    CHECK(r.view.engine_failed());
    CHECK(r.view.fatal_reason() == "the audio server went away");
    CHECK(mentions(screen, "the audio server went away"));
    return 0;
}

}  // namespace

int main() {
    int failures = 0;
    failures += test_keys_drive_the_engine();
    failures += test_engine_events_and_death();
    failures += test_backend_failure_ends_the_interface();
    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    std::printf("terminal ui: all tests passed\n");
    return 0;
}
