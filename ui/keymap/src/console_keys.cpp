// ADR-0070: what a Linux virtual console can hand to read(). Pure text and
// table work; no process, no device.
#include "sonora/console_keys.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace sonora {
namespace {

constexpr int kShiftBit = 1, kCtrlBit = 4, kAltBit = 8;

bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> split_words(std::string_view s) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_space(s[i])) ++i;
        size_t j = i;
        while (j < s.size() && !is_space(s[j])) ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool parse_int(std::string_view s, int& out) {
    if (s.empty() || s.size() > 5) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

// "0-2,4-6,8-9,12" -> masks. False on anything outside 0-15.
bool parse_keymaps_line(std::string_view list, std::vector<int>& masks) {
    masks.clear();
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string_view::npos) j = list.size();
        const std::string_view part = list.substr(i, j - i);
        const size_t dash = part.find('-');
        int lo = 0, hi = 0;
        if (dash == std::string_view::npos) {
            if (!parse_int(part, lo)) return false;
            hi = lo;
        } else if (!parse_int(part.substr(0, dash), lo) ||
                   !parse_int(part.substr(dash + 1), hi)) {
            return false;
        }
        if (lo > hi || hi > 15) return false;
        for (int m = lo; m <= hi; ++m) masks.push_back(m);
        i = j + 1;
    }
    return !masks.empty();
}

// Lines of a full dump that are real but carry no key action we use.
bool ignorable(std::string_view line) {
    static const char* const kIgnored[] = {"string", "compose", "charset", "#", "!"};
    for (const char* p : kIgnored)
        if (line.substr(0, std::strlen(p)) == p) return true;
    return false;
}

void note(std::vector<std::string>* problems, const std::string& what) {
    if (problems) problems->push_back(what);
}

bool parse_row(std::string_view line, ConsoleKeyRow& row) {
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) return false;
    const auto head = split_words(line.substr(0, eq));
    if (head.size() != 2 || head[0] != "keycode" || !parse_int(head[1], row.keycode))
        return false;
    row.columns.clear();
    for (std::string_view w : split_words(line.substr(eq + 1))) {
        if (w.size() > 1 && w.front() == '+') w.remove_prefix(1);
        row.columns.emplace_back(w);
    }
    return !row.columns.empty();
}

// Actions that switch or scroll the console, or are consumed by the kernel
// rather than sent to the reader.
bool consumed_by_kernel(const std::string& a) {
    return a.rfind("Console_", 0) == 0 || a == "Incr_Console" ||
           a == "Decr_Console" || a == "Last_Console" ||
           a.rfind("Scroll_", 0) == 0 || a == "Boot" || a == "Compose" ||
           a.rfind("Show_", 0) == 0 || a == "Caps_Lock" || a == "Num_Lock" ||
           a == "Hold";
}

// What the key does in the column for `mask`; "" when nothing is declared.
std::string action_for(const ConsoleKeymap& km, const ConsoleKeyRow& r, int mask) {
    size_t idx = static_cast<size_t>(mask);
    if (!km.declared_masks.empty()) {
        const auto it = std::find(km.declared_masks.begin(), km.declared_masks.end(), mask);
        if (it == km.declared_masks.end()) return {};
        idx = static_cast<size_t>(it - km.declared_masks.begin());
    }
    if (idx >= r.columns.size()) return {};
    const std::string& a = r.columns[idx];
    return a == "VoidSymbol" ? std::string() : a;
}

const ConsoleKeyRow* find_row(const ConsoleKeymap& km, int keycode) {
    for (auto it = km.rows.rbegin(); it != km.rows.rend(); ++it)
        if (it->keycode == keycode) return &*it;
    return nullptr;
}

// Linux keycodes (US layout) for the key names the xterm vocabulary uses;
// 0 for a key we cannot place, which the filter leaves alone.
int keycode_of(const std::string& key) {
    static const struct { const char* name; int code; } kNamed[] = {
        {"escape", 1}, {"backspace", 14}, {"tab", 15}, {"return", 28},
        {"space", 57}, {"home", 102}, {"up", 103}, {"pageup", 104},
        {"left", 105}, {"right", 106}, {"end", 107}, {"down", 108},
        {"pagedown", 109}, {"insert", 110}, {"delete", 111},
        {"f11", 87}, {"f12", 88}};
    for (const auto& n : kNamed)
        if (key == n.name) return n.code;
    if (key.size() >= 2 && key[0] == 'f') {
        int n = 0;
        if (parse_int(std::string_view(key).substr(1), n) && n >= 1 && n <= 10)
            return 58 + n;
    }
    if (key.size() == 1) {
        static const char kLetters[] = "qwertyuiopasdfghjklzxcvbnm";
        static const int kStart[] = {16, 30, 44};
        static const int kLen[] = {10, 9, 7};
        const char c = key[0];
        if (c >= '1' && c <= '9') return 2 + (c - '1');
        if (c == '0') return 11;
        if (c >= 'a' && c <= 'z') {
            int off = static_cast<int>(std::strchr(kLetters, c) - kLetters);
            for (int r = 0; r < 3; ++r) {
                if (off < kLen[r]) return kStart[r] + off;
                off -= kLen[r];
            }
        }
    }
    return 0;
}

bool is_navigation(const std::string& key) {
    static const char* const k[] = {"up", "down", "left", "right", "home",
                                    "end", "pageup", "pagedown", "insert", "delete"};
    for (const char* n : k)
        if (key == n) return true;
    return false;
}

bool keeps(const KeyChord& c, const ConsoleKeymap& km) {
    const int code = keycode_of(c.key);
    if (c.mods == kModNone || code == 0) return true;
    if (c.mods & kModCmd) return false;  // a console has no Super key
    const ConsoleKeyRow* row = find_row(km, code);
    if (!row) return true;
    int mask = 0;
    if (c.mods & kModShift) mask |= kShiftBit;
    if (c.mods & kModCtrl) mask |= kCtrlBit;
    if (c.mods & kModAlt) mask |= kAltBit;
    const std::string act = action_for(km, *row, mask);
    if (act.empty() || consumed_by_kernel(act)) return false;
    // Alt arrives as ESC plus the key, so it may equal the plain action;
    // Shift and Ctrl send no parameter, so on a navigation key an equal
    // action makes the chord indistinguishable from the plain key.
    if (!(mask & kAltBit) && is_navigation(c.key) && act == action_for(km, *row, 0))
        return false;
    return true;
}

// Default Linux keymap behaviour for the keys a chord can name, in the
// layout `dumpkeys` prints (keymaps 0-2,4-6,8-9,12).
const char kBuiltinText[] =
    "keymaps 0-2,4-6,8-9,12\n"
    "keycode 1 = Escape Escape Escape Escape Escape Escape Meta_Escape Meta_Escape Escape\n"
    "keycode 14 = Delete Delete Delete Delete Delete Delete Meta_Delete Meta_Delete Delete\n"
    "keycode 15 = Tab Tab Tab Tab Tab Tab Meta_Tab Meta_Tab Tab\n"
    "keycode 28 = Return Return Return Return Return Return Meta_Control_m Meta_Control_m Return\n"
    "keycode 57 = space space space nul nul nul Meta_space Meta_space nul\n"
    "keycode 59 = F1 F11 Console_13 F1 F11 Console_13 Console_1 Console_11 Console_1\n"
    "keycode 60 = F2 F12 Console_14 F2 F12 Console_14 Console_2 Console_12 Console_2\n"
    "keycode 61 = F3 F13 Console_15 F3 F13 Console_15 Console_3 Console_13 Console_3\n"
    "keycode 62 = F4 F14 Console_16 F4 F14 Console_16 Console_4 Console_14 Console_4\n"
    "keycode 63 = F5 F15 Console_17 F5 F15 Console_17 Console_5 Console_15 Console_5\n"
    "keycode 64 = F6 F16 Console_18 F6 F16 Console_18 Console_6 Console_16 Console_6\n"
    "keycode 65 = F7 F17 Console_19 F7 F17 Console_19 Console_7 Console_17 Console_7\n"
    "keycode 66 = F8 F18 Console_20 F8 F18 Console_20 Console_8 Console_18 Console_8\n"
    "keycode 67 = F9 F19 Console_21 F9 F19 Console_21 Console_9 Console_19 Console_9\n"
    "keycode 68 = F10 F20 Console_22 F10 F20 Console_22 Console_10 Console_20 Console_10\n"
    "keycode 87 = F11 F11 Console_23 F11 F11 Console_23 Console_11 Console_11 Console_11\n"
    "keycode 88 = F12 F12 Console_24 F12 F12 Console_24 Console_12 Console_12 Console_12\n"
    "keycode 102 = Find Find Find Find Find Find Find Find Find\n"
    "keycode 103 = Up Up Up Up Up Up Up Up Up\n"
    "keycode 104 = Prior Scroll_Backward Prior Prior Prior Prior Prior Prior Prior\n"
    "keycode 105 = Left Left Left Left Left Left Decr_Console Decr_Console Left\n"
    "keycode 106 = Right Right Right Right Right Right Incr_Console Incr_Console Right\n"
    "keycode 107 = Select Select Select Select Select Select Select Select Select\n"
    "keycode 108 = Down Down Down Down Down Down Down Down Down\n"
    "keycode 109 = Next Scroll_Forward Next Next Next Next Next Next Next\n"
    "keycode 110 = Insert Insert Insert Insert Insert Insert Insert Insert Insert\n"
    "keycode 111 = Remove Remove Remove Remove Remove Remove Remove Remove Remove\n";

}  // namespace

ConsoleKeymap parse_console_keymap(std::string_view text,
                                   std::vector<std::string>* problems) {
    ConsoleKeymap km;
    int lineno = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        const std::string_view line = trim(text.substr(pos, nl - pos));
        pos = nl + 1;
        ++lineno;
        if (line.empty() || ignorable(line)) continue;
        const std::string where = "line " + std::to_string(lineno) + ": ";
        if (line.size() > 7 && line.substr(0, 7) == "keymaps" && is_space(line[7])) {
            if (!parse_keymaps_line(trim(line.substr(7)), km.declared_masks))
                note(problems, where + "unreadable keymaps line");
            continue;
        }
        ConsoleKeyRow row;
        if (parse_row(line, row)) km.rows.push_back(std::move(row));
        else note(problems, where + "unrecognised: " + std::string(line));
    }
    if (km.rows.empty()) km.declared_masks.clear();
    return km;
}

ConsoleKeymap builtin_console_keymap() {
    return parse_console_keymap(kBuiltinText, nullptr);
}

std::vector<KeyChord> filter_console_chords(const std::vector<KeyChord>& xterm,
                                            const ConsoleKeymap& console) {
    std::vector<KeyChord> out;
    for (const KeyChord& c : xterm)
        if (keeps(c, console)) out.push_back(c);
    return out;
}

}  // namespace sonora
