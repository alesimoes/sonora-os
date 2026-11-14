# ADR-0212 - Loop Clip... dialog: how it is implemented

- Status: Accepted (recommended default, 2026-11-14)
- Date: 2026-11-14
- Item: `ui.editing.clip_groups`
- Relates to: ADR-0098 §2 (the design, binding), ADR-0052

## Decision

1. ADR-0098 §2 stands. `repeatPanel` in `Main.qml` takes a command (`edit.repeat` or
   `clip.loop`); OK and Return send the count to that command. No new QML file.
2. The Edit menu entry `Loop Clip...` sends `clip.loop` with the argument `ask`. The
   command checks a clip is selected (a refusal is the usual one) and emits the
   existing `repeatRequested(count, what)` with the last loop count (3 at first,
   per-session state). It changes nothing. No header, protocol or format change.
3. `ctrl+alt+l` sends no argument and still loops silently with the last count.
4. A refused count keeps the dialog open, as for `edit.repeat`. The panel's command
   is set only around the menu's one invoke, so every other request is a Repeat.

## Consequences

Tests in `ui/qt/tests/test_clip_groups.cpp`: `ask` opens with the last count and
edits nothing; count 4 is one `Loop Clip` undo step.
