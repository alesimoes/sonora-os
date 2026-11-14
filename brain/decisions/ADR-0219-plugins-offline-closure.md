# ADR-0219 — plugins.offline closure record

- Status: Accepted (closure record; decides no new behaviour)
- Date: 2026-11-14
- Progress item: `plugins.offline` (Pro Tools: AudioSuite; Logic Pro:
  selection-based processing)
- Depends on: ADR-0087 (Process Selection through one chosen plug-in, with
  its 2026-11-13 amendment and its 2026-11-14 closure note), ADR-0004
  (plug-ins out of process), ADR-0012 (hosting; any miss fails a render),
  ADR-0051 (render in place)
- Amends: nothing

## Summary

*Navigation only; the record below is the evidence.*

- `plugins.offline` is delivered on `main`: Process Selection through one
  chosen insert, `track.process_selection insert=<id>`, rendered on the
  engine's render task, wired into the control server and offered in the
  Qt editor as `Track > Process Selection With`.
- Each part below is quoted from `git log origin/main -S`, taken on
  2026-11-14 against `origin/main` at `06e234cc`. The search is by commit,
  so it shows where a string entered `main`, not where it is now.
- One item stays open, and it belongs to the researcher, not to the
  feature: the worker-thread render speed above 1x real time (ADR-0087
  §Unverified). It does not block closure.

## Evidence (git log origin/main -S)

The commits that carry the feature: the plan, the engine, the server.

- `54af7493` `[partial] plugins.offline: ... (#625)`: the engine
  (`RenderInPlace`, `PluginHosting`), the contract and the Qt scene.
- `89ca1b44` `plugins.offline: wire Process Selection With into the control
  server; amend ADR-0087 (#707)`: the server wiring and the amendment.
- `daaa939e` `Architect plans and ADRs 0089/0090/0092 ...(#554)`: adds
  `planning/plans/plugins.offline.md`, the plan that first names
  `only_insert`. It is a plan, not code; the code arrived in `54af7493`.

Per part:

- **`only_insert`** (the one-insert rule, in the model):
  `git log origin/main --oneline -S only_insert` returns `89ca1b44`,
  `54af7493`, `daaa939e`. The rule is in
  `common/model/include/sonora/model/render_in_place.h` (line 67, "Whether
  `insert` can be the one insert a Process Selection runs through") and
  `common/model/src/render_in_place.cpp` (line 166, "only Process Selection
  runs through one plug-in").
- **`insert=`** (the argument on the wire):
  `git log origin/main --oneline -S 'insert='` returns `89ca1b44`,
  `54af7493`, `daaa939e`. It is read in
  `contracts/api/include/sonora/api/render.h` (line 30, "insert=<id>
  (ADR-0087) is taken by track.process_selection only"; line 113, "insert=
  applies only to Process Selection; freeze and commit print the whole
  chain").
- **The capability `track.render.insert`**:
  `git log origin/main --oneline -S track.render.insert` returns `89ca1b44`,
  `54af7493`, `daaa939e`. It is announced in Hello after `track.render`
  (ADR-0087 §Amendment 2026-11-13, "Hello announces
  `track.render.insert` after `track.render`"). The constant is
  `kCapabilityRenderInsert`.
- **The UI entry** (`Track > Process Selection With`):
  `git log origin/main --oneline -S 'Process With'` returns nothing, because
  the entry's label is `Process Selection With ▸` and the scene is named
  `process_with`. `git log origin/main --oneline -S process_with` returns
  `89ca1b44` and `54af7493`. On `main` the entry is
  `ui/qt/qml/Main.qml` line 1598 (`label: "Process Selection With ▸"`,
  `command: "track.process_selection"`, `render: "with"`) and the scene is
  `ui/qt/src/scenes/process_with.cpp`, added by `54af7493`.
- **The server wiring**: `git log origin/main --oneline -S
  track.render.insert` and `-S 'Process Selection With'` return `89ca1b44`
  (the commit that changed `engine/control/src/engine_control_server.cpp`,
  "wire Process Selection With into the control server"). The command-aware
  parse, `parse_render_argument(command, arg, ...)`, is in that file;
  `track.freeze` and `track.commit` refuse `insert=` with the parse's reason.
- **Tests on `main`**: `engine/control/tests/test_control_process_with.cpp`
  (added by `89ca1b44`) registers `control_process_with` on every build and
  `control_process_with_host` where `sonora-plugin-host` is built, as
  `engine/control/CMakeLists.txt` lines 207-212 and 326-330 show.

## Decision

`plugins.offline` is closed. The feature is complete on `main` for what
was in scope in ADR-0087 §1-§4 and its amendment. Closure does not turn the
Unverified items into passing checks; they stay listed in ADR-0087 and are
handed to the researcher.

## Open, for the researcher (does not block closure)

- Worker-thread render speed above 1x real time, with real LV2 and VST3
  hosts on the target machine (ADR-0087 §Unverified, first item). A yes
  reopens faster-than-real-time Process; a no closes it.
- The override path on real out-of-process hosts (ADR-0087 §Unverified,
  second item) and soft-bypass leakage (third item).

## Checks not run by this record

This record changes documentation only. It did not build or run the tests
above. The test results on `main` are the nightly's, not quoted here.
