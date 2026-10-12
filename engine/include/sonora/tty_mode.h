// SPDX-License-Identifier: GPL-3.0-or-later
//
// The terminal mode the interactive loop reads keys in, as a pure function of
// the mode it found, so the flag set can be tested without a terminal.
#pragma once

#include <termios.h>

namespace sonora {

// The mode to set on the console while the engine reads keys.
termios key_reading_mode(const termios& saved);

// key_reading_mode() with one signal key left live: Ctrl+C raises SIGINT,
// which stops the engine cleanly. For a profile with no key that can quit,
// where giving Ctrl+C to edit.copy would leave no way out at all. Quit and
// suspend stay off: Ctrl+Z stopping the engine mid-take is not a way out.
termios interrupt_key_mode(const termios& saved);

// Whether a byte typed at a terminal in mode `t` reaches read() unaltered,
// rather than being eaten or acted on by the line discipline. A model of
// Linux's n_tty, conservative where POSIX leaves room: IEXTEN functions are
// treated as live even outside canonical mode.
bool tty_delivers_byte(const termios& t, unsigned char byte);

}  // namespace sonora
