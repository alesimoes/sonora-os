// ADR-0070: the dumpkeys parser, the built-in table and the console filter.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "sonora/console_keys.h"

using namespace sonora;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d %s\n", \
    __FILE__, __LINE__, #c); ++failures; } } while (0)

// An excerpt of default-US `dumpkeys` (columns: plain, shift, altgr, ctrl,
// shift+ctrl, altgr+ctrl, alt, shift+alt, ctrl+alt).
static const char* kDump =
    "keymaps 0-2,4-6,8-9,12\n"
    "# a comment\n"
    "string F1 = \"\\033[[A\"\n"
    "keycode   1 = Escape Escape Escape Escape Escape Escape Meta_Escape Meta_Escape Escape\n"
    "keycode  30 = +a +A +a Control_a Control_a Control_a Meta_a Meta_A Meta_Control_a\n"
    "keycode  59 = F1 F11 Console_13 F1 F11 Console_13 Console_1 Console_11 Console_1\n"
    "keycode  63 = F5 F15 Console_17 F5 F15 Console_17 Console_5 Console_15 Console_5\n"
    "keycode 103 = Up Up Up Up Up Up Up Up Up\n"
    "keycode 104 = Prior Scroll_Backward Prior Prior Prior Prior Prior Prior Prior\n"
    "keycode 105 = Left Left Left Left Left Left Decr_Console Decr_Console Left\n"
    "keycode 106 = Right Right Right Right Right Right Incr_Console Incr_Console Right\n"
    "keycode 109 = Next Scroll_Forward Next Next Next Next Next Next Next\n"
    "bogus line\n";

static KeyChord ch(const std::string& k, unsigned m = kModNone) { return KeyChord{k, m}; }

static bool has(const std::vector<KeyChord>& v, const KeyChord& c) {
    for (const auto& x : v) if (x == c) return true;
    return false;
}

static std::vector<KeyChord> candidates() {
    std::vector<KeyChord> v;
    for (int n = 1; n <= 12; ++n) {
        const std::string f = "f" + std::to_string(n);
        v.push_back(ch(f));
        v.push_back(ch(f, kModAlt));
    }
    for (const char* k : {"left", "right", "up", "down", "pageup", "pagedown"}) {
        v.push_back(ch(k));
        v.push_back(ch(k, kModShift));
        v.push_back(ch(k, kModCtrl));
        v.push_back(ch(k, kModAlt));
    }
    v.push_back(ch("a", kModAlt));
    v.push_back(ch("a", kModCtrl));
    v.push_back(ch("left", kModCmd));
    return v;
}

static void test_parser() {
    std::vector<std::string> problems;
    ConsoleKeymap km = parse_console_keymap(kDump, &problems);
    CHECK(km.rows.size() == 9);
    const std::vector<int> masks = {0, 1, 2, 4, 5, 6, 8, 9, 12};
    CHECK(km.declared_masks == masks);
    CHECK(km.rows[1].keycode == 30);
    CHECK(km.rows[1].columns[0] == "a");  // '+' prefix dropped
    CHECK(km.rows[2].columns[6] == "Console_1");
    // Only "bogus line": string lines and comments are known and skipped.
    CHECK(problems.size() == 1);
    CHECK(problems.size() == 1 && problems[0].find("bogus") != std::string::npos);

    CHECK(parse_console_keymap("", nullptr).empty());
    std::vector<std::string> p2;
    CHECK(parse_console_keymap("garbage\nmore\n", &p2).empty());
    CHECK(p2.size() == 2);
    ConsoleKeymap none = parse_console_keymap("keycode 1 = a b\n", nullptr);
    CHECK(none.declared_masks.empty() && none.rows.size() == 1);
    p2.clear();
    parse_console_keymap("keymaps 0-99\nkeycode 1 = a\n", &p2);
    CHECK(p2.size() == 1);
}

static void test_filter_default() {
    ConsoleKeymap km = parse_console_keymap(kDump, nullptr);
    const auto in = candidates();
    auto out = filter_console_chords(in, km);
    CHECK(!has(out, ch("f1", kModAlt)));
    CHECK(!has(out, ch("f5", kModAlt)));
    CHECK(!has(out, ch("left", kModAlt)));
    CHECK(!has(out, ch("right", kModAlt)));
    CHECK(!has(out, ch("pageup", kModShift)));
    CHECK(!has(out, ch("pagedown", kModShift)));
    CHECK(!has(out, ch("up", kModCtrl)));
    CHECK(!has(out, ch("left", kModShift)));
    CHECK(!has(out, ch("left", kModCmd)));
    CHECK(has(out, ch("left")));
    CHECK(has(out, ch("up")));
    CHECK(has(out, ch("down", kModShift)));  // no row: cannot be judged, kept
    CHECK(has(out, ch("up", kModAlt)));      // Alt column is a plain string
    CHECK(has(out, ch("a", kModAlt)));
    CHECK(has(out, ch("a", kModCtrl)));
    CHECK(has(out, ch("f1")));
    size_t last = 0;
    bool ordered = true;
    for (const auto& c : out) {
        size_t i = 0;
        while (i < in.size() && !(in[i] == c)) ++i;
        if (i < last) ordered = false;
        last = i;
    }
    CHECK(ordered);
}

static void test_remapped() {
    std::string text = kDump;
    const std::string from = "Console_5 Console_15 Console_5";
    text.replace(text.find(from), from.size(), "Meta_F5 Console_15 Console_5");
    ConsoleKeymap km = parse_console_keymap(text, nullptr);
    auto out = filter_console_chords(candidates(), km);
    CHECK(has(out, ch("f5", kModAlt)));   // Alt column is now a string
    CHECK(!has(out, ch("f1", kModAlt)));  // untouched row still switches VT
}

static void test_builtin() {
    ConsoleKeymap km = builtin_console_keymap();
    CHECK(!km.empty());
    auto out = filter_console_chords(candidates(), km);
    for (int n = 1; n <= 12; ++n) {
        const std::string f = "f" + std::to_string(n);
        CHECK(!has(out, ch(f, kModAlt)));
        CHECK(has(out, ch(f)));
    }
    CHECK(!has(out, ch("left", kModAlt)));
    CHECK(!has(out, ch("pageup", kModShift)));
    CHECK(!has(out, ch("left", kModShift)));
    CHECK(!has(out, ch("up", kModCtrl)));
    CHECK(has(out, ch("up", kModAlt)));
    CHECK(has(out, ch("left")));
}

// A profile whose only record key is Alt+F1: fine on an xterm, dead on a
// console, so ensure_essentials() must now report it and repair it.
static void test_essentials_on_console() {
    const auto all = [](const std::string&) { return true; };
    const char* profile = R"({ "name": "x", "bindings": {
        "alt+f1": "transport.toggle_record",
        "q": "app.quit", "a": "track.arm_all", "s": "transport.stop",
        "space": "transport.toggle_play" } })";
    const std::vector<std::string>* record = nullptr;
    for (const auto& g : KeyMap::essential_commands())
        if (std::find(g.begin(), g.end(), "transport.toggle_record") != g.end())
            if (!record) record = &g;  // the start group; the next is stop
    CHECK(record != nullptr);
    if (!record) return;

    KeyMap xterm;
    std::string err;
    CHECK(KeyMap::load_string(profile, "<test>", xterm, err));
    CHECK(xterm.reaches(*record, KeyMap::terminal_chords(), all));

    const auto device = filter_console_chords(KeyMap::terminal_chords(),
                                              builtin_console_keymap());
    KeyMap m;
    CHECK(KeyMap::load_string(profile, "<test>", m, err));
    CHECK(!m.reaches(*record, device, all));
    const auto notices = m.ensure_essentials(device, all);
    CHECK(!notices.empty());
    CHECK(m.reaches(*record, device, all));
}

int main() {
    test_parser();
    test_filter_default();
    test_remapped();
    test_builtin();
    test_essentials_on_console();
    return failures ? 1 : 0;
}
