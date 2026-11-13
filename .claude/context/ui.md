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
- The slice's check is the micro build. With the wait for the lock a UI run
  can pass the 10-minute tool limit, so run it detached and collect it:
  `tools/build-slot.sh --detach tools/micro-build.sh --image ui --no-rtsan`,
  then `tools/build-wait.sh` (exit 75: still running, call it again; any
  other exit is the build's). `--files-from F` with the slice's own paths
  keeps it to the targets they reach. Lines `micro-build: phase ...` say where
  the time went (lock-queue, volumes, configure, build, test).
- Gate: `ui/` and `keymaps/` touch only the ui context.

## Traps
- Screen-visible change: the daemon regenerates and commits every picture
  under `docs/images` before the feature's PR opens (`tools/harness/shots.py`;
  the full run outlives a 10-minute tool call). Commit code and the scene
  only. Check your scene renders by shooting it alone from your build tree
  (`sonora-ui --fake-engine --scene NAME --screenshot ...`) and open it; a
  missing picture is never a reason to block the slice.
- Do not own model/session logic; it lives in `common/model`.
- Keep QML/C++ files under ~500 lines (`.claude/rules/code-structure.md`).
