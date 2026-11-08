#pragma once
// Is this terminal the Linux virtual console, and what does its keymap say
// (ADR-0070)? Detection and the process run are injected so the decision
// logic is tested without a VT or a dumpkeys binary.

#include <functional>
#include <string>
#include <vector>

#include "sonora/keymap.h"

namespace sonora::tui {

// Whether ioctl(KDGKBTYPE) succeeds on `fd`: only a VT answers it.
using KeyboardTypeProbe = std::function<bool(int fd)>;
// Runs dumpkeys, puts its stdout in `out`; false if it could not run or failed.
using DumpkeysRunner = std::function<bool(std::string& out)>;

bool is_linux_console(int fd, const char* term_env, const KeyboardTypeProbe& probe);
KeyboardTypeProbe real_keyboard_type_probe();

// The text of dumpkeys, empty when the runner fails.
std::string read_dumpkeys(const DumpkeysRunner& runner);
// fork/exec of /usr/bin/dumpkeys, no shell, output bounded in size and time.
DumpkeysRunner real_dumpkeys_runner();

struct ConsoleChords {
    std::vector<KeyChord> chords;
    std::string notice;  // empty unless the built-in table stood in
};

// `xterm` unchanged when not a console; otherwise filtered by the parsed
// dumpkeys table, or by the built-in one with a notice when that is unusable.
ConsoleChords chords_for_console(const std::vector<KeyChord>& xterm, bool on_console,
                                 const DumpkeysRunner& runner);

}  // namespace sonora::tui
