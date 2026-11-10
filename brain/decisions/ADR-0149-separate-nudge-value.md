# ADR-0149 - A nudge value separate from the grid

- Status: Accepted
- Date: 2026-11-09
- Item: `ui.editing.grid_nudge`
- Relates to: ADR-0004 (workflow, not binaries), ADR-0007 section 1 (UI never depends on engine)

## Context

Nudge today moves a region by one grid step: `nudge_target` in
`ui/qt/src/time_format.h` is tempo-map aware and follows the grid setting.
An engineer often wants a grid of one bar and a nudge of a few milliseconds
(or the reverse). With one value for both, they must change the grid, nudge,
and change it back, and the grid no longer says what the snap is.

## Decision

- A UI-only `NudgeValue`: `{FollowGrid, Bar, Beat, Eighth, Sixteenth, Ms100,
  Ms10, Ms1}`. The default is `FollowGrid`, which is today's behaviour.
- Musical values reuse `nudge_target` (tempo-map aware).
- Millisecond values are a fixed distance of `max(1, round(rate * ms / 1000))`
  frames, clamped at frame 0.
- The value is not saved in the session, like the grid: no protocol, session
  or window format change.
- Command `nudge.value.next` cycles the value. It has no default key in
  `keymaps/*.json`; a profile may bind it.
- The toolbar gets a NUDGE button beside GRID, showing the current value.

## Rationale

The value is a view preference, so it lives where the grid lives and costs no
wire or file change. Reusing `nudge_target` keeps musical nudges correct
under tempo changes; a millisecond nudge must be absolute time, so it ignores
the tempo map. Minimum one frame means a nudge never silently does nothing.
Defaulting to `FollowGrid` changes nothing for current users.

## Consequences

- No protocol bump, no session or window-state migration.
- The value resets when the application starts and when a session opens.
- A new command name becomes a stable identifier.
- The nudge value is not restored after a restart; accepted, as for the grid.

## Alternatives rejected

- **Unit-switchable nudge (timecode, feet+frames) as Pro Tools has.**
  Milliseconds plus musical values cover the use; more units are more state
  to show and test.
- **Saving the value in the session.** It would need a session format change
  and a version bump for a preference, and it would travel to other machines.

## Implementation split

Decision only now; the code is the follow-up feature, after these merge
(they touch the same files): `ui.editing.commands` (`edit_controller.h`),
`time.tempo_map` (`session_model.h`, `EditWindow.qml`), `ui.mixer.sidechain`
(`ui/qt/CMakeLists.txt`), `ui.record.punch_loop` (`ui/qt/tests/CMakeLists.txt`).

1. `nudge_value.h/.cpp` and its tests (value to frames, cycling, clamp at 0).
2. `EditController` accessors for the value and the cycle command.
3. `SessionModel` property exposing the value to QML.
4. `EditWindow.qml` NUDGE button beside GRID.
5. Screenshots of the scenes the toolbar touches, under `docs/images`.
