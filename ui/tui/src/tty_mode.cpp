// SPDX-License-Identifier: GPL-3.0-or-later
#include "sonora/tty_mode.h"

#include <unistd.h>

namespace sonora {
namespace {

// On Linux _POSIX_VDISABLE is NUL, which is also what Ctrl+Space sends, so a
// disabled slot must never be read as "NUL is special".
bool is_special(const termios& t, int slot, unsigned char byte) {
    const cc_t c = t.c_cc[slot];
    return c != static_cast<cc_t>(_POSIX_VDISABLE) && c == byte;
}

}  // namespace

termios key_reading_mode(const termios& saved) {
    termios t = saved;
    // ISIG off: Ctrl+C and Ctrl+Z are edit.copy and edit.undo in the shipped
    // profiles, and as signals they killed or stopped the engine mid-take.
    // SIGTERM and SIGINT from systemd or kill still arrive; they never went
    // through the terminal. IEXTEN off: POSIX lets an implementation keep
    // Ctrl+V and Ctrl+O special outside canonical mode, and ctrl+v is paste.
    t.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
    // IXON/IXOFF off: with flow control on, Ctrl+S froze the console, so
    // draw() blocked in printf and keys stopped being read with a take open,
    // and Ctrl+Q -- Pro Tools' quit -- was eaten. The CR/NL rewrites and
    // ISTRIP go too, so the decoder sees the bytes the keyboard sent.
    t.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | ICRNL | INLCR | IGNCR |
                                        ISTRIP);
    // OPOST stays: draw() writes "\n" and relies on it becoming CR LF.
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    return t;
}

termios interrupt_key_mode(const termios& saved) {
    termios t = key_reading_mode(saved);
    t.c_lflag |= ISIG;
    // Set rather than inherited, because the notice on screen names Ctrl+C.
    t.c_cc[VINTR] = 0x03;
    t.c_cc[VQUIT] = static_cast<cc_t>(_POSIX_VDISABLE);
    t.c_cc[VSUSP] = static_cast<cc_t>(_POSIX_VDISABLE);
    return t;
}

bool tty_delivers_byte(const termios& t, unsigned char byte) {
    // Canonical mode hands nothing over until a line is complete, so no key
    // arrives as a key at all.
    if (t.c_lflag & ICANON) return false;
    if ((t.c_iflag & ISTRIP) && byte >= 0x80) return false;
    if (byte == '\r' && (t.c_iflag & (IGNCR | ICRNL))) return false;
    if (byte == '\n' && (t.c_iflag & INLCR)) return false;
    if ((t.c_iflag & IXON) &&
        (is_special(t, VSTART, byte) || is_special(t, VSTOP, byte))) {
        return false;
    }
    if ((t.c_lflag & ISIG) &&
        (is_special(t, VINTR, byte) || is_special(t, VQUIT, byte) ||
         is_special(t, VSUSP, byte))) {
        return false;
    }
    if (t.c_lflag & IEXTEN) {
#ifdef VLNEXT
        if (is_special(t, VLNEXT, byte)) return false;
#endif
#ifdef VDISCARD
        if (is_special(t, VDISCARD, byte)) return false;
#endif
#ifdef VWERASE
        if (is_special(t, VWERASE, byte)) return false;
#endif
#ifdef VREPRINT
        if (is_special(t, VREPRINT, byte)) return false;
#endif
    }
    return true;
}

}  // namespace sonora
