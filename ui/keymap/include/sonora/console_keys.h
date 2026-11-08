#pragma once
// What the Linux virtual console can hand to read(), derived from its own
// keymap (ADR-0070). KeyMap::terminal_chords() stays the xterm vocabulary;
// filter_console_chords() is the pure step that removes from it what a VT
// cannot deliver. Nothing here spawns a process or touches a device.

#include <string>
#include <string_view>
#include <vector>

#include "sonora/keymap.h"

namespace sonora {

// One `keycode N = a b c ...` row of `dumpkeys`: the action of each column.
// Each column is a modifier mask: bit 1 shift, 2 altgr, 4 control, 8 alt.
// Columns with no action are empty strings.
struct ConsoleKeyRow {
    int keycode = 0;
    std::vector<std::string> columns;  // columns[i] is for declared_masks[i]
};

struct ConsoleKeymap {
    std::vector<ConsoleKeyRow> rows;
    // Which modifier masks the `keymaps` line declared (0-15); empty means
    // the parser saw no `keymaps` line and assumed every column in order.
    std::vector<int> declared_masks;
    bool empty() const { return rows.empty(); }
};

// Parses the text of `dumpkeys` (full table). Malformed lines are skipped and
// described in `problems` when it is not null; an input that yields no rows
// returns an empty ConsoleKeymap, which the caller treats as "unreadable".
ConsoleKeymap parse_console_keymap(std::string_view dumpkeys_text,
                                   std::vector<std::string>* problems);

// The default Linux keymap's behaviour, used when the console is detected
// and `dumpkeys` cannot be run or parses to nothing. Never empty.
ConsoleKeymap builtin_console_keymap();

// Returns `xterm` without the chords the console keymap cannot deliver:
// VT switch and scroll actions, and Shift/Ctrl+cursor/navigation chords whose
// action equals the unmodified key's (no modifier parameter on a console).
// Alt chords stay only when the Alt column is a plain string (ESC prefix).
// Order of the survivors is preserved.
std::vector<KeyChord> filter_console_chords(const std::vector<KeyChord>& xterm,
                                            const ConsoleKeymap& console);

}  // namespace sonora
