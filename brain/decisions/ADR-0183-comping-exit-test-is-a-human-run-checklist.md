# ADR-0183: The comping exit test is a human-run checklist

Status: accepted (2026-11-13)

## Context

ADR-0049 §6 defines the exit test for comping in the Edit window: a vocal with
three takes, comped with the lane button, a drag, To Main, a profile key,
Undo, and consolidated, every step on a control an engineer finds where Pro
Tools puts it. The scripted half is `ui/qt/tests/test_comping.cpp`
(`test_exit`), which runs the real QML with the Pro Tools key map.

A scripted test shows that the window and the model agree. It cannot show that
a key is the one an engineer expects, that a notice is held where the operator
looks, or that the playback sounds right. The item `ui.comping.exit_test` needs
that second check too, and it needs a human to do it.

## Decision

1. The exit test has two parts. `test_exit` stays as the scripted check. The
   human run is `docs/exit-tests/comping.md`: a checklist with the key from
   `keymaps/protools.json` for each step, and a PASS/FAIL box per step.
2. The checklist is written and merged without a run. Nobody ticks a box
   until a person has run the step on a build, and the tick names the commit.
3. Until a human run is recorded, the human half is **unverified**. A report or
   a backlog tick must not say the exit test passed on the strength of the
   checklist or of `test_exit` alone.
4. A key the checklist names that the profile does not bind, or binds to
   another command, is a finding against the binding. It is not patched in the
   checklist.

## Unverified

Everything below is unverified until a person runs `docs/exit-tests/comping.md`
on a build and records the result:

- Each of the 6 sections of the checklist, on a real interface and real audio.
- The loop-record binding `alt+l` (`transport.toggle_loop_record`), bound with
  `"verified": false` in `keymaps/protools.json`.
- The held notices for the no-input refusal (1.2) and the no-range refusal
  (6.1), as the operator sees them, not as the model reports them.
- Quick Swipe (4.8) and Cancel during consolidate (6.5), both optional.
- Audible joins between lanes in 5.1: a model test cannot hear a click.

Known gap found while writing this ADR, not fixed here: `docs/comping-a-vocal.md`
says `ctrl+alt+l` shows the take lanes. In `keymaps/protools.json`
`ctrl+alt+l` is `clip.loop`. The checklist uses `ctrl+alt+shift+t`
(`comp.toggle_lanes`), which matches the keymap. The walkthrough needs a
follow-up line to correct it.

## Consequences

- One new doc, `docs/exit-tests/comping.md`, is the human run. It is the
  place the owner or a tester records results. It is not a generated file.
- No source change. The scripted test and the keymap are not altered by this
  decision.
- The backlog line `ui.comping.exit_test` stays open until the human run is
  recorded. A PR that merges this ADR and the checklist is a partial item and
  says so in its title.
