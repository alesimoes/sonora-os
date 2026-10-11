# Key maps

The same engine, driven the way the operator already knows. A profile maps a
key to a command name the engine implements; it does not define new behaviour.

| File | Profile |
|---|---|
| `protools.json` | Pro Tools key bindings |
| `logic.json` | Logic Pro key bindings |
| `default.json` | Our own, used when no profile is selected |

Only `default.json` exists so far. The other two land in Phase 3, once there
are enough commands for a profile to be meaningful (see
`planning/ROADMAP.md`).

## Why this is a data file

Reproducing key bindings is reproducing a user interface convention, which is
not protected expression — see ADR-0004. Keeping them as data rather than code
also means an operator can edit one without rebuilding the image, which is the
actual reason it is worth doing this way.

## Format

    {
      "name": "Default",
      "bindings": {
        "space":  "transport.toggle_record",
        "1".."8": "track.toggle_arm",
        "a":      "track.arm_all",
        "q":      "app.quit"
      }
    }

Command names are stable identifiers. A profile that names a command the engine
does not implement fails to load loudly, rather than silently ignoring the
line — a key that does nothing is worse than a key map that refuses to load.
