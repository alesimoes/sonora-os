# Context: UI

Local CI context: `sonora-ui`. Agent: ui-engineer (look: visual-designer).

## Where
- `ui/qt/src`, `ui/qt/qml`, `ui/qt/tests` (Qt 6 shell), `ui/client`
  (engine link), `ui/keymap`, `ui/tui`; command maps in `keymaps/`.
- Screenshots: `docs/images`.

## Bound by
- ADR-0006: Qt 6, custom GPU scene-graph items for meters/timeline; the UI
  pulls at 60 Hz, the audio thread never pushes.
- ADR-0010: kiosk compositor (labwc); editors are floating windows.
- ADR-0007: UI is a separate process from the engine; talks through
  `contracts/api`.

## Build and test
- `ctest -L ui`; build with `tools/build-slot.sh tools/build-ui.sh` (ccache).
- Gate: `ui/` and `keymaps/` touch only the ui context.

## Traps
- Screen-visible change: regenerate and commit screenshots of every scene it
  touches, open them, list them in the PR.
- Do not own model/session logic; it lives in `common/model`.
- Keep QML/C++ files under ~500 lines (`.claude/rules/code-structure.md`).
