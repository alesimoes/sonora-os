# Pro Tools and Logic Pro key bindings, checked against the vendor documentation

Status: research note. Written 2026-10-11. The §8 change list was applied to
`keymaps/*.json` on 2026-10-12; see "Applied" at the end of §8.

Scope: every binding in `keymaps/protools.json` (65) and `keymaps/logic.json`
(56), checked against the vendors' own published shortcut documentation. In
scope under [ADR-0004](../decisions/ADR-0004-no-proprietary-reverse-engineering.md):
key bindings are an interface convention, and these are published documents.
Nothing here required a binary.

**Standard of evidence.** Everything below is *what the vendor documentation
says*. Neither application was run, and nothing was measured. There is no
"what the implementation actually does" column, and that distinction matters
for the handful of rows where the vendor's own two tables disagree — those are
marked. No row rests on a secondary source: where I had no primary source I
wrote Unknown.

## Sources

| # | Document | Version | How reached |
|---|---|---|---|
| PT-A | *Pro Tools Shortcuts Guide*, Avid Technology, "Created 5/20/2025 · REV A" | 2025.6 | <https://resources.avid.com/SupportFiles/PT/Pro_Tools_Shortcuts_2025.6.pdf> |
| PT-B | *Pro Tools Shortcuts Guide* | 2021.6 | <https://resources.avid.com/SupportFiles/PT/Pro_Tools_Shortcuts_2021.6.pdf> |
| PT-C | *Pro Tools Shortcuts Guide* | 12.5 | <https://resources.avid.com/SupportFiles/attach/Pro_Tools/12.5/Pro_Tools_Shortcuts.pdf> |
| LP-A | *Logic Pro User Guide for Mac*, Apple (PDF edition of the support.apple.com guide) | Logic Pro for Mac 12.3 | <https://help.apple.com/pdf/logicpromac/en_US/logic-pro-mac-user-guide.pdf> |
| LP-B | "Key commands for Global Commands in Logic Pro for Mac", Apple Support | 12.3 | <https://support.apple.com/guide/logicpro/global-commands-lgcp02bf31b6/mac> |

Three Pro Tools versions were used deliberately. PT-A's tables extract with the
shortcut column offset by a row in places; PT-C prints the Commands Keyboard
Focus table **in alphabetical order by shortcut** and PT-B prints it in QWERTY
order, so the two orderings cross-check each other key by key. Every Commands
Focus mapping below agrees in all three documents.

Pro Tools chords are quoted in our `protools.json` convention: `ctrl` = the
Mac **Command** key, `alt` = **Option**. Logic chords are quoted in our
`logic.json` convention: `cmd` = Command, `ctrl` = Mac **Control**, `alt` =
Option. See "Schema limitations" for why that convention breaks down for
Pro Tools.

---

## 1. Pro Tools

Counts: **Confirmed 51 · Corrected 3 · Ours 10 · Unknown 1 · Not expressible 1**
(the Not-expressible case is the Smart Tool, which also appears as a Corrected
row because a single-chord form does exist).

Section references are to PT-A unless noted.

### 1.1 Transport

| Our chord | Our command | Verdict | Correct chord / note |
|---|---|---|---|
| `space` | `transport.toggle_play` | **Confirmed** | "Start/stop playback — Spacebar". Ch.7 *Playback*. |
| `shift+space` | `transport.play_half_speed` | **Confirmed** | "Half-speed playback — Shift+Spacebar". Ch.7 *Playback*. |
| `ctrl+space` | `transport.toggle_record` | **Corrected (command)** | Chord right, command wrong. Ch.7 *Recording*: "Start record — Command+Spacebar, F12, or 3 (on numeric keypad)"; "Stop record — Spacebar". It is not a toggle; use **`transport.record`**, same as `f12`. Stopping is `space`. Update 2026-11-01: unchanged by record-ready (ADR-0009 update of 2026-11-01). Avid lists this key under *Start record*, so it starts a take at once; only the Record button and keypad 3 arm. |
| `f12` | `transport.record` | **Confirmed** | Ch.7 *Recording*, same row as above. Update 2026-11-01: unchanged by record-ready (ADR-0009 update of 2026-11-01). Avid lists this key under *Start record*, so it starts a take at once; only the Record button and keypad 3 arm. |
| `return` | `transport.return_to_zero` | **Confirmed** | "Return to start of session — Return (Mac) / Enter (Windows)". Ch.7 *Edit Selection, Definition, and Navigation*; clean in PT-B l.766 and PT-C l.696. |
| `alt+return` | `transport.go_to_end` | **Confirmed** | "Go to end of session — Option+Return (Mac) / Control+Enter (Windows)". Same table, PT-B l.767, PT-C l.697. Was marked unverified; it is right. **But** the Mac and Windows chords genuinely differ here (not the usual Command↔Ctrl swap): on Windows it is `ctrl+return`. Our profile follows the Mac set, so `alt+return` is correct by our own stated convention. |
| `numpad_0` | `transport.toggle_play` | **Confirmed** | Ch.7 *Transport Mode (Numeric Keypad)*: "Play/Stop — 0". Only when Preferences → Operations → Numeric Keypad is set to **Transport**. |
| `numpad_1` | `transport.rewind` | **Confirmed** | Same table: "Rewind — 1". |
| `numpad_2` | `transport.fast_forward` | **Confirmed** | Same table: "Fast Forward — 2". |
| `numpad_3` | `transport.record_ready` | **Confirmed (chord); command ambiguous in Avid's own guide** | *Transport Mode (Numeric Keypad)* calls keypad 3 "**Record Enable**" (arms the transport, does not roll). *Recording* lists "3 (on numeric keypad)" under "**Start record**". The same document says both. I could not resolve it from the documentation; keeping `transport.record` matches one table and contradicts the other. Flagged rather than silently chosen. Update 2026-11-01: resolved for Transport keypad mode, which this profile assumes. That table calls keypad 3 "Record Enable", and record-ready is that (ADR-0009 update of 2026-11-01), so it is now **`transport.record_ready`**. The *Recording* table's "Start record" stays with Command+Spacebar and F12. |
| `numpad_4` | `transport.toggle_loop` | **Confirmed** | *Transport Mode (Numeric Keypad)*: "Loop Playback mode on/off — 4". |
| `ctrl+shift+l` | `transport.toggle_loop` | **Confirmed** | Ch.7 *Playback*: "Loop playback toggle — Command+Shift+L, or Control-click on Transport Play button (Mac) / Control+Shift+L (Windows)". Was unverified; it is right. Two chords on one command is fine — both are real. |

### 1.2 Edit modes

All five confirmed from Ch.7 *Edit Modes and Edit Tools*, identical in PT-A,
PT-B and PT-C.

| Our chord | Our command | Verdict | Note |
|---|---|---|---|
| `f1` | `edit.mode.shuffle` | **Confirmed** | "Shuffle mode — F1, or Option+1 (on computer keyboard)". |
| `f2` | `edit.mode.slip` | **Confirmed** | "Slip mode — F2, or Option+2". |
| `f3` | `edit.mode.spot` | **Confirmed** | "Spot mode — F3, or Option+3". |
| `f4` | `edit.mode.grid` | **Confirmed** | "Grid mode — F4, or Option+4". |
| `grave` | `edit.mode.next` | **Confirmed** | "Cycle through Edit modes — Single Open Quote (\`) key, located above the Tab key". **Version caveat:** in PT-C (12.5) the Commands Keyboard Focus table assigns \` to "Move Edit Insertion To Next Edit"; PT-B and PT-A moved that to `'` and list no \` in the focus table. On 12.5 with focus on, \` was taken. |

Also documented and unbound by us: `shift+f4` = Enable Snap To Grid while in
another edit mode; `f1+f4`, `f2+f4`, `f3+f4` = Snap-to-Grid plus a mode (held
pairs — see Not expressible).

### 1.3 Tools

| Our chord | Our command | Verdict | Correct chord / note |
|---|---|---|---|
| `f5` | `tool.zoom` | **Confirmed** | "Zoomer tool — F5, or Command+1". |
| `f6` | `tool.trim` | **Confirmed** | "Trimmer tool — F6, or Command+2". |
| `f7` | `tool.select` | **Confirmed** | "Selector tool — F7, or Command+3". |
| `f8` | `tool.grab` | **Confirmed** | "Grabber tool — F8, or Command+4". |
| `f9` | `tool.scrub` | **Confirmed** | "Scrubber tool — F9, or Command+5 (if Mac OS X Exposé is disabled)". |
| `f10` | `tool.pencil` | **Confirmed** | "Pencil tool — F10, or Command+6 (if Mac OS X Exposé is disabled)". |
| `escape` | `tool.next` | **Confirmed** | "Cycle through Edit tools — Escape (Esc) key" (Windows column in PT-A/PT-B adds "or Center-click"). |
| `ctrl+f7` | `tool.smart` | **Corrected** | **`ctrl+7`.** Ch.7 *Edit Modes and Edit Tools*: "Smart Tool — F6+F7, F7+F8, or **Command+7** (on computer keyboard)" / "Control+7" on Windows. Identical in PT-A (l.1279 area), PT-B (l.714) and PT-C. Ours said `ctrl+f7`, which is not a Pro Tools binding at all. The single-chord form exists, so the comment in our file ("cannot express two keys held at once, so a single chord stands in") is wrong twice over — the held pair is real *and* there is a documented single chord. |

**Also confirmed as alternates we do not bind**, and worth knowing because they
occupy chords: `ctrl+1`…`ctrl+6` are the Zoomer/Trimmer/Selector/Grabber/
Scrubber/Pencil tools, and `alt+1`…`alt+4` are the four edit modes. Any future
addition to `protools.json` must avoid `ctrl+1..7` and `alt+1..4`.

### 1.4 Zoom

| Our chord | Our command | Verdict | Correct chord / note |
|---|---|---|---|
| `ctrl+bracketright` | `zoom.in` | **Confirmed** | Ch.7 *Zoom*: "Horizontal zoom in or out — Command+ ] or [". |
| `ctrl+bracketleft` | `zoom.out` | **Confirmed** | Same row. |
| `ctrl+alt+bracketright` | `zoom.vertical_in` | **Confirmed (chord); direction inferred** | Ch.7 *Zoom*: "Incrementally zoom vertically on all audio tracks — Command+Option+[ or Command+Option+]". Avid prints the pair without saying which is in and which is out. `]`→in, `[`→out follows the horizontal row's convention. The chords are right; the direction is an inference, not a citation. |
| `ctrl+alt+bracketleft` | `zoom.vertical_out` | **Confirmed (chord); direction inferred** | Same row. |
| `e` | `zoom.toggle` | **Confirmed** | Ch.3 *Commands Keyboard Focus Shortcuts*: "Zoom Toggle — E". Confirmed twice: PT-B lists it in QWERTY order, PT-C (l.292) lists it alphabetically as "Zoom Toggle E". **Focus-gated — see §3.** The focus-free equivalent is Mac **Control+E** / Windows **Start+E** (Ch.7 *Zoom*), which our schema cannot spell. |
| `alt+f` | `zoom.fit_session` | **Corrected** | **`alt+a` for `zoom.fit_session`; `alt+f` is `zoom.fit_selection`.** Ch.7 *Zoom*, clean in PT-C ll.905–911: "Horizontal zoom to show all clips and reset amplitude zoom — Option+A / **Alt+A**"; "Horizontal zoom and scroll timeline excluding MIDI events — Option+F / **Alt+F**". PT-A's same section also lists "Fill window with selection — Option-click on Zoomer tool **or Option+F**". So Alt+F fits the *selection* and Alt+A fits the *session*; ours had them the wrong way round with only one of the two bound. Recommend binding both. |

### 1.5 Nudge

| Our chord | Our command | Verdict | Note |
|---|---|---|---|
| `numpad_plus` | `nudge.forward` | **Confirmed** | Ch.7 *Editing, Nudging, and Trimming*: "Nudge forward by next Nudge value — Plus (+) key (on numeric keypad), or Period (.) (in Commands Keyboard Focus)". |
| `numpad_minus` | `nudge.back` | **Confirmed** | Same table: "Nudge back by next Nudge value — Minus (-) key (on numeric keypad), or Comma (,) (in Commands Keyboard Focus)". |

### 1.6 Edit operations

Ch.8 *Pro Tools Menus Keyboard Shortcuts* → *Edit Menu* and *File Menu*.

| Our chord | Our command | Verdict | Note |
|---|---|---|---|
| `ctrl+z` | `edit.undo` | **Confirmed** | *Edit Menu*: "Undo — Command+Z". |
| `ctrl+shift+z` | `edit.redo` | **Confirmed** | "Redo — Command+Shift+Z". |
| `ctrl+x` | `edit.cut` | **Confirmed** | "Cut — Command+X". |
| `ctrl+c` | `edit.copy` | **Confirmed** | "Copy — Command+C". |
| `ctrl+v` | `edit.paste` | **Confirmed** | "Paste — Command+V". |
| `ctrl+a` | `edit.select_all` | **Confirmed** | "Select All — Command+A". |
| `ctrl+d` | `edit.duplicate` | **Confirmed** | "Duplicate selection — Command+D / Control+D". |
| `ctrl+e` | `edit.separate_at_selection` | **Confirmed** | "Separate Clip at Selection — Command+E / Control+E". |
| `ctrl+h` | `edit.heal_separation` | **Confirmed** | "Heal Separation — Command+H / Control+H". |
| `ctrl+s` | `app.save` | **Confirmed** | *File Menu*: "Save Session — Command+S / Control+S". |
| `ctrl+q` | `app.quit` | **Unknown** | Quit/Exit is **not listed** in the *File Menu* table of PT-A, PT-B or PT-C, and a full-text search of all three for "quit" and "exit Pro Tools" returns nothing. Command+Q is the macOS system-wide Quit and Pro Tools has no reason to deviate, but that is platform convention, not an Avid citation. Left Unknown rather than dressed up as Confirmed. |
| `ctrl+b` | `edit.clear` | **Confirmed** | *Edit Menu*: "Clear — Command+B / Control+B". Was unverified; it is right. Corroborated a second time in Ch.7 *Automation*, PT-A l.1958: "Clear — Command+B / Control+B". |
| `alt+r` | `edit.repeat` | **Confirmed** | *Edit Menu*: "Repeat selection — Option+R / Alt+R". Was unverified; it is right. |
| `alt+shift+3` | `edit.consolidate` | **Confirmed** | *Edit Menu*: "Consolidate Clip — Option+Shift+3 / Alt+Shift+3" (PT-A ll.2355–2356). Our comment worried it might be on the numeric keypad: Avid annotates "(on numeric keypad)" explicitly where it applies — two rows earlier, for "Selection > Change Timeline to Match Edit — Option+Shift+5 (on numeric keypad)" — and does **not** annotate Consolidate. It is the main-keyboard 3. |

**Confirmed and unbound, worth considering:** `ctrl+t` = Trim Clip to Selection;
`ctrl+shift+e` = Insert Silence; `ctrl+f` = Create Fades (opens the dialog);
`ctrl+m` = Mute/Unmute Clips; `alt+shift+7` / `alt+shift+8` = Trim Clip Start /
End to Insertion — the focus-free equivalents of our `a` and `s`, see §3.

### 1.7 Commands Keyboard Focus letters

All eight chords are **correct**. Every one is cross-confirmed by PT-B (QWERTY
order), PT-C (alphabetical order) and PT-A. They are, however, all gated on
Commands Keyboard Focus — see §3, which matters more than any row here.

| Our chord | Our command | Verdict | Avid's name for it / note |
|---|---|---|---|
| `a` | `edit.trim_start_to_insertion` | **Confirmed** | "Trim Clip Start To Cursor". Focus-free equivalent: `alt+shift+7` (Edit > Trim Clip Start to Insertion). |
| `s` | `edit.trim_end_to_insertion` | **Confirmed** | "Trim Clip End To Cursor". Focus-free equivalent: `alt+shift+8`. |
| `d` | `edit.fade_to_start` | **Confirmed** | "Fade from Start". Focus-free equivalent: Mac Control+D / Windows Start+D (Ch.7 *Fades*) — unspellable in our schema. |
| `f` | `edit.fade_selection` | **Confirmed** | "Crossfade With No Dialog". Focus-free equivalent: Command+Control+F, "Apply crossfade without opening Fades dialog (uses last selected fade shape)" (Ch.7 *Fades*) — unspellable. |
| `g` | `edit.fade_to_end` | **Confirmed** | "Fade Out From Cursor To End Of Clip". Focus-free equivalent: Mac Control+G / Windows Start+G — unspellable. |
| `b` | `edit.separate_at_selection` | **Confirmed** | "Separate". Duplicates `ctrl+e` onto the same command, which is correct — Pro Tools has both. |
| `p` | `track.select_previous` | **Confirmed (chord); semantics approximate** | Avid: "Move Edit Insertion To Previous **Track**". That moves the edit insertion, which is Pro Tools' notion of track focus; it is not literally "select the track". The chord is right; the catalog has no command with the exact semantics (see §5). |
| `semicolon` | `track.select_next` | **Confirmed (chord); semantics approximate** | Avid: "Move Edit Insertion To Next Track". Same note. |

For reference, the rest of the focus layout (PT-C ll.266–315, PT-B ll.318–355),
so that future additions do not collide: `1`–`5` recall zoom memories 1–5;
`6`–`9` play to/from edit start/end; `0` Edit Selection To Timeline Selection;
`-` Toggle Playlist View Mode; `q`/`w` display timeline selection start/end;
`e` Zoom Toggle; `r`/`t` zoom out/in horizontal; `y`/`u`/`i` move clip
start/sync/end to current timecode; `o` Timeline Selection To Edit Selection;
`p` previous track; `[`/`]` play edit/timeline selection; `\` nudge play
earlier by grid; `a`–`k` as above plus `h`/`j`/`k` snap clip start/sync/end to
play position; `l` previous edit; `;` next track; `'` next edit; `z` undo,
`shift+z` redo, `x` cut, `c` copy, `v` paste, `b` separate; `n` toggle
insertion follows playback; `m`, `,`, `.`, `/` nudge play.

### 1.8 Ours, not theirs — and what they collide with

This is the section that can actually hurt. A collision means pressing our key
in real Pro Tools does something else.

| Our chord | Our command | Verdict | Collision in Pro Tools? |
|---|---|---|---|
| `shift+1` | `track.toggle_arm` 1 | **Ours** | **No collision.** No bare `Shift+1` appears in PT-A, PT-B or PT-C. |
| `shift+2` | `track.toggle_arm` 2 | **Ours** | **COLLIDES** — "Bypass (or unbypass) inserts A-E on selected tracks — Shift+2". Ch.7 *Plugin Inserts and Sends*, PT-A ll.2196–2202. |
| `shift+3` | `track.toggle_arm` 3 | **Ours** | **COLLIDES** — "Bypass (or unbypass) inserts F-J on selected tracks — Shift+3". Same table. |
| `shift+4` | `track.toggle_arm` 4 | **Ours** | **COLLIDES** — "Mute (or unmute) Sends A-E on selected tracks — Shift+4". Same table. |
| `shift+5` | `track.toggle_arm` 5 | **Ours** | **COLLIDES** — "Mute (or unmute) Sends F-J on selected tracks — Shift+5". PT-A l.2228. |
| `shift+6` | `track.toggle_arm` 6 | **Ours** | **No collision** found. |
| `shift+7` | `track.toggle_arm` 7 | **Ours** | **No collision** found. |
| `shift+8` | `track.toggle_arm` 8 | **Ours** | **No collision** found. |
| `alt+shift+r` | `track.arm_all` | **Ours** | **No collision.** `Option+Shift+R` does not appear in any of the three guides (the only textual matches are prefixes of `Option+Shift+Return`). |
| `alt+shift+d` | `track.disarm_all` | **Ours** | **COLLIDES, and badly** — "Duplicate Tracks — Option+Shift+D / **Alt+Shift+D**". Ch.8 *Track Menu*, clean in PT-C l.1270, same in PT-A l.2387 and PT-B. In real Pro Tools this chord duplicates the selected tracks. An engineer reaching for "disarm everything" and getting a track duplication is the worst outcome in this file. Recommend moving it. Done: `track.disarm_all` moved to `alt+shift+w`, and since `daw.model.tracks` (ADR-0023) this chord is `track.duplicate`, as in Pro Tools. |

The neighbouring `shift+letter` keys are also taken and are worth avoiding:
`shift+a` bypass all inserts, `shift+q` mute all sends, `shift+e`/`shift+c`/
`shift+v`/`shift+d`/`shift+w` bypass EQ/Dynamics/Reverb/Delay/Modulation
plugins (Ch.7 *Plugin Inserts and Sends*).

**Pro Tools does have a record-arm key, and we do not bind it.** Ch.7 *Edit
Selection, Definition, and Navigation* (PT-A ll.1390–1397):

- `shift+r` — "Record enable tracks that contain the Edit cursor or Edit selection"
- `shift+s` — "Solo tracks that contain the Edit cursor or Edit selection"
- `shift+m` — "Mute tracks that contain the Edit cursor or Edit selection"
- `shift+i` — "TrackInput Monitor enable tracks that contain the Edit cursor or Edit selection"

These are the real, documented bindings for the four things our `track.*`
toggles do, and they act on the selection — which is what our commands are
meant to do when invoked with no `target`. (Correction, 2026-10-12: this note
first said they already do. They do not: the engine has no selection yet, so
a target-less `track.toggle_arm` fails, and `ensure_essentials()` does not
count it as a way to arm.) `shift+r` → `track.toggle_arm`, `shift+s` →
`track.toggle_solo`, `shift+m` → `track.toggle_mute`, `shift+i` →
`track.toggle_input_monitor` would be four Confirmed bindings for free, and
they make the eight invented `shift+digit` arms much less necessary. The one
thing I could not confirm from the documentation is whether Avid's "Record
enable" wording means toggle or set; the parallel "Bypass (or unbypass)" and
"Mute (or unmute)" phrasings elsewhere suggest toggle.

---

## 2. Logic Pro

Counts: **Confirmed 30 · Corrected 4 · Ours 20 · Not expressible 1 · Unknown 1**

All references are to LP-A (Logic Pro for Mac 12.3), chapter *Key commands*,
and each table is headed "The following table reflects default key commands
included in the U.S. default preset." LP-B is the same content served as a web
page and was used to confirm the Global Commands table independently.

Note the structural difference from Pro Tools: Apple's tables are *per window
or context* — Global Commands, Various Windows, Main Window Tracks, Mixer,
Various Editors, Tool Menu, and so on. A key means different things in
different tables. That is itself a schema problem (§4).

### 2.1 Transport

| Our chord | Our command | Verdict | Correct chord / note |
|---|---|---|---|
| `space` | `transport.toggle_play` | **Confirmed** | *Global Commands*: "Play or Stop — Space bar". |
| `r` | `transport.record` | **Confirmed** | *Global Commands*: "Record — R". |
| `return` | `transport.return_to_zero` | **Confirmed** | *Global Commands*: "Go to Beginning — Return (↩)". |
| `c` | `transport.toggle_cycle` | **Confirmed** | *Global Commands*: "Toggle Cycle Mode — C". Was `transport.toggle_loop`; Cycle is loop playback and loop record together and keeps the cycle area; with none set, the selection becomes it (ADR-0016, updates "loop record as a record mode" and "Logic's cycle area"). |
| `cmd+u` | `transport.cycle_selection` | **Unverified** | "Set Locators by Selection and Enable Cycle — Command-U", from Logic's default key commands; not yet quoted from Apple's guide here, so bound `"verified": false`. The only Logic key that moves the cycle area to a later selection. |
| `comma` | `transport.rewind` | **Confirmed** | *Global Commands*: "Rewind — Comma (,)". |
| `period` | `transport.fast_forward` | **Confirmed (chord); command approximate** | *Global Commands*: "**Forward** — Period (.)"; "**Fast Forward** — Shift-Period (.)". Logic distinguishes the two and our catalog does not. `.`→`transport.fast_forward` is the right pairing given the catalog, but it is Logic's *Forward*. If `transport.fast_forward` is ever split, `shift+period` is the real Fast Forward. |
| `numpad_0` | `transport.stop` | **Confirmed** | *Global Commands*: "Stop — 𝍖 0", where Apple's note reads "𝍖 indicates that the key shown is available twice on an extended keyboard, and that you must press the one on the numeric keypad". Was unverified; it is right. |
| `numpad_multiply` | `transport.record` | **Corrected (command)** | Chord right. *Global Commands*: "**Record/Record Toggle** — 𝍖 Asterisk (*)". Use **`transport.toggle_record`**; plain `transport.record` is `r`. Update 2026-10-19: bound to **`transport.punch`** once the catalog had it, since Record Toggle punches out with playback rolling on and `transport.toggle_record` stops the transport. |
| `alt+return` | `transport.go_to_end` | **Confirmed** | *Global Commands*: "Go to End of Last Region — Option-Return (↩)". Was unverified; it is right, with the caveat that Logic's end is the end of the last region, not a fixed project end. |

Also documented and unbound: `numpad_enter` = Play, `numpad_period` = Pause.

### 2.2 Edit operations

| Our chord | Our command | Verdict | Correct chord / note |
|---|---|---|---|
| `cmd+z` | `edit.undo` | **Confirmed** | *Various Windows*: "Undo — Command-Z". |
| `cmd+shift+z` | `edit.redo` | **Confirmed** | *Various Windows*: "Redo — Shift-Command-Z". |
| `cmd+x` | `edit.cut` | **Confirmed** | *Various Windows*: "Cut — Command-X". |
| `cmd+c` | `edit.copy` | **Confirmed** | *Various Windows*: "Copy — Command-C". |
| `cmd+v` | `edit.paste` | **Confirmed** | *Various Windows*: "Paste — Command-V". |
| `cmd+a` | `edit.select_all` | **Confirmed** | *Various Windows*: "Select All — Command-A". |
| `cmd+s` | `app.save` | **Confirmed** | *Global Commands*: "Save — Command-S". |
| `cmd+q` | `app.quit` | **Confirmed** | *Global Commands*: "Quit — Command-Q". |
| `cmd+t` | `edit.separate_at_selection` | **Confirmed** | *Global Commands*: "Split Regions/Events at Playhead Position — Command-T". Note "Split Regions/Events at **Locators or Marquee Selection** — Option-Command-T" is arguably nearer to "separate at selection"; both are real, and `cmd+t` is the one an engineer's hands know. |
| `cmd+j` | `edit.heal_separation` | **Confirmed** | *Global Commands*: "Join Regions/Notes — Command-J". (Distinct from *Main Window Tracks*' "Join Regions per Tracks / Fill Cell — J".) |
| `cmd+r` | `edit.repeat` | **Confirmed** | *Global Commands*: "Repeat Regions/Cells/Events — Command-R". |
| `delete` | `edit.clear` | **Confirmed (prose, not a table)** | The Delete key is not listed in any key-commands table. The guide states it in prose: "Choose Edit > Delete (or press Delete)" (LP-A, *Delete an audio region*). Edit > Delete removes the selection without the clipboard, which is `edit.clear`. Caveat: one other passage says "choose Edit > Cut (or press Delete)" in a Marquee context, so the mapping is not uniform across contexts. The judgement recorded in our comment is sound. |
| `cmd+d` | `edit.duplicate` | **Ours — and COLLIDES** | **Remove or rebind.** `Command-D` in Logic is "**New Track with Duplicate Settings**" (*Global Commands*). It also means "Duplicate Event and Numerical Edit", "Duplicate Row with Next Assignment" and "Duplicate Group" in other windows. Logic has no "duplicate the selection" command at all — its equivalent workflow is Repeat (`cmd+r`), which we already bind. Our comment ("Logic's Duplicate; unconfirmed, and easily confused with Repeat") understates it: pressing `cmd+d` in Logic **creates a track**. |
| `ctrl+b` | `edit.consolidate` | **Confirmed** | *Main Window Tracks*: "Bounce Regions/Cells in Place — Control-B". Was unverified; it is right, and it is the correct analogue of Consolidate. |
| `ctrl+alt+bracketleft` | `edit.trim_start_to_insertion` | **Corrected** | **`cmd+bracketleft`.** *Global Commands*: "Set Region/Event/Marquee **Start** to Playhead Position — Command-Left Bracket ([)". Logic *does* have a default key for trimming to the playhead; our comment claiming otherwise is wrong. |
| `ctrl+alt+bracketright` | `edit.trim_end_to_insertion` | **Corrected** | **`cmd+bracketright`.** *Global Commands*: "Set Region/Event/Marquee **End** to Playhead Position — Command-Right Bracket (])". |
| `cmd+f` | `edit.fade_selection` | **Corrected, and ours COLLIDES** | **`ctrl+alt+x`.** *Main Window Tracks*: "Apply Default Crossfade — Control-Option-X". Logic does have a fade key command; our comment claiming otherwise is wrong. Worse, `Command-F` is taken: *Main Window Tracks* "Show/Hide Flex Pitch/Time — Command-F". Related and unbound: `alt+x` = Audio Crossfade Options for Merge, `alt+shift+x` = Apply Last Edited Fade Again. |

Adjacent and unbound, relevant because `ctrl+[`/`ctrl+]` are nearly our old
chords: "Trim Region End to Previous/Next Transient — Control-[ / Control-]"
and "Trim Region Start to Previous/Next Transient — Control-Shift-[ /
Control-Shift-]" (*Main Window Tracks*). Our former `ctrl+alt+[` / `ctrl+alt+]`
did not collide with those, but they were inventions sitting in the middle of a
real family.

### 2.3 Zoom

| Our chord | Our command | Verdict | Correct chord / note |
|---|---|---|---|
| `z` | `zoom.fit_selection` | **Confirmed (chord); command approximate** | *Various Windows*: "**Toggle** Zoom to Fit Selection or All Contents — Z". It is a toggle with two states, so `zoom.toggle` is arguably the closer catalog entry; `zoom.fit_selection` captures the first press. Flagging rather than changing. |
| `cmd+right` | `zoom.in` | **Confirmed** | *Various Windows*: "Zoom Horizontal In — Command-Right Arrow". |
| `cmd+left` | `zoom.out` | **Confirmed** | *Various Windows*: "Zoom Horizontal Out — Command-Left Arrow". |
| `cmd+down` | `zoom.vertical_in` | **Confirmed** | *Various Windows*: "Zoom Vertical In — Command-Down Arrow". |
| `cmd+up` | `zoom.vertical_out` | **Confirmed** | *Various Windows*: "Zoom Vertical Out — Command-Up Arrow". |
| `alt+z` | `zoom.toggle` | **Ours — and COLLIDES** | *Various Windows*: "**Navigation: Back** — Option-Z". Also taken nearby: `shift+z` Store Navigation Snapshot, `ctrl+z` Toggle Zoom Focused Track or Row, `ctrl+shift+z` Zoom to Fit Locators. The whole `z` family is occupied; a free chord has to come from elsewhere. Our comment's reasoning ("Logic's Z already toggles zoom-to-fit") was right, the replacement was not. |

### 2.4 Nudge

| Our chord | Our command | Verdict | Note |
|---|---|---|---|
| `alt+right` | `nudge.forward` | **Confirmed** | *Global Commands*: "Nudge Region/Event/Marquee Position Right by Nudge Value — Option-Right Arrow". Was unverified; it is right. |
| `alt+left` | `nudge.back` | **Confirmed** | *Global Commands*: "Nudge Region/Event/Marquee Position Left by Nudge Value — Option-Left Arrow". Was unverified; it is right. |

Logic sets the nudge value with `ctrl+alt+m`/`b`/`d`/`t`/`f`/`h`/`s`/`1`/`0`
(bar/beat/division/tick/SMPTE frame/half frame/sample/1 ms/10 ms) — see the
`ctrl+alt+1` collision below.

### 2.5 Tools

Apple is explicit about the mechanism (LP-A, *Key commands for Tool Menu*):

> The following table lists the key commands that are specific to the Left-click
> Tool menu, **which are only active when the floating Left-click Tool menu is
> visible.** You can show and hide it at the current pointer position using the
> key command Show Tool Menu (default assignment: T).

and, in *Work with tools*: "Press the "T" key twice to return to the default
Pointer tool."

The tool keys are **letters, not digits**: P Pencil, E Eraser, I Scissors,
J Join, F Finger, Shift-T Text, Y Zoom, S Solo, M Mute, Q Quantize,
V Velocity, U Automation Select, W Automation Curve, A Fade, R Marquee,
X Flex, L Layout, Hyphen Voice Separation, Z Resize, C Camera, H Crosshair,
Shift-M MIDI Thru, G Gain. The comment in our file — "Logic picks tools from
the Tool menu by number after pressing T" — is wrong on the digits.

| Our chord | Our command | Verdict | Correct chord / note |
|---|---|---|---|
| `t` | `tool.next` | **Not expressible** | `T` is "Show Tool Menu" (*Various Windows*) — it opens a floating menu that then takes a *letter*. A menu is not a command and this schema cannot express the two-step chord. **However, there is a real cycling command we should bind instead:** *Various Windows* lists "**Set Next Tool or Edit Mode — Option-Page Down**" and "Set Previous Tool or Edit Mode — Option-Page Up". So `tool.next` belongs on **`alt+pagedown`** (both `page_down` and `pagedown` are accepted by `engine/src/keymap.cpp`), and a `tool.previous` command — which the catalog lacks — would belong on `alt+pageup`. |
| `escape` | `tool.select` | **Unknown** | Escape appears exactly once in the whole 12.3 user guide, and it is about closing a search field. There is no Esc tool binding in Apple's documentation, and no "Pointer tool" key command either — Apple documents pressing **T twice**. Our comment ("Escape returns to the Pointer tool (and in Logic also opens the Tool menu)") is unsupported by the primary source. Older Logic versions are widely said to have used Esc for the tool box, but I have only secondary recollection for that and no citation, so: Unknown. Treat `escape` as ours until someone with Logic in front of them says otherwise. |
| `ctrl+alt+1` | `tool.pencil` | **Ours — and COLLIDES** | *Global Commands*: "**Set Nudge Value to 1 ms** — Control-Option-1". Logic's Pencil is `T` then `P`. |
| `ctrl+alt+2` | `tool.zoom` | **Ours** | **No collision** found. Logic's Zoom Tool is `T` then `Y`. |
| `ctrl+alt+3` | `tool.scrub` | **Ours** | **No collision** found, but note Logic has **no Scrubber tool at all** — it is absent from the Tool menu list. This command has no Logic counterpart in any form. |

Logic also has no Trim, Grabber or Smart Tool, as our file says — confirmed by
the Tool menu table. Conversely Logic has a dozen tools our `tool.*` catalog
cannot name (Fade, Marquee, Flex, Gain, Quantize, Velocity, Mute, Solo, …).

### 2.6 Edit modes

Confirmed as absent: Logic has no Shuffle/Slip/Spot/Grid *edit modes*. Its
nearest equivalents are the Drag modes (Overlap, No Overlap, X-Fade, Shuffle),
which have **no key commands at all** — a full-text search of the key-commands
chapter for "Drag Mode" returns nothing — and Snap, which does:
"Toggle Snap to Grid — Command-G" (*Global Commands*). Also real:
"Shuffle Left — Option-[" and "Shuffle Right — Option-]", which are region
moves, not a mode.

| Our chord | Our command | Verdict | Collision? |
|---|---|---|---|
| `ctrl+f1` | `edit.mode.shuffle` | **Ours** | **No collision** found. Caveat: macOS claims F1–F4 for display brightness and Mission Control unless "Use F1, F2, etc. keys as standard function keys" is set — that is a Logic-platform fact, not a Logic binding, and irrelevant on our own image. |
| `ctrl+f2` | `edit.mode.slip` | **Ours** | **No collision** found. |
| `ctrl+f3` | `edit.mode.spot` | **Ours** | **No collision** found. |
| `ctrl+f4` | `edit.mode.grid` | **Ours** | **No collision** found. Note `cmd+g` is the real "Toggle Snap to Grid" and would be a more honest home for a Grid-ish command. |
| `ctrl+grave` | `edit.mode.next` | **Ours — and COLLIDES** | *Views Showing Time Ruler*: "**Scroll in Play** — Control-Grave Accent (\`)". Also nearby: plain \` is "Catch Playhead Position" (*Various Windows*) and `shift+grave` is "Scroll to Selection". |

### 2.7 Track commands

| Our chord | Our command | Verdict | Note |
|---|---|---|---|
| `m` | `track.toggle_mute` | **Confirmed** | *Global Commands*: "**Toggle Channel Strip Mute** — M". Our comment ("Logic's M mutes the selected regions; here it mutes the selected track") is wrong — `M` already acts on the channel strip, i.e. the track. Region muting is `ctrl+m` ("Mute Notes/Regions/Cells/Folders On/Off"). The binding is right and the comment should go. |
| `s` | `track.toggle_solo` | **Confirmed** | *Global Commands*: "**Toggle Channel Strip Solo** — S". Same correction to the comment. |

### 2.8 Ours, not theirs

| Our chord | Our command | Verdict | Collision? |
|---|---|---|---|
| `shift+1` … `shift+8` | `track.toggle_arm` 1–8 | **Ours** | **No global collision.** No bare `Shift-<digit>` appears in *Global Commands*, *Various Windows*, *Main Window Tracks* or *Mixer*. The only bare `Shift-3` in the whole guide is "Enharmonic Shift: #" in the Score Editor and "Next note will be sharp" in the Step Input Keyboard — both window-local. Safe. |
| `alt+shift+r` | `track.arm_all` | **Ours — and COLLIDES** | *Global Commands*: "**Region Inspector Float** — Option-Shift-R". |
| `alt+shift+d` | `track.disarm_all` | **Ours** | **No collision** for `Option-Shift-D`. Note plain `Shift-D` is "Deselect All" (*Various Windows*), so this is close quarters but not a clash. |

**Logic does have a record-arm key, and we do not bind it.** *Main Window
Tracks*: "**Toggle Track Record Enable — Control-R**". Our file's claim that
"Logic arms a track from the track header, not from a default key" is wrong.
`ctrl+r` → `track.toggle_arm` (no `target`, so it acts on the selection once one exists)
is a Confirmed binding available for free, and it weakens the case for the
eight invented `shift+digit` arms.

Three more real bindings for commands we already have and leave unbound in
this profile:

- `ctrl+i` — "Toggle Channel Strip Input Monitoring" (*Global Commands*) → `track.toggle_input_monitor`
- `up` — "Select Previous Track" (*Global Commands*) → `track.select_previous`
- `down` — "Select Next Track" (*Global Commands*) → `track.select_next`

---

## 3. Commands Keyboard Focus — the thing that matters most

**Applied 2026-10-14 (`feature/ui-keymaps`): option 2 in the Qt window, with
option 4's focus-free trims.** Bindings carry `"focus": "commands"`,
`focus.commands` is on `ctrl+alt+1`, and an a-z button shows and toggles the
state. The tty keeps the letters live, having no indicator. See
`keymaps/README.md`, "Commands Keyboard Focus".

### What Pro Tools actually does

Avid, PT-A Ch.3:

> Commands Keyboard Focus provides single-key shortcuts for a range of edit and
> play commands.
>
> To enable Commands Keyboard Focus for the Edit window, do one of the
> following: Click the a-z button for the Edit window. Press Command+Option+1
> (Mac) or Control+Alt+1 (Windows).

There are **three** keyboard-focus targets, each with its own `a-z` button and
its own toggle:

| Focus target | Toggle (Mac / Windows) | What letters mean there |
|---|---|---|
| Edit window (Commands) | Command+Option+1 / Control+Alt+1 | the single-key command set in §1.7 |
| Clip List | Command+Option+2 / Control+Alt+2 | type the first letters of a clip name to select it (Ch.7 *Clip List*: "Select Clip by name — Click the Clip List a-z button, then type the first letters of the desired Clip") |
| Groups List | Command+Option+3 / Control+Alt+3 | enable/disable a group by its letter |

Exactly one of the three holds focus at a time. So in Pro Tools the letter `a`
is not a binding on the application — it is a binding on *whichever pane holds
keyboard focus*. Press `a` with the Clip List focused and you jump to a clip
beginning with "a". Press it with the Edit window focused and you trim the
clip start to the cursor.

**What I could not determine:** Avid documents what the letters do when a focus
target is enabled; the Shortcuts Guide never states what they do in the Edit
window when Commands Keyboard Focus is *off*. The inference from the structure
of the document is "nothing", because the single-key assignments live only in
Ch.3, under the focus heading, and are absent from the Edit-window tables in
Ch.7. But that is my inference from an absence, not an Avid sentence, and I am
not going to write it down as a fact. Avid also notes the related hazard the
other way round: Ch.7 *Recording* carries the footnote "Some Pro Tools
recording shortcuts use the same key commands as some Mac OS X shortcuts. If
you want to retain use of these key commands in Pro Tools, these Mac OS X
shortcuts must be disabled or remapped."

### Why this is a deviation for us

`engine/src/keymap.cpp` resolves a chord to one command, unconditionally. There
is no focus concept anywhere in the engine. So the eight letters in §1.7 plus
`e` are bound as if Commands Keyboard Focus were permanently on — which is a
real Pro Tools state, just not its default, and not a state the operator can
leave.

The cost of leaving it as-is is narrow but sharp: the engineer's reflex for
Commands Focus keys will fire, which is good, but nine letters are permanently
unavailable for anything else, and there is no equivalent of the Clip List /
Groups List letter-select behaviour to put on them later. The cost is paid in
the *future* UI, not today.

### Options, with what each costs

1. **Leave them bound unconditionally** (status quo). Zero work. The reflex
   works. The deviation is that nine letters can never mean anything else, and
   the profile will be wrong the day we grow a clip list or a groups list that
   wants letter-select. Honest only if the file says so — which it does, in the
   top-level `comment`, and should keep saying.

2. **Implement focus modes properly.** A focus owner, a toggle on
   `ctrl+alt+1`/`2`/`3`, and a binding that is conditional on the owner. This
   is the faithful answer and the expensive one: it needs a `condition` or
   `context` member in the keymap schema, a focus-owner concept in the engine,
   and an `a-z` indicator in the UI so the operator can see which state they
   are in. Pro Tools' own `a-z` button exists precisely because the mode is
   invisible otherwise. Without that indicator, a focus mode is worse than no
   focus mode.

3. **Bind the focus-free equivalents instead, and drop the letters.** Avid
   documents a non-focus chord for most of them, so the commands stay
   reachable: `alt+shift+7` Trim Clip Start to Insertion, `alt+shift+8` Trim
   Clip End to Insertion, `ctrl+e` Separate (already bound). This loses the
   reflex, which is the entire point of the profile, and three of them —
   `d`, `f`, `g` → Mac Control+D / Command+Control+F / Control+G — are not
   spellable in our schema at all (§4.3). Not recommended.

4. **Bind both.** Letters unconditionally *and* the focus-free chords. Costs
   nothing extra today, keeps the reflex, and makes the eventual move to
   option 2 a deletion rather than a rewrite. This is the cheapest thing that
   is not a dead end: do option 4 now, keep option 2 on the list, and record
   in `keymaps/README.md` that nine letters in `protools.json` encode a focus
   state the engine does not have.

Logic has the same shape of problem in a smaller place: the Tool-menu letters
are "only active when the floating Left-click Tool menu is visible", and
Apple's tables are per-window throughout (`m` is Toggle Channel Strip Mute
globally but the Mute Tool inside the Tool menu). Whatever mechanism solves
Commands Keyboard Focus should also be able to express "T, then P".

---

## 4. Schema limitations the UI work has to solve

1. **One chord, one command — no held pairs.** Pro Tools' Smart Tool is
   `F6+F7` or `F7+F8` held simultaneously, and `F1+F4`/`F2+F4`/`F3+F4` are
   "Snap to Grid plus this edit mode". Four real bindings cannot be written at
   all. (`ctrl+7` rescues the Smart Tool specifically; the Snap-to-Grid pairs
   have no single-chord form in the guide.)

2. **No two-step or prefix chords.** Logic's `T` then `P`. Pro Tools' Memory
   Location and Window Configuration keypad sequences, e.g. "New/Replace
   Windows Configuration at Number — Period (.) + Window Configuration number
   (1-99) + Asterisk (*)" and "Go to Previous Memory Location — Period (.) +
   Memory Location + Plus (+)". These need a chord *sequence*, with a timeout
   and a visible pending state.

3. **No modifier spelling for Mac Control / Windows Start in the Pro Tools
   profile.** This is the biggest concrete gap. Pro Tools uses four modifiers
   on each platform — Mac Command/Option/Control/Shift, Windows
   Ctrl/Alt/Start/Shift — and `README.md` spends `ctrl` on Command and `alt` on
   Option, leaving Mac-Control with no name. Real bindings that are therefore
   unwritable today: Zoom Toggle (Control+E / Start+E), Fade to Start
   (Control+D), Fade to End (Control+G), Apply crossfade without dialog
   (Command+Control+F), Recall Zoom presets (Control+1..5), Pro Tools Search
   (Control+Shift+S), the Keyboard Shortcuts window (Control+Shift+K), and the
   whole `Command+Control+…` Selection submenu. `keymap.cpp` already accepts
   `cmd`/`super`/`meta`/`win` as spellings of one modifier, so the fix is a
   fifth modifier, not a parser rewrite — but it is a schema change and it
   changes what `protools.json`'s header comment means.

4. **No condition or context on a binding.** Commands Keyboard Focus (§3),
   Logic's Tool-menu-visible context, and Logic's per-window tables all mean
   "this chord means X only in context Y". Without it, every context-sensitive
   binding is either wrong or permanently in one state.

5. **No way to mark a binding as mode- or preference-dependent.** All five
   `numpad_*` bindings in `protools.json` are correct **only** when
   Preferences → Operations → Numeric Keypad is set to *Transport*; the same
   keys are a shuttle controller in Shuttle mode (Pro Tools Ultimate only) and
   different again in Classic mode. The profile states this in its header
   comment, which is the right place for now, but the UI will need to say it
   where the operator can see it.

6. **No per-platform variant inside one profile.** Pro Tools' Mac and Windows
   sets are not a mechanical Command↔Ctrl swap: "Go to end of session" is
   Option+Return on Mac and Control+Enter on Windows. Our file can only carry
   one of the two.

7. **`verified` is a boolean, and the commonest real state is not boolean.**
   Many rows above are "chord confirmed, command approximate" — `period` in
   Logic (Forward vs Fast Forward), `p`/`semicolon` in Pro Tools (move edit
   insertion vs select track), `z` in Logic (toggle vs fit), `numpad_3` in
   Pro Tools (Avid's own tables disagree). A tri-state, or separate
   `chord_verified` / `command_verified`, would stop the next reader re-doing
   this work.

8. **No marker for "ours, and it collides with the host DAW".** Eight bindings
   across the two profiles collide with a real binding of the DAW they imitate.
   That is the one property of an invented binding that can actively mislead,
   and the schema has nowhere to record it.

---

## 5. Commands our bindings want and the catalog does not have

Read against `engine/include/sonora/command.h` as it stands (the catalog is
unchanged by the `takes_argument` addition). **Nothing was added — this is a
list of gaps.**

Needed by a correction above:

- `tool.previous` — Logic's "Set Previous Tool or Edit Mode" (Option-Page Up) is
  the documented partner of the `alt+pagedown` correction for `tool.next`.
  `edit.mode.previous` has the same problem on the edit-mode half of that
  command.

Needed to express a real binding of either DAW that maps onto something we
already half-have:

- `edit.toggle_snap` — Logic's "Toggle Snap to Grid" (Command-G) and Pro Tools'
  "Enable Snap To Grid while in another Edit mode" (Shift+F4). Distinct from
  `edit.mode.grid`.
- `tool.show_menu` — Logic's `T`. Without it the two-step tool selection cannot
  be represented even once sequences exist.
- `focus.commands` / `focus.clip_list` / `focus.groups_list` — Pro Tools'
  Command+Option+1/2/3. Needed by option 2 in §3.
- `edit.insertion_next` / `edit.insertion_previous` (track) — the exact
  semantics of Pro Tools' `p` and `;` ("Move Edit Insertion To Previous/Next
  Track"), which we currently approximate with `track.select_previous` /
  `track.select_next`.
- `edit.insertion_next_edit` / `edit.insertion_previous_edit` — Pro Tools' `l`
  and `'`.
- `transport.forward` / `transport.rewind_fast` — Logic distinguishes Forward
  (`.`) from Fast Forward (Shift-`.`) and Rewind (`,`) from Fast Rewind
  (Shift-`,`); the catalog has one of each.
- `edit.trim_to_selection` — Pro Tools' Command+T.
- `edit.insert_silence` — Pro Tools' Command+Shift+E, Logic's Control-Command-I.
- `edit.fade_dialog` — Pro Tools' Command+F opens the Fades dialog, as distinct
  from `edit.fade_selection` (Crossfade With No Dialog).
- `clip.toggle_mute` — Pro Tools' Command+M "Mute/Unmute Clips" and Logic's
  Control-M "Mute Notes/Regions/Cells/Folders On/Off". `track.toggle_mute` is a
  different object.
- `zoom.recall_preset` / `zoom.save_preset` with an argument — Pro Tools' zoom
  memories 1–5 (focus keys `1`–`5`) and Logic's Recall Zoom 1–3
  (Control-Option-Command-1..3). These are the first commands that would
  actually use `CommandContext::argument`, and so the first to set the new
  `takes_argument` flag.
- `transport.toggle_quickpunch` — Pro Tools' Command+Shift+P.
  (`transport.toggle_loop_record`, Option+L, was added 2026-10-26 and is
  bound `"verified": false` until quoted from Avid's guide.)
- `edit.quantize` — Logic's `Q`.
- `region.toggle_loop` — Logic's `L` ("Loop Regions/Cells/Folders On/Off"),
  which is not `transport.toggle_loop`.

Tools either DAW has and `tool.*` cannot name: Fade, Marquee, Flex, Gain,
Quantize, Velocity, Eraser, Scissors, Join, Finger, Text, Automation
Select/Curve, Resize, Crosshair (all Logic). Logic has no Scrubber, Trimmer,
Grabber or Smart Tool, as our profile says.

---

## 6. Version differences in the documented behaviour

### Pro Tools

- **Commands Focus, `` ` `` vs `'`.** PT-C (12.5) assigns "Move Edit Insertion
  To Next Edit" to `` ` ``. PT-B (2021.6) and PT-A (2025.6) assign it to `'`
  and list no `` ` `` in the focus table. Our `grave` → `edit.mode.next` is
  clean on current versions and was contended on 12.5.
- **Commands Focus, `0` vs `O`.** PT-C: "Timeline Selection To Edit Selection —
  0" and "Edit Selection To Timeline Selection — O". PT-B and PT-A: exactly the
  reverse. One of the two documents is in error and I could not determine which.
  It does not touch any of our bindings.
- **User-assignable shortcuts arrived.** PT-A has a whole chapter
  ("5 Custom Keyboard Shortcuts": Setup > Keyboard Shortcuts, Control+Shift+K /
  Start+Shift+K, with search, filters, tags, presets and export) and a Pro Tools
  Search field (Control+Shift+S / Start+Shift+S). Neither exists in PT-C. From
  roughly 2024-era Pro Tools onward, *every* binding in §1 is a **default**, not
  a property of the application — which is an argument for our profiles being
  data, and should be said in `keymaps/README.md`.
- **Tier, not version.** Shuttle mode on the numeric keypad, TrackPunch,
  DestructivePunch and several Selection submenu commands are marked "Pro Tools
  Ultimate only" or "Ultimate and Studio only". None of our bindings depend on
  one.
- Everything our two profiles bind that is confirmed above is identical across
  12.5, 2021.6 and 2025.6. The Smart Tool (`Command+7`), the tool F-keys, the
  edit-mode F-keys, the Commands Focus letters, the Edit-menu chords and the
  transport chords did not move.

### Logic Pro

- The guide I used documents **Logic Pro for Mac 12.3**. Apple's version picker
  offers 12.2, 12.0, 11.2, 11.1, 11.0, 10.8, 10.7.8, 10.7.5, 10.7.3, 10.7,
  10.6.2, 10.6 and X 10.5. **I checked only 12.3.** Whether, say, `ctrl+r`
  (Toggle Track Record Enable) or `ctrl+alt+x` (Apply Default Crossfade) carried
  the same assignment in 10.x is **not determined**.
- Apple's own *Global Commands* table lists both "Go to Marker Number 1 — 𝍖 1"
  and "Recall Screenset 1 — 𝍖 1", i.e. the same chord for two functions. I
  could not resolve which one actually owns the numeric keypad digit. It does
  not touch our bindings (we bind no bare or keypad digits in `logic.json`
  except `numpad_0`), but it is a reason not to add any.
- Every Logic table is prefaced "reflects default key commands included in the
  **U.S. default preset**". Logic ships other presets and the user can edit
  them, so the same caveat as Pro Tools applies: these are defaults.

---

## 7. What I could not determine

- **Pro Tools Quit/Exit.** Not in the *File Menu* table of any of the three
  Shortcuts Guides, and not anywhere in their text. `ctrl+q` rests on macOS
  convention alone.
- **Pro Tools `numpad_3`.** "Record Enable" in one Avid table, "Start record" in
  another, same document.
- **Pro Tools vertical zoom direction.** Avid prints "Command+Option+[ or
  Command+Option+]" without splitting in from out.
- **What Pro Tools' `a`/`s`/`d`/`f`/`g`/`b`/`p`/`;`/`e` do with Commands
  Keyboard Focus off.** Avid documents the enabled case only. "Nothing" is my
  inference from the document's structure, not a quotation.
- **Logic and Escape.** No tool binding for Esc anywhere in the 12.3 guide. I
  have only unsourced recollection that older Logic opened the tool box with it,
  which under the charter is a rumour.
- **Logic's version history** for the individual bindings above.
- **Whether Avid's "Record enable tracks…" (`shift+r`) toggles or sets.** The
  wording does not say; the parallel "Bypass (or unbypass)" rows suggest toggle.
- **Everything, in the charter's third sense.** Nothing here was measured.
  Neither Pro Tools nor Logic Pro was run — the project owner does not have
  either, and running them is not what ADR-0004 puts in scope anyway. This note
  is "what the specification says" from end to end. A row marked Confirmed means
  *the vendor's current documentation says so*, not that anyone watched it
  happen.

---

## 8. Change list, for applying to the JSON

Shortest path from here to a correct pair of files. **Applied 2026-10-12** —
every item below is now in `keymaps/*.json`, and
`engine/tests/test_keymap.cpp` (`test_profile_corrections`) checks the chord
moves. Where an item said "move" without naming a destination, the choice is
recorded under "Applied" at the end of this section.

### `keymaps/protools.json`

1. `ctrl+f7` → **`ctrl+7`** for `tool.smart`; clear `verified`; replace the
   comment with the F6+F7 / F7+F8 held-pair note as a *schema* limitation, not
   as the reason for the chord.
2. `alt+f` → keep the chord but change the command to **`zoom.fit_selection`**;
   add **`alt+a`** → `zoom.fit_session`. Clear `verified` on both.
3. `ctrl+space` → command becomes **`transport.record`**.
4. Clear `verified` (it is now confirmed) on: `alt+return`, `ctrl+shift+l`,
   `ctrl+alt+bracketright`, `ctrl+alt+bracketleft`, `e`, `ctrl+b`, `alt+r`,
   `alt+shift+3`, `a`, `s`, `d`, `f`, `g`, `b`, `p`, `semicolon`. Keep a comment
   on the nine focus-gated keys (`a s d f g b p ; e`) saying *why* they are
   bound unconditionally, and on `e` noting the unspellable Control+E form.
5. Move **`alt+shift+d`** off `track.disarm_all` — it is Pro Tools' Duplicate
   Tracks.
6. Add the `collides` note (prose comment, until the schema has a member) to
   `shift+2`, `shift+3`, `shift+4`, `shift+5`.
7. Add, all Confirmed: **`shift+r`** → `track.toggle_arm`, **`shift+s`** →
   `track.toggle_solo`, **`shift+m`** → `track.toggle_mute`, **`shift+i`** →
   `track.toggle_input_monitor` (no `target`, so they act on the selection).
8. `numpad_3` keeps its chord; add a comment recording that Avid's two tables
   disagree about the command.
9. `ctrl+q` keeps `"verified": false` with the comment changed to "not listed in
   Avid's Shortcuts Guide; macOS convention".

### `keymaps/logic.json`

1. `ctrl+alt+bracketleft` → **`cmd+bracketleft`**, `ctrl+alt+bracketright` →
   **`cmd+bracketright`**; clear `verified`; drop the "Ours" comments.
2. `cmd+f` → **`ctrl+alt+x`** for `edit.fade_selection`; clear `verified`; drop
   the "Ours" comment. `cmd+f` is Logic's Show/Hide Flex Pitch/Time.
3. `numpad_multiply` → command becomes **`transport.toggle_record`**; clear
   `verified`.
4. Remove **`cmd+d`** → `edit.duplicate`, or move it. In Logic it creates a
   track. Logic has no duplicate-the-selection command; `cmd+r` (Repeat) is
   already bound and is the real workflow.
5. `t` → replace with **`alt+pagedown`** → `tool.next`, confirmed as "Set Next
   Tool or Edit Mode". Keep a comment that `T` is Show Tool Menu and that the
   two-step `T`-then-letter form is not expressible. (`alt+pageup` →
   `tool.previous` once that command exists.)
6. Move **`alt+z`** off `zoom.toggle` — it is Logic's Navigation: Back. Also
   avoid `shift+z`, `ctrl+z`, `ctrl+shift+z`.
7. Move **`ctrl+alt+1`** off `tool.pencil` — it is Set Nudge Value to 1 ms.
8. Move **`ctrl+grave`** off `edit.mode.next` — it is Scroll in Play.
9. Move **`alt+shift+r`** off `track.arm_all` — it is Region Inspector Float.
10. Clear `verified` on: `numpad_0`, `alt+return`, `delete` (keep the prose-only
    citation in the comment), `ctrl+b`, `alt+right`, `alt+left`, `m`, `s`.
11. Rewrite the comments on `m` and `s`: `M` and `S` are Toggle Channel Strip
    Mute/Solo, i.e. already the track, not the region.
12. Add, all Confirmed: **`ctrl+r`** → `track.toggle_arm`, **`ctrl+i`** →
    `track.toggle_input_monitor`, **`up`** → `track.select_previous`,
    **`down`** → `track.select_next`.
13. Fix the file-level `comment`: Logic's tool keys are **letters**, not
    numbers; Logic *does* have keys for record-arm, trim-to-playhead and
    crossfade.
14. `escape` → keep `"verified": false`; change the comment to say Apple's 12.3
    guide documents no Esc tool binding and prescribes pressing `T` twice to
    return to the Pointer tool.

### Applied

All items in both lists were applied on 2026-10-12. The choices the list left
open:

| Profile | Item | Was | Now | How the new chord was checked |
|---|---|---|---|---|
| Pro Tools | 5 | `alt+shift+d` → `track.disarm_all` | `alt+shift+w` | No Option/Alt+Shift+W in PT-A, PT-B or PT-C (text search of all three). |
| Logic | 6 | `alt+z` → `zoom.toggle` | unbound | Logic's own zoom toggle is `Z`, already bound (to `zoom.fit_selection`, see §2.3), and `shift+z`, `ctrl+z`, `ctrl+shift+z`, `ctrl+alt+z` and `ctrl+alt+cmd+z` are all taken. A third invented chord would add a key no Logic user reaches for. Recorded in the file's top-level comment. |
| Logic | 7 | `ctrl+alt+1` → `tool.pencil` | `ctrl+alt+4` | No Control-Option-4 in LP-A. Logic's nudge-value family uses `ctrl+alt+1` and `ctrl+alt+0` only among the digits. |
| Logic | 8 | `ctrl+grave` → `edit.mode.next` | `ctrl+f5` | Follows `ctrl+f1`..`ctrl+f4`; no Control-F5 in LP-A. |
| Logic | 9 | `alt+shift+r` → `track.arm_all` | `alt+shift+y` | No Option-Shift-Y in LP-A. |

**Pro Tools item 3 (`ctrl+space` → `transport.record`)** needed engine work,
because `transport.record` had no handler and the start-up essentials check
would have rebound `space` to cover the gap. `transport.record` is now
implemented: it starts a take, and does nothing if one is already running. The
essentials check now also asks whether a take can be *stopped*, separately
from started, because Pro Tools starts on Command+Space and stops on Space.
Space is `transport.toggle_play`, and there is no playback yet, so at start-up
the engine rebinds `space` to `transport.stop` and says so. That stops a take
exactly as Pro Tools' Space does; where Pro Tools would play, it does nothing.
(An earlier version rebound it to `transport.toggle_record`, which started a
take where the operator expected playback.) Logic gets the same repair for the
same reason. Once playback exists and `transport.toggle_play` can stop a take,
the repair goes away.

**Found while applying, not in the list above.** LP-A's event-editor prose
(page 466 of the PDF) gives "Deselect All (or press Option-Shift-D)". That is
window-local and not in any key-command table, so §2.8's "no collision" stands
for the global tables. But it is close quarters, and `logic.json`'s comment on
`alt+shift+d` now says so.
