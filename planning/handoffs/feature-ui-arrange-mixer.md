# feature/ui-arrange-mixer

- **Goal:** a working Pro Tools-style Edit and Mix window in `ui/qt` (owner, 2026-10-13: DAW MVP; look per `brain/specs/ui-design.md`).
- **State:** in review (PR open, not merged).
- **Commits:** see `git log origin/main..feature/ui-arrange-mixer`.

## Verify
    tools/build-ui.sh                                   # expected: 100% tests passed, 0 tests failed out of 12
    tools/build-engine.sh                               # expected: 7/7, and "ui/qt: Qt 6.7+ ... not found ... skipped"
    tools/build-ui.sh --screenshot docs/images/sonora-ui-edit.png   # writes -edit, -mix, -edit-no-engine

## Done
- Model seam `TimelineDocument` / `LocalDocument` — `ui/qt/src/timeline_document.h` (swap table in its header comment); types and pure `apply` in `timeline_model.h/.cpp`, names from `timeline-model.md`.
- Edit controller (modes, tools, zoom/scroll, selection, gestures -> resolved ops) — `edit_controller.h`; resolvers `resolve_move` / `resolve_trim` are what moves to engine/control.
- Scene-graph timeline + rulers — `scene_item.h`, drawn from `timeline_scene.h` (Qt-free, pixel-testable).
- Mix window, fader taper — `fader_taper.h`, `qml/ChannelStrip.qml`, `qml/MixWindow.qml`.
- Theme tokens — `theme.h` (single source) + `Theme` QML singleton (`theme_object.h`); spec `brain/specs/ui-design.md`.
- Catalog: `window.toggle_mix_edit` (additive); `protools.json` ctrl+equal, `logic.json` x, both `verified: false`.
- Tests: `ui_edit` (1650+ checks), `ui_shell` (Edit/Mix wiring, Ctrl+=), `ui_render` (region, waveform, cursor, playhead, meter pixels).

## Open
- Swap to `common/model` when feature/timeline-model merges: replace `ui::model` with `sonora::model`, delete timeline_model.*, keep resolvers.
- Shuffle and Spot refuse moves/trims/clear (`edit_controller.cpp` refuse_in_mode); a separate on a range is two undo steps (no compound op in the spec).
- Synthetic waveforms only (`peak_source.h`); real peak files are ui.editing.waveform.
- Mix state changes nothing audible (no playback path); Mix window says so.
- `fonts-inter` is in `tools/Dockerfile.ui` only; the ISO package list needs it (falls back to DejaVu Sans).
- Verify ctrl+equal (Pro Tools) and x (Logic) against the guides, then drop `verified: false`.

## Do not redo
- `final` on QML-registered QQuickItem subclasses: QQmlElement<T> derives from them.
- Forward-declaring SessionModel in a header with a `SessionModel*` Q_PROPERTY: moc needs the full type.

## Pointers
- `brain/specs/timeline-model.md`, `brain/specs/ui-design.md`, ADR-0006 (+amendment), ADR-0007 §5.
