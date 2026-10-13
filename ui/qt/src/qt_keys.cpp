// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt_keys.h"

#include <cctype>
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
// here: chord_from_terminal_byte() already decodes those, and using it keeps
// "!" meaning shift+1 on both devices.
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

}  // namespace

QtKeyDecode chord_from_qt_key(int key, Qt::KeyboardModifiers qmods, bool auto_repeat,
                              KeyChord& out) {
    out = KeyChord{};
    if (is_modifier_key(key)) return QtKeyDecode::ModifierOnly;
    if (auto_repeat) return QtKeyDecode::AutoRepeat;

    const unsigned mods = mods_from_qt(qmods);

    if (qmods & Qt::KeypadModifier) {
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

    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        out.key = std::string(1, static_cast<char>('a' + (key - Qt::Key_A)));
        out.mods = mods;
        return QtKeyDecode::Chord;
    }
    if (key >= Qt::Key_0 && key <= Qt::Key_9) {
        out.key = std::string(1, static_cast<char>('0' + (key - Qt::Key_0)));
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

std::vector<KeyChord> qt_device_chords() {
    std::vector<int> keys;
    for (const auto& k : kNamed) keys.push_back(k.key);
    keys.push_back(Qt::Key_Backtab);
    for (int k = Qt::Key_A; k <= Qt::Key_Z; ++k) keys.push_back(k);
    for (int k = 0x21; k < 0x7f; ++k) {
        if (!(k >= Qt::Key_A && k <= Qt::Key_Z)) keys.push_back(k);
    }
    keys.push_back(Qt::Key_Clear);

    std::vector<KeyChord> chords;
    const auto add = [&chords](const KeyChord& c) {
        for (const auto& have : chords) {
            if (have == c) return;
        }
        chords.push_back(c);
    };
    for (int key : keys) {
        for (unsigned bits = 0; bits < 32; ++bits) {
            Qt::KeyboardModifiers m;
            if (bits & 1) m |= Qt::ControlModifier;
            if (bits & 2) m |= Qt::AltModifier;
            if (bits & 4) m |= Qt::ShiftModifier;
            if (bits & 8) m |= Qt::MetaModifier;
            if (bits & 16) m |= Qt::KeypadModifier;
            KeyChord c;
            if (chord_from_qt_key(key, m, false, c) != QtKeyDecode::Chord) continue;
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
