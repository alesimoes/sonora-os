# Handoff: feature/qt-shell

**Goal.** The first Qt 6 interface, within ADR-0007 §10 scope: shell for 18 tracks, transport status, footer, scene-graph meters, track arming, and key routing through ui/keymap. A separate process (§3), and no timeline.
**State.** Done and verified locally. Not pushed and not reviewed. Branch is rebased on origin/main 1031fa1.
**Commits.** One feature commit on top of 1031fa1 (`git log origin/main..feature/qt-shell`).

**Verify.**
- `tools/build-ui.sh` should end with `100% tests passed, 0 tests failed out of 9` (6 engine tests plus ui_meter_model, ui_keys, ui_shell, on the offscreen platform).
- `tools/build-engine.sh` should print `ui/qt: Qt 6 (Core Gui Qml Quick Test) not found; sonora-ui and its tests are skipped` and then `100% tests passed, 0 tests failed out of 6`, with 0 warnings.
- `tools/build-ui.sh --screenshot docs/images/sonora-ui-shell.png` writes that file and `-no-engine.png`, captured by grim under cage.
- `tools/build-ui.sh --measure 20` runs 4 measurements of 20 s each.

**Done, with evidence.**
- The seam is `ui/qt/src/engine_adapter.h`, `sonora::ui::EngineAdapter`. It is a subset of ADR-0007 §4.1 under the same names, and its header maps each piece to what replaces it. Only ShellModel and main.cpp hold an adapter.
- The stand-in behind the seam is `SyntheticEngine`, deleted at M4. Key tests use `tests/recording_adapter.h`, not the stand-in.
- Meters are `MeterItem` (QQuickItem plus QSGGeometryNode, one vertex-colour material). The maths, with no Qt in it, is in `meter_model.*`: dBFS scale, instant attack, IEC release, 2 s hold, latched over, no-data state. A single 60 Hz GUI tick in ShellModel pulls them.
- Keys go through `KeyRouter` to ui/keymap `KeyMap`/`ensure_essentials` and the stock profiles. `qt_keys.*` only decodes Qt chords.
- A refused profile does not end the process. The UI runs on the built-in keys and names the file and the reason in the footer for as long as it runs.
- Rendering path is detected at start-up. On a CPU rasteriser it is shown in red and meters drop to 30 Hz. The software scene graph shows that meters are unavailable.
- When the engine dies, every meter shows NO DATA and the take, xrun and overrun fields show `--`. The no-engine screenshot shows this.
- Render cost is in `brain/research/qt-shell-render-cost.md` (llvmpipe only).
- ADR-0007 §1 holds: only ui/qt includes Qt, and ui/qt includes nothing from engine/.

**Open.**
- No GPU measurement. The cost figure needs the appliance, with the engine's xrun counters beside it.
- At M4, replace EngineAdapter and SyntheticEngine with api::EngineControl, StateView and FakeEngine. Command domains (§5) then replace the router's local-versus-engine split.
- There is no compositor in the ISO and no systemd unit for sonora-ui. Choosing a compositor is an ADR for architect (ADR-0006 leaves it open; see the cage X11 finding in adr-0007-measurements §6).
- Whether `app.quit` should close only the UI (current behaviour) is undecided.
- The CI runner has no Qt, so ui/qt is skipped there. No CI job builds the UI.

**Do not redo.** Software scene graph meters: they cannot draw custom geometry, which is by design. Overlap of the health text in the header: fixed with elision.

**Pointers.** `ui/qt/CMakeLists.txt`, `ui/qt/src/{engine_adapter.h,shell_model.*,meter_item.*,key_router.*}`, `ui/qt/qml/Main.qml`, `tools/build-ui.sh`, `tools/Dockerfile.ui`, `docs/images/sonora-ui-shell*.png`.
