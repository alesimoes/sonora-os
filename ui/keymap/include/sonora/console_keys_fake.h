#pragma once
// Test builders for ConsoleKeymap (ADR-0070). Header-only so contract tests
// need no implementation of the parser or filter.

#include <string>
#include <utility>

#include "sonora/console_keys.h"

namespace sonora::fake {

inline ConsoleKeyRow row(int keycode, std::vector<std::string> columns) {
    ConsoleKeyRow r;
    r.keycode = keycode;
    r.columns = std::move(columns);
    return r;
}

// Default-US-like: F1..F2 plain, Alt+Fn switches VT, cursor keys carry the
// same string with Shift/Control, Alt+a is an ESC-prefixed plain string.
inline ConsoleKeymap default_us() {
    ConsoleKeymap k;
    k.declared_masks = {0, 1, 2, 4, 8};
    // keycode 59 = F1 (Alt column: Console_1), 105 = Left, 30 = a.
    k.rows.push_back(row(59, {"F1", "F13", "", "Console_13", "Console_1"}));
    k.rows.push_back(row(60, {"F2", "F14", "", "Console_14", "Console_2"}));
    k.rows.push_back(row(105, {"Left", "Left", "", "Left", "Decr_Console"}));
    k.rows.push_back(row(30, {"a", "A", "", "Control_a", "Meta_a"}));
    return k;
}

// A remapped console: F2's Alt column is no longer a VT switch.
inline ConsoleKeymap remapped() {
    ConsoleKeymap k = default_us();
    k.rows[1].columns[4] = "Meta_F2";
    return k;
}

}  // namespace sonora::fake
