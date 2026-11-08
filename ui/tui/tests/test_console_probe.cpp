// SPDX-License-Identifier: GPL-3.0-or-later
//
// Console detection and the dumpkeys step (ADR-0070), with a fake detector
// and a fake runner: no VT and no dumpkeys binary are needed.
#include "sonora/console_probe.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "sonora/api/fake_engine.h"
#include "sonora/terminal_ui.h"

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                         #cond);                                         \
            return 1;                                                    \
        }                                                                \
    } while (0)

namespace {

using namespace sonora;
using namespace sonora::tui;

const char* kDump =
    "keymaps 0-2,4-6,8-9,12\n"
    "keycode   1 = Escape Escape Escape Escape Escape Escape Meta_Escape Meta_Escape Escape\n"
    "keycode  59 = F1 F11 Console_13 F1 F11 Console_13 Console_1 Console_11 Console_1\n"
    "keycode  60 = F2 F12 Console_14 F2 F12 Console_14 Console_2 Console_12 Console_2\n"
    "keycode  61 = F3 F13 Console_15 F3 F13 Console_15 Console_3 Console_13 Console_3\n"
    "keycode  62 = F4 F14 Console_16 F4 F14 Console_16 Console_4 Console_14 Console_4\n"
    "keycode  63 = F5 F15 Console_17 F5 F15 Console_17 Console_5 Console_15 Console_5\n"
    "keycode 105 = Left Left Left Left Left Left Decr_Console Decr_Console Left\n";

DumpkeysRunner returns(const char* text) {
    return [text](std::string& out) {
        out = text;
        return true;
    };
}
DumpkeysRunner fails() {
    return [](std::string&) { return false; };
}

std::string footer(const std::vector<KeyChord>& device) {
    sonora::api::testing::FakeEngine engine;
    std::string error;
    engine.connect(error);
    TerminalView view(engine, KeyMap::built_in_default());
    view.prepare(device);
    std::string all;
    for (const auto& l : view.help()) all += l + "\n";
    return all;
}

}  // namespace

int main() {
    const auto linux_probe = [](int) { return true; };
    CHECK(is_linux_console(0, "linux", linux_probe));
    CHECK(!is_linux_console(0, "xterm-256color", linux_probe));
    CHECK(!is_linux_console(0, nullptr, linux_probe));
    CHECK(!is_linux_console(0, "linux", [](int) { return false; }));  // ssh with TERM=linux
    CHECK(read_dumpkeys(fails()).empty());
    CHECK(read_dumpkeys(returns("abc")) == "abc");

    const auto xterm = KeyMap::terminal_chords();

    // Not a console: the set is untouched and the runner is never asked.
    bool asked = false;
    const auto spy = [&asked](std::string&) {
        asked = true;
        return false;
    };
    ConsoleChords plain = chords_for_console(xterm, false, spy);
    CHECK(plain.chords == xterm && plain.notice.empty() && !asked);

    // Console and a readable table: filtered, no notice.
    ConsoleChords good = chords_for_console(xterm, true, returns(kDump));
    CHECK(good.notice.empty());
    CHECK(good.chords.size() < xterm.size());

    // Console and the runner fails: builtin table, one notice, still filtered.
    ConsoleChords failed = chords_for_console(xterm, true, fails());
    CHECK(!failed.notice.empty());
    CHECK(failed.chords.size() < xterm.size());
    // Output that parses to nothing is the same failure.
    CHECK(!chords_for_console(xterm, true, returns("garbage\n")).notice.empty());

    // The footer names no chord the filter removed.
    std::vector<KeyChord> dropped;
    for (const KeyChord& c : xterm)
        if (std::find(good.chords.begin(), good.chords.end(), c) == good.chords.end())
            dropped.push_back(c);
    const std::string text = footer(good.chords);
    for (const KeyChord& c : dropped) {
        const std::string entry = KeyMap::chord_to_string(c) + "=";
        const bool listed = text.find("  " + entry) != std::string::npos ||
                            text.compare(0, entry.size(), entry) == 0;
        if (listed) std::fprintf(stderr, "footer lists %s\n", entry.c_str());
        CHECK(!listed);
    }
    CHECK(footer(xterm).size() >= text.size());
    return 0;
}
