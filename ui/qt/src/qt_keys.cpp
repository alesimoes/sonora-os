// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt_keys.h"

#include <QKeyEvent>
#include <QString>
#include <cctype>
#include <cstring>
#include <string>

namespace sonora::ui {

namespace {

struct Named {
    int key;
    const char* name;
};

const Named kNamed[] = {
    {Qt::Key_Space, "space"},       {Qt::Key_Return, "return"},
    {Qt::Key_Enter, "return"},      {Qt::Key_Tab, "tab"},
    {Qt::Key_Escape, "escape"},     {Qt::Key_Backspace, "backspace"},
    {Qt::Key_Delete, "delete"},     {Qt::Key_Insert, "insert"},
    {Qt::Key_Up, "up"},             {Qt::Key_Down, "down"},
    {Qt::Key_Left, "left"},         {Qt::Key_Right, "right"},
    {Qt::Key_Home, "home"},         {Qt::Key_End, "end"},
    {Qt::Key_PageUp, "pageup"},     {Qt::Key_PageDown, "pagedown"},
    {Qt::Key_F1, "f1"},   {Qt::Key_F2, "f2"},   {Qt::Key_F3, "f3"},
    {Qt::Key_F4, "f4"},   {Qt::Key_F5, "f5"},   {Qt::Key_F6, "f6"},
    {Qt::Key_F7, "f7"},   {Qt::Key_F8, "f8"},   {Qt::Key_F9, "f9"},
    {Qt::Key_F10, "f10"}, {Qt::Key_F11, "f11"}, {Qt::Key_F12, "f12"},
};

// With Num Lock off, xkb sends the navigation keysyms for the keypad; Qt
// keeps KeypadModifier on them, which is how they are told apart from the
// real Home or Up.
const Named kKeypad[] = {
    {Qt::Key_0, "numpad_0"},        {Qt::Key_1, "numpad_1"},
    {Qt::Key_2, "numpad_2"},        {Qt::Key_3, "numpad_3"},
    {Qt::Key_4, "numpad_4"},        {Qt::Key_5, "numpad_5"},
    {Qt::Key_6, "numpad_6"},        {Qt::Key_7, "numpad_7"},
    {Qt::Key_8, "numpad_8"},        {Qt::Key_9, "numpad_9"},
    {Qt::Key_Insert, "numpad_0"},   {Qt::Key_End, "numpad_1"},
    {Qt::Key_Down, "numpad_2"},     {Qt::Key_PageDown, "numpad_3"},
    {Qt::Key_Left, "numpad_4"},     {Qt::Key_Clear, "numpad_5"},
    {Qt::Key_Right, "numpad_6"},    {Qt::Key_Home, "numpad_7"},
    {Qt::Key_Up, "numpad_8"},       {Qt::Key_PageUp, "numpad_9"},
    {Qt::Key_Plus, "numpad_plus"},  {Qt::Key_Minus, "numpad_minus"},
    {Qt::Key_Asterisk, "numpad_multiply"}, {Qt::Key_Slash, "numpad_divide"},
    {Qt::Key_Period, "numpad_period"},     {Qt::Key_Comma, "numpad_period"},
    {Qt::Key_Delete, "numpad_period"},     {Qt::Key_Enter, "numpad_enter"},
    {Qt::Key_Return, "numpad_enter"},
};

// Shifted punctuation on a US layout that TerminalInput has no name for,
// because the tty profiles never needed one. The shifted digits are not
// here: they are named by the digit row's keycode, never by a character.
struct Shifted {
    char c;
    const char* base;
};
const Shifted kShiftedPunct[] = {
    {'_', "minus"},  {'{', "bracketleft"}, {'}', "bracketright"},
    {'|', "backslash"}, {':', "semicolon"}, {'"', "apostrophe"},
    {'<', "comma"},  {'>', "period"},      {'?', "slash"},
    {'~', "grave"},
};

// XKB keycodes, which are Linux evdev codes plus 8. The evdev codes are
// linux/input-event-codes.h's KEY_1 (2) .. KEY_0 (11), KEY_Q (16) ..
// KEY_P (25), KEY_A (30) .. KEY_L (38) and KEY_Z (44) .. KEY_M (50); the
// +8 is libxkbcommon's, and xkeyboard-config's keycodes/evdev agrees
// (<AE01> = 10, <AD01> = 24, <AC01> = 38, <AB01> = 52).
constexpr quint32 kXkbDigitRow = 2 + 8;  // "1" .. "9", then "0"
struct LetterRow {
    quint32 first;
    const char* letters;
};
const LetterRow kXkbLetterRows[] = {
    {16 + 8, "qwertyuiop"},
    {30 + 8, "asdfghjkl"},
    {44 + 8, "zxcvbnm"},
};

// AltGr is ISO_Level3_Shift, which xkeyboard-config maps to Mod5. Qt's xcb
// plugin may also report it as GroupSwitchModifier; Qt Wayland reports it
// only in the native mask. Both are read.
constexpr quint32 kXkbMod5 = 0x80;

unsigned mods_from_qt(Qt::KeyboardModifiers m) {
    unsigned out = kModNone;
    if (m & Qt::ControlModifier) out |= kModCtrl;
    if (m & Qt::AltModifier) out |= kModAlt;
    if (m & Qt::ShiftModifier) out |= kModShift;
    if (m & Qt::MetaModifier) out |= kModCmd;
    return out;
}

bool is_modifier_key(int key) {
    switch (key) {
        case Qt::Key_Shift:
        case Qt::Key_Control:
        case Qt::Key_Alt:
        case Qt::Key_AltGr:
        case Qt::Key_Meta:
        case Qt::Key_Super_L:
        case Qt::Key_Super_R:
        case Qt::Key_Hyper_L:
        case Qt::Key_Hyper_R:
        case Qt::Key_CapsLock:
        case Qt::Key_NumLock:
        case Qt::Key_ScrollLock:
            return true;
        default:
            return false;
    }
}

// The digit on the digit-row key `xkb_keycode`, or -1 for any other key.
int digit_row(quint32 xkb_keycode) {
    if (xkb_keycode < kXkbDigitRow || xkb_keycode >= kXkbDigitRow + 10) return -1;
    return static_cast<int>((xkb_keycode - kXkbDigitRow + 1) % 10);
}

// The letter a US layout puts on `xkb_keycode`, or 0.
char us_letter_at(quint32 xkb_keycode) {
    for (const auto& row : kXkbLetterRows) {
        const std::size_t n = std::strlen(row.letters);
        if (xkb_keycode >= row.first && xkb_keycode < row.first + n) {
            return row.letters[xkb_keycode - row.first];
        }
    }
    return 0;
}

bool is_shifted_digit(int key) {
    switch (key) {
        case Qt::Key_Exclam:
        case Qt::Key_At:
        case Qt::Key_NumberSign:
        case Qt::Key_Dollar:
        case Qt::Key_Percent:
        case Qt::Key_AsciiCircum:
        case Qt::Key_Ampersand:
        case Qt::Key_Asterisk:
        case Qt::Key_ParenLeft:
        case Qt::Key_ParenRight:
            return true;
        default:
            return false;
    }
}

bool is_dead_key(int key) {
    return key >= Qt::Key_Dead_Grave && key <= Qt::Key_Dead_Longsolidusoverlay;
}

}  // namespace

bool qt_scan_codes_are_xkb(const QString& platform_name) {
    return platform_name == QLatin1String("xcb") ||
           platform_name.startsWith(QLatin1String("wayland"));
}

QtKeyInput qt_key_input(const QKeyEvent& e, bool scan_codes_are_xkb) {
    QtKeyInput in;
    in.key = e.key();
    in.mods = e.modifiers();
    in.auto_repeat = e.isAutoRepeat();
    if (scan_codes_are_xkb) {
        in.xkb_keycode = e.nativeScanCode();
        in.xkb_mods = e.nativeModifiers();
    }
    return in;
}

QtKeyDecode chord_from_qt_key(const QtKeyInput& in, KeyChord& out) {
    out = KeyChord{};
    const int key = in.key;
    if (is_modifier_key(key)) return QtKeyDecode::ModifierOnly;
    if (in.auto_repeat) return QtKeyDecode::AutoRepeat;

    const unsigned mods = mods_from_qt(in.mods);

    if (in.mods & Qt::KeypadModifier) {
        for (const auto& k : kKeypad) {
            if (k.key == key) {
                out.key = k.name;
                out.mods = mods;
                return QtKeyDecode::Chord;
            }
        }
    }
    if (key == Qt::Key_Backtab) {  // what Qt calls Shift+Tab
        out.key = "tab";
        out.mods = mods | kModShift;
        return QtKeyDecode::Chord;
    }
    for (const auto& k : kNamed) {
        if (k.key == key) {
            out.key = k.name;
            out.mods = mods;
            return QtKeyDecode::Chord;
        }
    }

    // Everything below depends on the layout. AltGr takes a third character
    // off the key and no profile can name AltGr: on AZERTY, AltGr+3 types
    // "#", and that is not shift+3.
    if ((in.mods & Qt::GroupSwitchModifier) || (in.xkb_mods & kXkbMod5)) {
        return QtKeyDecode::Unrecognised;
    }

    // The digit row, by the key. Shift is whatever Qt says, whatever the key
    // typed -- a dead key included, such as ABNT2's Shift+6.
    const int d = digit_row(in.xkb_keycode);
    if (d >= 0) {
        out.key = std::string(1, static_cast<char>('0' + d));
        out.mods = mods;
        return QtKeyDecode::Chord;
    }
    // A digit or a shifted digit from any other key, or from a key the
    // platform did not identify, cannot be placed: AZERTY types "!" and "*"
    // unshifted, on keys far from the digit row. The keypad's were named
    // above.
    if ((key >= Qt::Key_0 && key <= Qt::Key_9) || is_shifted_digit(key)) {
        return QtKeyDecode::Unrecognised;
    }
    if (is_dead_key(key)) return QtKeyDecode::Unrecognised;

    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        out.key = std::string(1, static_cast<char>('a' + (key - Qt::Key_A)));
        out.mods = mods;
        return QtKeyDecode::Chord;
    }
    // A letter key on a layout with no Latin letter on it.
    if (const char l = us_letter_at(in.xkb_keycode); l != 0 && key > 0x7f) {
        out.key = std::string(1, l);
        out.mods = mods;
        return QtKeyDecode::Chord;
    }

    // Printable ASCII: Qt's key code for these is the character itself.
    if (key > 0x20 && key < 0x7f) {
        const char c = static_cast<char>(key);
        // Shift made the character; the character says whether the chord
        // carries it, exactly as at the tty. Ctrl, Alt and Cmd come from Qt.
        const unsigned other = mods & ~static_cast<unsigned>(kModShift);
        KeyChord c_chord;
        if (KeyMap::chord_from_terminal_byte(static_cast<unsigned char>(c), c_chord)) {
            out.key = c_chord.key;
            out.mods = c_chord.mods | other;
            return QtKeyDecode::Chord;
        }
        for (const auto& s : kShiftedPunct) {
            if (s.c == c) {
                out.key = s.base;
                out.mods = other | kModShift;
                return QtKeyDecode::Chord;
            }
        }
    }
    return QtKeyDecode::Unrecognised;
}

std::vector<KeyChord> qt_device_chords(bool scan_codes_are_xkb) {
    std::vector<QtKeyInput> keys;
    const auto key = [&keys](int k, quint32 xkb) {
        QtKeyInput in;
        in.key = k;
        in.xkb_keycode = xkb;
        keys.push_back(in);
    };
    for (const auto& k : kNamed) key(k.key, 0);
    key(Qt::Key_Backtab, 0);
    key(Qt::Key_Clear, 0);
    for (int k = 0x21; k < 0x7f; ++k) key(k, 0);
    if (scan_codes_are_xkb) {
        for (quint32 i = 0; i < 10; ++i) {
            key(Qt::Key_0 + static_cast<int>((i + 1) % 10), kXkbDigitRow + i);
        }
    }

    std::vector<KeyChord> chords;
    const auto add = [&chords](const KeyChord& c) {
        for (const auto& have : chords) {
            if (have == c) return;
        }
        chords.push_back(c);
    };
    for (QtKeyInput in : keys) {
        for (unsigned bits = 0; bits < 32; ++bits) {
            Qt::KeyboardModifiers m;
            if (bits & 1) m |= Qt::ControlModifier;
            if (bits & 2) m |= Qt::AltModifier;
            if (bits & 4) m |= Qt::ShiftModifier;
            if (bits & 8) m |= Qt::MetaModifier;
            if (bits & 16) m |= Qt::KeypadModifier;
            in.mods = m;
            KeyChord c;
            if (chord_from_qt_key(in, c) != QtKeyDecode::Chord) continue;
            // wlroots turns Ctrl+Alt+Fn into a VT switch before any client
            // sees it. The image has no getty to switch to, but the key
            // still never reaches us.
            const bool vt_switch = (c.mods & kModCtrl) && (c.mods & kModAlt) &&
                                   c.key.size() >= 2 && c.key[0] == 'f' &&
                                   std::isdigit(static_cast<unsigned char>(c.key[1]));
            if (vt_switch) continue;
            add(c);
        }
    }
    return chords;
}

}  // namespace sonora::ui
