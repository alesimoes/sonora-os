# ADR-0070: Console chords come from the console keymap

Status: Proposed (architect review pending).
Builds on: ADR-0004 (workflow, not binaries), `keymaps/README.md` (terminal chords).

## Summary

The Linux virtual console is not an xterm. `KeyMap::terminal_chords()` stays the xterm vocabulary; a pure step, `filter_console_chords()`, removes what a console cannot hand to `read()`, using the console's own keymap parsed from the text of `dumpkeys`. If the console is detected and that table is unavailable, a built-in tty1 table is used and the operator sees one notice. This ADR and its headers are the contract; the implementation is a later task.

## Context

On tty1 the kernel keymap consumes some chords before the application sees a byte (Alt+Fn and Ctrl+Alt+Fn switch VT, Alt+Left/Right step VTs, Shift+PageUp scrolls) and sends no modifier parameter for others (Shift+Left and Ctrl+Left emit the same bytes as Left). The xterm vocabulary promises chords that on tty1 do nothing or collide with the plain key. A key that silently does nothing is worse than a key that is not offered (CLAUDE.md of `ui-engineer`).

## Decision

1. **A different device.** `terminal_chords()` is unchanged. New pure `filter_console_chords(xterm, ConsoleKeymap)` drops: chords whose console action is a VT switch or scroll (`Console_N`, `Incr_Console`, `Decr_Console`, `Last_Console`, `Scroll_Forward`, `Scroll_Backward`, `Scroll_Lock`-class); Shift/Ctrl+cursor/navigation chords whose action string equals the unmodified key's. Alt+Fn and Alt+key stay only if the keymap's Alt column for that key is a plain string (ESC prefix).
2. **Source of truth.** The text of `dumpkeys` (full table: a `keymaps` line, then `keycode N = ...` rows; column index bit 1 shift, 2 altgr, 4 control, 8 alt), parsed by the pure `parse_console_keymap(std::string_view, std::vector<std::string>* problems)`. `ui/keymap` spawns no process; the caller (ui/tui) runs `dumpkeys` and passes the text.
3. **Fail loudly.** When the console is detected (stdin is a VT, `TERM=linux`) and `dumpkeys` cannot run or parses to zero rows, use `builtin_console_keymap()` (default Linux keymap behaviour) and put one notice in front of the operator. Never fall back silently to the full xterm set. Not a console (xterm, ssh, tmux): nothing is filtered.
4. **Headers only here.** `ui/keymap/include/sonora/console_keys.h` declares `ConsoleKeymap`, `parse_console_keymap`, `builtin_console_keymap`, `filter_console_chords`; `console_keys_fake.h` builds `ConsoleKeymap` values for tests (`default_us()`, `remapped()`). `keymap.h` does not grow. `ui/keymap/CMakeLists.txt` wires `sonora_keymap_console_keys` and `test_console_keys` behind `if(EXISTS ...)`, so later tasks add files without editing it. Contract test: `keymap_console_contract` (label `keymap`).

## Rationale

The console keymap is per machine and loadable (`loadkeys`), so a static filter is wrong the day someone remaps it; the table the kernel is actually using is the truth. Parsing text keeps the module pure and testable with fixtures, and the filter being a separate step leaves the xterm path untouched.

## Consequences

- tty1 offers only chords that arrive; the help and key-map views show the filtered set.
- ui/tui gains a detection step, a `dumpkeys` runner and a held notice (a refusal the operator must see is not the 2.5 s status line).
- A `dumpkeys` format drift is caught by "zero rows" and falls to the built-in table with a notice, not to a wrong set.

## Alternatives rejected

- **`dumpkeys` at runtime vs a static table only.** Static alone ignores remaps and non-US layouts; kept only as the loud fallback.
- **`ioctl(KDGKBENT)` per key.** Needs a device handle and per-keycode loops inside `ui/keymap`, is untestable without a VT, and returns symbol numbers needing their own name table; `dumpkeys` already gives names.
- **Filtering nothing and documenting it.** A chord that does nothing on the operator's own screen.

## Unverified

- The exact `dumpkeys` column layout on the Debian `kbd` version in the image (columns per `keymaps` line, `+` prefixed symbols, `keycode` rows with fewer columns).
- Which VT-switch symbols a custom keymap can rebind.

Handed to `researcher`: run `dumpkeys` and `dumpkeys --keys-only` on the appliance image with a US and a pt-BR layout and paste the output into `brain/research/console-dumpkeys.md`.
