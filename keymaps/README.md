# Key maps

The same engine, driven the way the operator already knows. A profile maps a
key to a command name the engine implements; it does not define new behaviour.

| File | Profile |
|---|---|
| `protools.json` | Pro Tools key bindings |
| `logic.json` | Logic Pro key bindings |
| `default.json` | Our own, used when no profile is selected |

All three exist. `protools.json` and `logic.json` name commands this build
does not implement yet; see "Known but unimplemented" below.

## Why this is a data file

Reproducing key bindings is reproducing a user interface convention, which is
not protected expression — see ADR-0004. Keeping them as data rather than code
also means an operator can edit one without rebuilding the image, which is the
actual reason it is worth doing this way.

## Format

    {
      "name": "Pro Tools",
      "comment": "optional, free text",
      "bindings": {
        "space":        "transport.toggle_play",
        "ctrl+shift+z": "edit.redo",
        "shift+3":      { "command": "track.toggle_arm", "target": 3,
                          "origin": "sonora" },
        "ctrl+q":       { "command": "app.quit", "verified": false,
                          "comment": "why we are unsure" }
      }
    }

A binding's value is either a command name, or an object with:

| Member | Meaning |
|---|---|
| `command` | required; a name from `contracts/api/include/sonora/command.h` |
| `target` | track number as the operator sees it, counting from 1, up to 1024. Only for commands that take a track. Without one, a track command acts on the selection -- and this build has no selection yet, so such a key reports that it failed, is left out of the on-screen key list, and does not count as a way to arm. |
| `argument` | free-form string for a command that needs a value. No command in this build takes one, so supplying it is a load error rather than a field that is quietly ignored. |
| `origin` | `"vendor"` (the default): the application's own key for this command. `"sonora"`: a chord we chose because the application has no key for the command -- arming track 3 by number, say. |
| `verified` | `false` marks a vendor binding we have not confirmed against the application's documentation. Defaults to `true`. Not allowed with `"origin": "sonora"`, which has nothing to be confirmed against. |
| `focus` | `"commands"`: the binding runs only while Pro Tools' Commands Keyboard Focus is on (see "Commands Keyboard Focus" below). The only value; Pro Tools' Clip List and Groups List focuses do not exist here, and naming one is a load error. Not allowed on `focus.commands` itself, which could then never turn focus back on. |
| `comment` | free text, usually why a binding is ours or unverified |

The key is a chord: zero or more modifiers and one key, joined by `+`.
Modifiers are `ctrl`, `shift`, `alt`, `cmd` and are order-independent;
`control`, `opt`/`option`, `command`/`super`/`meta`/`win` are accepted
spellings. Keys are lower-case names — `a`, `4`, `space`, `return`, `escape`,
`f7`, `left`, `bracketleft`, `numpad_3`, `numpad_plus`. Punctuation may be
written literally (`ctrl+[`) or by name (`ctrl+bracketleft`); they are the same
chord, and binding both is an error. An upper-case letter is a spelling of
shift, so `S` and `shift+s` are one chord.

The modifiers are written in the Windows/Linux spelling throughout:
`protools.json` uses `ctrl` where a Mac keyboard has Command. `logic.json`
uses `cmd`, because Logic is a Mac application and that is what its
documentation says.

## Loading fails loudly

A profile that names a command the engine has never heard of, misspells a key,
binds the same chord twice, or carries a member the schema does not define,
fails to load with the file, the line, the key and the reason. Nothing is
partially applied — a key that does nothing is worse than a key map that
refuses to load.

`ui/keymap/tests/test_keymap.cpp` loads all three shipped profiles and checks
every binding, so a typo fails CI rather than a session.

## What a terminal can send

Phase 1 has no graphical interface, so the only input device is a raw-mode
terminal, and a terminal sends bytes rather than key events. That is strictly
less than a keyboard can express, and a binding the device cannot send is a
key that does nothing however correct the file is. `sonora-tui` (which
`sonora-engine --keymap PATH` starts on its terminal, passing the profile on)
therefore
checks, after loading a profile, that the commands an operator *must* have --
arming a track, starting a take, stopping it, and leaving the engine -- are
reachable from this device and implemented in this build. Where one is not, it
says so at start-up, holds the message on screen, and binds the built-in
default's key in its place, naming the key and what that key used to do. It
does not refuse to start: on the appliance there is no shell to fall back to
and systemd would simply restart it, so a loud notice plus a working keyboard
beats a restart loop.

A repair never costs the profile an essential it already had. The default's
key goes on a chord the profile leaves free if there is one; otherwise it may
take over a single chord, preferring one whose binding cannot work in this
build, and only if everything that worked before still works. If neither is
possible the notice says the keyboard cannot do it.

One repair does not come from the default. A profile that starts a take on a
key of its own but stops it with a play/stop key (`transport.toggle_play`)
this build cannot run gets `transport.stop` on that play/stop key. The
default's substitute would be `transport.toggle_record`, and on Space that
starts a take exactly where the operator expects playback; `transport.stop`
stops a take as the application's Space does and never starts one.

With the shipped profiles that means: `protools.json` starts a take with
`ctrl+space` or `f12` and `logic.json` with `r` (both `transport.record`), and
both stop with `space`, which is `transport.toggle_play`. The engine plays
back since ADR-0009's port onto the control server, and `toggle_play` closes
a running take, so `space` needs no repair in either: it plays, and stops a
roll or a take, as the applications' Space does. `logic.json`'s quit is
`cmd+q`, so `q` is bound to `app.quit` from the default.

`ui/keymap/tests/test_keymap.cpp` sweeps all 256 bytes against every shipped
profile and fails if none of them arms a track, and pins the exact repairs
each shipped profile gets.

### The terminal mode

The engine puts the console in a mode where every key reaches it: no echo, no
line buffering, and no line-discipline functions on control keys. Flow control
is off, so Ctrl+S and Ctrl+Q are keys (`app.save` and `app.quit` in
`protools.json`) rather than freezing and thawing the console -- a frozen
console stopped the engine reading keys with a take open. Signals from the
keyboard are off, so Ctrl+C and Ctrl+Z are `edit.copy` and `edit.undo` rather
than killing the engine or stopping it mid-take. Ctrl+V and Ctrl+O are
delivered too. To leave the engine, use the profile's quit key; systemd's
SIGTERM and `kill` still stop it cleanly.

The one exception: if no key the profile has can run `app.quit`, even after
repair, the keyboard would have no way out at all. Then Ctrl+C is left to the
terminal -- it raises SIGINT and the engine closes the take and exits cleanly
-- and a notice says so. The profile is measured again against what the
keyboard can still send, so a binding on Ctrl+C is not counted, nor listed in
the footer. Ctrl+Z and Ctrl+\ stay keys.

The check above is made against the mode the terminal actually accepted, read
back after setting it: a control byte the line discipline would still eat is
not counted as a way to reach anything.

What the terminal path does handle:

- Ctrl+letter, as bytes 1..26, and Ctrl+Space as NUL. A few letters are the
  same byte as a named key and come out as that key: Ctrl+H is Backspace,
  Ctrl+I is Tab, Ctrl+J and Ctrl+M are Return.
- The shifted digits, read back from the characters a **US** layout prints on
  them, so `!` is `shift+1`. This is what makes the `shift+1`..`shift+8`
  arming keys in `protools.json` and `logic.json` work at a tty.
- `Alt+`*key*, as ESC followed by that key's byte or sequence.
- Arrows, Home/End, Insert/Delete, Page Up/Down and F1..F12, as CSI and SS3
  sequences, with xterm's Shift/Alt/Ctrl parameter -- including old xterm's
  SS3 form, where Ctrl+Up is ESC `O` `5` `A` -- and the Linux console's own
  F1..F5 (ESC `[` `[` A..E).

What it cannot, and no decoding will change:

- **Ctrl+Shift+anything.** Ctrl+Z and Ctrl+Shift+Z are both byte 0x1a. So
  `ctrl+shift+z` is unreachable at a tty, and so is Ctrl with punctuation or a
  digit, because Ctrl+`[` *is* the escape byte. That includes Pro Tools'
  `ctrl+7` Smart Tool.
- **Alt+`[` and Alt+Shift+O.** ESC `[` and ESC `O` are how every arrow and
  function key begins, so both are read as the start of a sequence.
- **The numeric keypad as distinct keys.** Keypad digits send the same bytes
  as the main ones, so every `numpad_*` binding is unreachable here.
- **Cmd/Super.** The protocol has no way to say it, so every `cmd+...`
  binding in `logic.json` is unreachable here by construction.
- **Any layout but US.** The shifted digits are decoded from the characters,
  not the keys; on a German or French layout the same keys send something
  else and the chord will be named wrongly. Fixing that needs the console
  keymap, which is Phase 3 work. The Qt window does not have this problem;
  see below.
- **Two keys held at once**, which is how Pro Tools engages the Smart Tool.

Known gap: the check assumes xterm's modifier encoding. On the Linux console
Alt+Fn and Alt+Left/Right switch virtual terminals, Shift+PgUp/PgDn scroll,
and Ctrl/Shift+arrow arrive without their modifier, yet those chords are still
counted as sendable. No essential in a shipped profile depends on one; see
`planning/BACKLOG.md`.

## What the Qt window can send

`sonora-ui` (`ui/qt`) reads key events, not bytes, so it can say more than a
tty: Ctrl+Shift chords, Cmd/Super, and the keypad as its own keys. The same
profile, the same `ensure_essentials()` repair and the same footer apply; only
the device differs. `ui/qt/src/qt_keys.h` is the decoder.

What a chord names in the window:

- **The digit row, by the key.** The physical key is read from the event's
  XKB keycode (Linux evdev code plus 8), so `shift+3` is Shift held on the key
  with 3 on it, whatever that key types on the layout in use: `#` on US, `3`
  on AZERTY, `#` on ABNT2. On AZERTY the unshifted "1/&" key is `1`, not
  `shift+7`; on ABNT2 Shift+6, a dead key, is still `shift+6`.
- **Letters, by the letter on the key.** `z` is the key with Z on it, on
  AZERTY too, which is what the tty sees and how Windows names letter keys. A
  layout with no Latin letter on the key (Cyrillic, Greek) gets the letter at
  that key's US position, again as Windows does.
- **Other punctuation, by its character**, read as a US layout reads it,
  exactly as at the tty: `+` is `plus`, `{` is `shift+bracketleft`.
- **Nothing, said out loud** ("that key means nothing here"), for a key the
  window cannot place rather than a guess: anything with AltGr held, a dead
  key off the digit row, a shifted-digit character (`!`, `*`, `)`, ...) typed
  on some other key -- AZERTY types `!` and `*` unshifted -- and every digit
  on a platform whose key events do not carry XKB keycodes. Qt's xcb and
  Wayland platforms do; any other platform has no digit row, and
  `ensure_essentials()` says so at start-up.

The difference that remains: the tty can only see characters, so on a layout
other than US the same keyboard arms tracks in the window and does not, or
arms the wrong one, at the tty. On AZERTY, `&` at a tty is still `shift+7`.
AltGr is a modifier no profile can name; if one ever needs it, that is a new
modifier in the chord grammar, not a decoder change.

### What the Qt window implements

The Edit and Mix windows install handlers, in the window's own registry,
for the edit modes, the Smart/Trim/Select/Grab tools, zoom, undo and redo,
separate, trim to the insertion point, clear, nudge, track selection, mute,
solo, `app.save` (the native session file, written by the window until the
engine has session commands) and `window.toggle_mix_edit` (`ctrl+equal` in `protools.json`, `x` in
`logic.json`, both `"verified": false` until checked against the guides).
A track command bound without a target acts on the selected track there, so
`shift+r`, `shift+s` and `shift+m` work in the window and still report that
they failed at a tty, which has no selection.

### Where a command goes

The window sends a command where its `CommandSpec::domain` says
(`contracts/api/include/sonora/command.h`, ADR-0007 §5). An Interface command
(`tool.*`, `zoom.*`, `edit.mode.*`, `focus.*`, `app.quit`) runs in the window
and never reaches the engine, even if an engine lists it. An Engine command
goes to the engine over the control API once the engine's Hello lists it;
until then a handler the window installed stands in (the Edit window's edits,
which reach the engine as model requests), and the engine's own takes over
the day it lists the command. Neither: "not implemented in this build", on
the status line.

### Commands Keyboard Focus

Pro Tools' single-key edit commands -- A trims the clip start, B separates, E
toggles zoom -- run only while Commands Keyboard Focus is on (Avid, *Pro Tools
Shortcuts Guide*, ch. 3: "To enable Commands Keyboard Focus for the Edit
window ... Click the a-z button for the Edit window. Press Command+Option+1
(Mac) or Control+Alt+1 (Windows)"). `protools.json` marks those keys
`"focus": "commands"` and binds `ctrl+alt+1` to `focus.commands`.

In the Qt window the a-z button in the footer, beside "Keys", shows the state, lit while
focus is on, and toggles it, as `ctrl+alt+1` does; it is shown only for a
profile that has focus keys. Focus starts on, which is how the letters
behaved before there was a toggle. With it off a focus key does not run its
command and says so on the status line, naming the key that turns focus on.
A focus key never counts as a way to arm, record, stop or quit, because it
can be off. The focus-free forms Avid documents for A and S are bound too
(`alt+shift+7`, `alt+shift+8`).

A terminal has no a-z indicator, and a mode the operator cannot see is worse
than none, so at the tty focus keys stay live as they always were.

Unverified: which state Pro Tools opens a session in; that Command+Option+1
turns focus off again (Avid describes enabling it); what Pro Tools does with
these letters while focus is off (Avid documents only the enabled case, see
`brain/research/protools-logic-key-bindings.md` §3); and whether focus applies
in the Mix window. Here it applies in both windows.

### Switching profiles

The Keys button at the left of the footer opens the Keys menu: every `*.json` in
the key map folder, each loaded when the list is made, so a file that would
be refused is listed with the reason rather than offered. Choosing one runs
the same essentials check and repair as start-up and replaces the key map's
notices; a file that no longer loads is refused and the keys stay as they
were. The Commands Keyboard Focus state is kept across a switch.

The folder is `--keymap-dir`, else `$SONORA_KEYMAP_DIR`, else the folder of
`--keymap`, else `share/sonora/keymaps` beside the binary (where `cmake
--install` puts these files), else `/usr/share/sonora/keymaps`. The choice is
remembered in `$XDG_CONFIG_HOME/sonora/keymap` (else
`~/.config/sonora/keymap`), as a bare file name when it is in the folder, and
used at the next start unless `--keymap` is given. If it cannot be written
the menu still switches, and a notice says the choice will not survive a
restart.

## Known but unimplemented

A command in the catalog with no handler in this build is a different thing
from a command that does not exist. The first is reported to the operator when
the key is pressed ("not implemented in this build"); the second refuses the
whole file. That split is what lets these profiles carry the real bindings for
edit modes and tools before the editor they drive exists, without any key ever
silently doing nothing.

## Unverified and Sonora bindings

These are two different things, and the files say which.

`"verified": false` is on a binding that claims to be the application's own
and that nobody has confirmed against its documentation. The value of a
profile is that it matches what the engineer's hands already know, so a wrong
binding is worse than a missing one. Confirming one and removing the flag is a
useful, small contribution.

`"origin": "sonora"` is on a chord we chose because the application has no
key for the command at all -- `protools.json`'s `shift+1`..`shift+8` arming
keys, for instance. There is nothing to confirm; the comment says why that
chord, and what it collides with if anything.

`test_keymap` prints both counts on every run, and fails if a binding whose
comment says it is ours is marked unverified instead.
