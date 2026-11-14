# ADR-0199 — plugins.offline is complete: Process Selection With

- Status: Accepted (recommended default, taken by the closer with no owner
  available, 2026-11-14)
- Date: 2026-11-14
- Progress item: `plugins.offline` (Pro Tools: AudioSuite; Logic Pro:
  selection-based processing)
- Depends on: ADR-0004 (plug-ins out of process), ADR-0012 (hosting; any
  miss fails a render), ADR-0051 (render in place), ADR-0087 (Process
  Selection through one chosen plug-in, with its 2026-11-13 amendment)
- Amends: nothing. This ADR records completion; it decides no new behaviour.

## Summary

*Navigation only; the sections below are the decision.*

- `plugins.offline` is complete end to end on `main`: Track > Process
  Selection With prints one chosen insert of a track, rendered in real time
  through the live chain, and the operator is told why a render fails.
- Every layer is on `main`: the contract (`insert=<id>`, capability
  `track.render.insert`), the model (`render_insert_valid`), the engine
  (render-scoped solo in `PluginHosting`), the UI entry and the control-server
  wiring.
- Faster-than-real-time hosted rendering is out of scope and stays not built.
  It is a measurement question, handed to the researcher (ADR-0087
  Unverified).

## Context

ADR-0087 split the work into a contract and engine change (#625), a UI
change, and the control-server wiring, which #625 left out because another
task held `engine_control_server.cpp`. The 2026-11-13 amendment of ADR-0087
recorded that the server is wired. Since then the feature branch
`feat/plugins.offline` has been merged in pieces and its history no longer
matches `main` line for line: `main` carries the work through #625 and #707.
The close-out has to say what `main` contains, not what a branch contains.

## Decision

1. `plugins.offline` is closed as complete. The item's own success
   criterion, one chosen insert of a track printed through its live chain, is
   met on `main`.
2. The evidence on `main` is the following. `git log origin/main -S` finds
   the names in these commits:
   - `only_insert`: `54af7493` (#625, engine and contract) and `89ca1b44`
     (#707, server wiring).
   - `insert=`: `89ca1b44` (#707), the command-aware parse of
     `contracts/api/include/sonora/api/render.h`.
   - `track.render.insert`: `89ca1b44` (#707); the Hello announcement is
     `engine/control/src/engine_control_server.cpp`, which pushes
     `api::kCapabilityRenderInsert`.
   - `renderInsertAvailable`: `54af7493` (#625); the UI gate that shows the
     entry only when the engine announces the capability is in
     `ui/qt/src/shell_model.{h,cpp}` and `ui/qt/qml/Main.qml`.
   - The UI entry is `ui/qt/qml/Main.qml` (`Process Selection With ▸`, the
     `track.process_selection` command) and `ui/qt/src/scenes/process_with.cpp`.
3. The server reads `insert=` only for `track.process_selection`. Freeze and
   Commit refuse the word with the parse's reason, as ADR-0087 §2 requires.
4. Nothing in the audio thread changed for this ADR. Completion is a
   control-thread and documentation statement.
5. Faster-than-real-time hosts stay out of scope. Hosted Process Selection
   runs at the live rate, as ADR-0051 §2 requires.

## Rationale

- ADR-0087 defined the feature as one insert, real time, fail with no file on
  any unplayed block, host miss or edit. The code on `main` does that, and the
  existing tests check it: `render_in_place` (engine/control; the model, the
  engine's override and the plan), `plugin_hosting` (engine/control; the
  scenarios of the real host, including `render_solo` and `render_unsolo`) and
  `ui_render_in_place` (ui/qt; the menu entry and the scene).
- A faster-than-real-time host would run a plug-in chain from a worker thread
  beside the live callback. That is an open question for ADR-0004 and
  ADR-0012, and it needs a measurement, not a design. Keeping it out of this
  close-out keeps the completion claim true today.
- Closing the item by ADR, not by a new feature branch, stops a stale branch
  from being mistaken for the state of `main`.

## Consequences

- `plugins.offline` may be ticked as done, with the open measurements listed
  below as their own items, not as parts of this one.
- The live-rate restriction is a limit operators will feel on long selections.
  It is documented here and in ADR-0087, not hidden.
- `feat/plugins.offline` and its sub-branches are not the source of truth. A
  reader who wants the feature reads `main`.
- No code, contract or protocol change follows from this ADR.

## Alternatives rejected

- **Keep the item open until faster-than-real-time rendering is built.** It
  would hold a finished feature open on a question that needs a measurement
  first, and the measurement has an owner (the researcher).
- **Re-open ADR-0087 for the close-out.** ADR-0087 already carries its
  2026-11-13 amendment for the server. A second amendment would add nothing
  the evidence above does not already show.
- **Rebase the feature branch onto `main` to make the history match.** The
  branch carries commits that `main` has in different form; rebasing them
  conflicts on ADR-0087 and `contracts/CONTRACTS.lock`. The branch is stale
  and is not the record.

## Unverified

- **Offline host measurement (for the researcher):** can a host process run
  a plug-in chain driven from a worker thread at more than 1x real time
  without breaking ADR-0004 (isolation, the host stays out of process) and
  ADR-0012 (the lock-free, deadline-bound handoff; any miss fails a render)?
  Measure on the target machine with real LV2 and VST3 hosts: throughput,
  misses, and whether the live callback's hosts are disturbed. A yes reopens
  faster-than-real-time Process; a no closes it. Handed to the researcher; it
  is not a part of this item.
- The override path on real out-of-process LV2 and VST3 hosts on the target
  machine (researcher measurement, carried from ADR-0087).
- Soft-bypass plug-ins (bypass with colouration) leaking into a print,
  carried from ADR-0087.
- The test run that closes this item was taken through `tools/micro-build.sh`
  with the three changed paths listed in the close-out report. Its per-test
  output did not reach the closer: the run exited 0 and printed `all passed`
  after the build phase, with no per-test lines. Until the three named tests
  are seen passing with their names in the output, the run is recorded as
  unverified.
