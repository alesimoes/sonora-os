# ADR-0098 - What is left of clip groups after ADR-0052: the Loop Clip dialog and three follow-ups

- Status: Accepted (recommended defaults, 2026-11-08)
- Date: 2026-11-08
- Item: `ui.editing.clip_groups`
- Relates to: ADR-0052 (the design, binding), ADR-0049 (comping shares `ctrl+alt+l`
  in the Logic keys), ADR-0097 and ADR-0094 (same closing pattern: an item whose
  core is delivered, decide what remains)
- Plan: `planning/plans/ui.mixer.folders.md` is the plan of a different item, see §3

## Summary

ADR-0052 and main already deliver clip groups and loops: the `SetClipGroup`
operation, `SetRegionSources`, the commands `clip.group`, `clip.ungroup`, `clip.loop`
and `clip.unloop`, their Edit menu entries, their key bindings, the group move as one
Compound, the screenshot and the tests. What is missing is the Pro Tools dialog that
asks for the number of loops. This ADR decides how that dialog ships, keeps the
format and the protocol as they are, and lists the three follow-ups that stay open.

## Context

Verified on `origin/main` (grep run on 2026-11-08 in this worktree):

- ADR-0052 is on main: `git log origin/main --oneline -S SetClipGroup` lists
  `02b58e12` and `62fd88e9`; `git show origin/main:brain/INDEX.md` line 45 has the
  ADR-0052 entry (Accepted, session format 24, window 20, protocol 57).
- The operations and commands: `git grep -n "clip.group\|clip.ungroup\|clip.loop\|clip.unloop" origin/main`
  finds them in `contracts/api/include/sonora/command.h` (lines 242 to 246),
  `keymaps/protools.json` (lines 349 to 364) and `ui/qt/qml/Main.qml` (lines 1394 to
  1397, the Edit menu). `SetRegionSources` is in `contracts/api/include/sonora/api/messages.h`
  and the ADR-0052 summary.
- The count parser: `parse_repeat_count` is declared in `ui/qt/src/edit_clipboard.h:108`
  and defined in `ui/qt/src/edit_clipboard.cpp:184`; `session_model_commands_clip.cpp:147`
  uses it for `clip.loop`'s argument.
- The repeat dialog that exists: `ui/qt/qml/Main.qml` lines 2126 to about 2190
  (`repeatPanel`, opened by `edit.repeat` with no count, OK and Return send the number
  through `shell.invokeWith`; a refused number keeps the dialog open).
- The screenshot `docs/images/sonora-ui-edit-clip-groups.png` and the tests
  `common/model/tests/test_clip_groups.cpp` and `ui/qt/tests/test_clip_groups.cpp` are
  on main (`git ls-tree origin/main`).
- Scenes are globbed: `ui/qt/CMakeLists.txt:216` has
  `file(GLOB SONORA_UI_SCENES CONFIGURE_DEPENDS src/scenes/*.cpp)`. A scene needs
  no edit to the CMake file. `edit_repeat.cpp` is registered this way.
- `ctrl+alt+l` is bound to `clip.loop` in `keymaps/protools.json` (line 356), with
  `"verified": false` and the comment "asking nothing: the last count, 3 repeats at
  first". Pro Tools' Loop Clip, Command+Option+L, asks for the count.

What is open: the Pro Tools dialog for Loop Clip, and three behaviours ADR-0052
named as follow-ups (its §3 and its trim note at lines 45 to 48, and the
"Automation Follows Edit" note at line 71).

## Decision

1. **No protocol, format or model change.** Protocol stays 57, session format 24,
   undo window 20. No new operation, no new command, no change to `SetClipGroup`
   or `SetRegionSources`. The Loop Clip dialog is a client of the existing
   `clip.loop` command.

2. **`Clip > Loop Clip...` is a menu entry that asks for the count.** It opens a
   dialog that offers the last count (3 at first), like Pro Tools. It ends in the
   existing `clip.loop` command with the count as its argument, parsed by
   `parse_repeat_count`. The dialog is the one `Main.qml` already has for
   `edit.repeat` (`repeatPanel`, lines 2126 onward), made generic over the command
   name: one dialog, two callers, not a second copy. The `edit.repeat` behaviour does
   not change.
   - The shortcut `ctrl+alt+l` keeps looping silently with the last count, with no
     dialog. This is a deliberate difference from Pro Tools, which asks: a loop bound
     to a key should not stop the hand. The difference is recorded here and in
     `protools.json`.
   - The last count is per-session state. It resets to 3 when a session opens, as
     all per-session state does.
   - A refused count (not a whole number, or out of range) is a held notice: the
     dialog stays open with the reason inside it, not on the 2.5 s status line. An
     operator who must see a refusal does not miss it.
   - While the count field has the keyboard, the profile's keys are off
     (`ShellModel::setTextEntry`), as for `edit.repeat`.

3. **The word "folders" in the BACKLOG title belongs to `ui.mixer.folders`.** That
   item is folder tracks and track stacks (BACKLOG line "Folder tracks and track
   stacks: `ui.mixer.folders`"); its own plan is `planning/plans/ui.mixer.folders.md`.
   `ui.editing.clip_groups` does not touch it, and nothing in this ADR or its PR
   changes folder tracks.

4. **Three follow-ups, each a BACKLOG line (text in the PR body; nobody edits BACKLOG
   here):**
   - (a) **Loop handle.** A handle dragged on the right edge of a clip (Logic: loop
     regions; Pro Tools: Loop trim) lets a clip be looped by dragging. It needs
     `ui/qt/src/edit_gestures.cpp`, `edit_controller.h` and `timeline_scene.cpp`, which
     `ui.editing.sample_edit` holds. It waits for that PR to merge.
   - (b) **Trimming a member trims its repeats.** ADR-0052 §1 chose copies, so each
     repeat is trimmed alone. Changing that is an owner decision. Recommended default:
     no; repeats stay independent, as ADR-0052 decided.
   - (c) **Automation Follows Edit follows a group move.** ADR-0052 §3 notes that
     automation does not follow a group move yet. Follow-up, no decision needed.

5. **Constraints on this feature.** `ui/qt/CMakeLists.txt`,
   `ui/qt/tests/CMakeLists.txt`, `session_model.h` and `shell_model.cpp` are held by
   other active features, so this feature adds no compiled file to them. A screenshot
   scene is allowed only as a new file under `ui/qt/src/scenes/`, which the glob picks
   up. `brain/INDEX.md` is held by `plugins.graph.parallel` and is not edited here; its
   entry is the one line in the PR body.

6. **The dialog ships in a follow-up feature.** `ui/qt/qml/Main.qml` is held by
   `plugins.offline`, so the Loop Clip... dialog cannot land in this feature. It is
   implemented by a follow-up feature after `plugins.offline` merges, using §2 as its
   design. Its BACKLOG line is in the PR body.

## Rationale

- The Pro Tools workflow is the reference (`docs/` and `keymaps/README.md`, ADR-0004).
  Asking for a count, with the last count offered, is how the engineer does it on
  the bench.
- Keeping the key silent is the faster path for a key bound to a loop, and it is a
  decision the operator can see in the key map (`protools.json`, `verified: false`
  until checked against Avid's guide).
- One dialog for `edit.repeat` and `clip.loop` avoids two copies of the same
  input behaviour (the router, the held refusal, the keyboard handover).
- No format or protocol change means no amendment to ADR-0052 and no migration.
- Each deferred item touches a file another feature holds, or needs an owner
  decision (b). Leaving them as BACKLOG lines keeps this item small.

## Consequences

- The menu entry Loop Clip... is a follow-up. Until it ships, the operator loops
  with `ctrl+alt+l` (last count, 3 at first) or the Edit menu's Loop Clip, which
  loops with the last count.
- `clip.loop` keeps its signature; the count is its argument, as now.
- The Main.qml dialog is generic over the command name; `edit.repeat` keeps its
  current behaviour and its tests.
- Three BACKLOG lines are created: the loop handle (waits for `ui.editing.sample_edit`),
  the trim of repeats (owner decision, default no), and automation following a
  group move.
- No engine code, no contract, no protocol, no session format.

## Alternatives rejected

- **A second dialog for Loop Clip.** Rejected: two copies of one input behaviour
  drift apart.
- **`ctrl+alt+l` asks, as Pro Tools does.** Rejected for the key: a loop bound to a
  key that stops to ask breaks the hand. The menu entry asks; the key does not. The
  difference is recorded.
- **A new `clip.loop_dialog` command.** Rejected: the existing command already takes
  the count; a new name is a new stable identifier with no new behaviour.
- **Storing the last count in the session file.** Rejected: that is a format change
  for a convenience, and per-session state resets at open by rule.
- **Ship the dialog in this feature by editing Main.qml anyway.** Rejected: it is
  held by `plugins.offline`; a side edit would collide with that feature.
- **Trim-follows-repeats by default.** Rejected pending the owner decision (b).

## Unverified

- Nothing real-time is touched: no `Session::process()` path, no audio thread, no
  buffer. No measurement is needed for this ADR.
- No build or test ran for this ADR (it is a decision record). The greps in
  Context were run on `origin/main`; the line numbers may move.
- `keymaps/protools.json` marks `ctrl+alt+l` as `verified: false`; it is not checked
  against Avid's guide in `brain/research/protools-logic-key-bindings.md` here.
- `planning/plans/ui.mixer.folders.md` is named by the owner; it is not in this
  worktree's checkout, so the plan was not read.
- The BACKLOG line for `ui.editing.clip_groups` was not located by its id in this
  checkout; its title is taken from the assignment.
- The Loop Clip dialog's exact layout and the status of `plugins.offline` are not
  verified here; they belong to the follow-up feature.
