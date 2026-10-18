// SPDX-License-Identifier: GPL-3.0-or-later
//
// A key map: chords to command names, loaded from JSON.
//
// Loading validates every command against command_catalog() and refuses the
// whole file if any binding names something unknown. A key map that half
// loads leaves the operator with keys that do nothing, which is the failure
// mode keymaps/README.md exists to prevent.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sonora/command.h"

namespace sonora {

// Bit flags rather than four bools, because a chord is compared and hashed as
// a unit and the terminal hands us the set all at once.
enum Mod : unsigned {
    kModNone = 0,
    kModCtrl = 1u << 0,
    kModShift = 1u << 1,
    kModAlt = 1u << 2,
    kModCmd = 1u << 3,  // Command on a Mac keyboard, Super elsewhere
};

// One key plus its modifiers. `key` is a normalised lower-case name:
// "a", "4", "space", "return", "f7", "left", "numpad_3", "bracketleft".
struct KeyChord {
    std::string key;
    unsigned mods = kModNone;

    bool operator==(const KeyChord& o) const {
        return mods == o.mods && key == o.key;
    }
    bool operator!=(const KeyChord& o) const { return !(*this == o); }
};

// Whose binding this is. A vendor binding reproduces the application's own
// key; a Sonora one is a chord we chose because the application has none for
// the command, and so has nothing to be verified against.
enum class BindingOrigin { Vendor, Sonora };

// When a binding runs. Pro Tools' single-letter edit keys (Trim Start is A,
// Separate is B) run only while Commands Keyboard Focus is on; with it off
// the same letters are not those commands. An interface that has a focus
// toggle (focus.commands) asks needs_commands_focus() before running one; a
// tty, which has no a-z indicator, keeps them live as it always did.
enum class BindingFocus { Always, Commands };

struct Binding {
    KeyChord chord;
    std::string spec;     // the chord as written in the file, for messages
    std::string command;
    CommandContext ctx;
    BindingOrigin origin = BindingOrigin::Vendor;
    // False where we are reproducing a vendor binding we could not confirm
    // against the real application. See keymaps/README.md.
    bool verified = true;
    BindingFocus focus = BindingFocus::Always;
    std::string comment;
    int line = 0;
};

// True when the binding names a command that acts on a track and gives it
// no target. There is no track selection in this build, so such a key can
// only fail: it must not count as a way to arm, nor be advertised as one.
bool needs_selection(const Binding& b);

// True when the binding only runs with Commands Keyboard Focus on. Such a key
// never counts as a way to reach an essential command: focus can be off.
inline bool needs_commands_focus(const Binding& b) {
    return b.focus == BindingFocus::Commands;
}

class KeyMap {
public:
    const std::string& name() const { return name_; }
    const std::string& comment() const { return comment_; }
    const std::vector<Binding>& bindings() const { return bindings_; }

    // Nullptr when nothing is bound to the chord. The caller decides whether
    // an unbound key is worth telling the operator about.
    const Binding* resolve(const KeyChord& chord) const;
    const Binding* resolve(const std::string& spec) const;

    // Both report precisely on failure: which chord, which line, what was
    // wrong. `origin` names the file in those messages.
    static bool load_file(const std::string& path, KeyMap& out, std::string& error);
    static bool load_string(const std::string& text, const std::string& origin,
                            KeyMap& out, std::string& error);

    // The map the engine uses when no profile is selected. Compiled in rather
    // than read from disk so the engine still starts with a usable keyboard on
    // an image with no keymaps directory; keymaps/default.json is the same set
    // of bindings and test_keymap asserts the two agree.
    static KeyMap built_in_default();

    // --- chord parsing ---
    // "ctrl+shift+s", "space", "numpad_+". Order of modifiers does not matter.
    static bool parse_chord(const std::string& spec, KeyChord& out,
                            std::string& error);
    static std::string chord_to_string(const KeyChord& chord);

    // Translates one byte read from a raw-mode terminal into a chord. Only
    // the chords a single byte can carry: Ctrl+letter (bytes 1..26), the
    // shifted digits a US layout puts on "!@#$%^&*()", and the printable
    // characters. Multi-byte sequences (every arrow, function key and
    // Alt+chord) are TerminalInput's job, not this one's.
    static bool chord_from_terminal_byte(unsigned char byte, KeyChord& out);

    // Every chord this build can produce from a raw-mode terminal, which is
    // strictly less than a keyboard can produce. It is the vocabulary the
    // reachability check in ensure_essentials() measures a profile against,
    // and it is generated from the same tables TerminalInput decodes with;
    // test_keymap sweeps the decoder and holds the two sets equal.
    static std::vector<KeyChord> terminal_chords();
    // The same, less every chord whose bytes the terminal does not hand over
    // as typed. `delivered` is asked about each byte; see tty_mode.h.
    static std::vector<KeyChord> terminal_chords(
        const std::function<bool(unsigned char)>& delivered);

    // Command groups the operator must be able to invoke or this is not a
    // recorder. Satisfying any one member satisfies the group.
    static const std::vector<std::vector<std::string>>& essential_commands();

    // Checks every essential group against the chords `device` can produce
    // and the commands `implemented` says this build has wired up, and
    // repairs what it can: a profile with a record key of its own gets
    // transport.stop on its dead play/stop key, and everything else comes
    // from built_in_default(). Returns one line per
    // finding for the caller to put in front of the operator; an empty return
    // means the keyboard can record.
    //
    // Repairing rather than refusing is deliberate: see the comment on the
    // definition in keymap.cpp.
    std::vector<std::string> ensure_essentials(
        const std::vector<KeyChord>& device,
        const std::function<bool(const std::string&)>& implemented);

    // Whether some binding in this map runs a member of `group`, on a chord
    // `device` can produce, with a handler `implemented` says exists. The
    // test ensure_essentials() repairs against, for a caller that needs to
    // know the outcome for one group rather than read the notices.
    bool reaches(const std::vector<std::string>& group,
                 const std::vector<KeyChord>& device,
                 const std::function<bool(const std::string&)>& implemented) const;

    // Rejected above this, rather than narrowed into whichever track the
    // low bits happen to name. No interface has a thousand inputs; a file
    // asking for track 4294967297 is a mistake, not a configuration.
    static constexpr long kMaxTrackTarget = 1024;

private:
    std::string name_;
    std::string comment_;
    std::vector<Binding> bindings_;
};

// Reassembles the byte sequences a raw-mode terminal produces into chords.
//
// One byte per read cannot work: Up arrow is ESC [ A, F5 is ESC [ 1 5 ~ and
// Alt+r is ESC r, so a reader that takes a byte at a time sees an Escape
// followed by letters and dispatches whatever those letters are bound to.
// This accumulates instead, and resolves the one genuine ambiguity -- a lone
// Escape looks exactly like the start of a sequence -- with the `idle` flag
// the caller sets once no byte has arrived for a short while.
//
// What a terminal still cannot express, and so neither can this:
//
//   - Alt+[ and Alt+Shift+O. ESC [ and ESC O are how every arrow and
//     function key begins, so both are taken as the start of a sequence.
//   - Ctrl+Shift+anything. Ctrl+Z and Ctrl+Shift+Z are both byte 0x1a, so
//     "ctrl+shift+z" in a profile is unreachable at a tty however it is
//     decoded. The same goes for Ctrl with punctuation: Ctrl+[ *is* ESC.
//   - Cmd/Super. Nothing in the tty protocol carries it, so every "cmd+..."
//     binding in keymaps/logic.json is unreachable here by construction.
//   - A layout other than US. The shifted digits are decoded from the
//     characters a US layout puts on them ("!" is Shift+1); on a German or
//     French layout those same keys send something else entirely, and this
//     will name the wrong chord. Fixing that needs the keymap of the console,
//     which is a Phase 3 problem, not something a byte stream can answer.
//   - Two keys held at once, which is how Pro Tools engages the Smart Tool.
//
// Nothing here runs on the audio thread; it allocates freely.
class TerminalInput {
public:
    enum class Result {
        None,          // nothing is buffered
        NeedMore,      // a sequence has begun and is not complete yet
        Chord,         // `out` is a chord
        Unrecognised,  // a complete sequence this build has no name for
    };

    struct Event {
        Result result = Result::None;  // Chord or Unrecognised
        KeyChord chord;                // set when result is Chord
    };

    void feed(const char* bytes, std::size_t count);

    // One pass of the interface loop: `count` is what read() returned, and
    // zero or less -- nothing arrived this frame -- is the escape timeout.
    // Returns every key press that is now complete, in order.
    std::vector<Event> frame(const char* bytes, long count);

    // `idle` tells us no further byte is coming for now, which is what turns
    // a buffered lone ESC into the Escape key rather than NeedMore.
    Result next(bool idle, KeyChord& out);

    bool pending() const { return !buf_.empty(); }

private:
    std::string buf_;
};

}  // namespace sonora
