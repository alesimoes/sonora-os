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
        "shift+3":      { "command": "track.toggle_arm", "target": 3 },
        "ctrl+q":       { "command": "app.quit", "verified": false,
                          "comment": "why we are unsure" }
      }
    }

A binding's value is either a command name, or an object with:

| Member | Meaning |
|---|---|
| `command` | required; a name from `engine/include/sonora/command.h` |
| `target` | track number as the operator sees it, counting from 1, up to 1024. Only for commands that take a track. Without one, a track command acts on the selection -- and this build has no selection yet, so such a key reports that it failed, is left out of the on-screen key list, and does not count as a way to arm. |
| `argument` | free-form string for a command that needs a value. No command in this build takes one, so supplying it is a load error rather than a field that is quietly ignored. |
| `verified` | `false` marks a binding we have not confirmed against the real application. Defaults to `true`. |
| `comment` | free text, usually the reason `verified` is `false` |

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

`engine/tests/test_keymap.cpp` loads all three shipped profiles and checks
every binding, so a typo fails CI rather than a session.

## What a terminal can send

Phase 1 has no graphical interface, so the only input device is a raw-mode
terminal, and a terminal sends bytes rather than key events. That is strictly
less than a keyboard can express, and a binding the device cannot send is a
key that does nothing however correct the file is. `sonora-engine` therefore
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
possible the notice says the keyboard cannot do it. With the shipped profiles
that means: `protools.json` and `logic.json` both start a take with
`transport.record` and stop it with `space`, which is `transport.toggle_play`
-- and there is no playback yet -- so `space` becomes `transport.toggle_record`
in both. It stops a take as those applications' Space does, and starts one
where they would play. `logic.json`'s quit is `cmd+q`, so `q` is bound to
`app.quit`.

`engine/tests/test_keymap.cpp` sweeps all 256 bytes against every shipped
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
  keymap, which is Phase 3 work.
- **Two keys held at once**, which is how Pro Tools engages the Smart Tool.

## Known but unimplemented

A command in the catalog with no handler in this build is a different thing
from a command that does not exist. The first is reported to the operator when
the key is pressed ("not implemented in this build"); the second refuses the
whole file. That split is what lets these profiles carry the real bindings for
edit modes and tools before the editor they drive exists, without any key ever
silently doing nothing.

## Unverified bindings

`"verified": false` means exactly that: nobody has confirmed the binding
against the application. The value of a profile is that it matches what the
engineer's hands already know, so a wrong binding is worse than a missing one.
`test_keymap` prints how many there are on every run. Confirming one and
removing the flag is a useful, small contribution.
