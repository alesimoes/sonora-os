# Context: design

No gate context (docs and assets). Agent: visual-designer; implementers:
ui-engineer, dsp-engineer.

## Where
- `docs/images` (mockups, scene screenshots), `ui/qt/qml` assets,
  `brain/research/design-*.md`.
- Plugin editors: parameters in `plugins/native/<name>`; editors drawn by
  `sonora-ui` (ADR-0011).

## Bound by
- `.claude/rules/design-faceplates.md`: look of the original hardware,
  redrawn, own names, no brands or logos (owner, 2026-11-06).
- ADR-0006 UI toolkit; keymaps `keymaps/protools.json`, `keymaps/logic.json`.
- Direction: Pro Tools workflow improved, Logic-level classic/modern polish.

## Workflow
- Skill `faceplate-design`: spec table (control, param id, range, unit,
  taper, px, colour tokens) -> ui-engineer builds, dsp-engineer confirms
  ranges.
- Generated screenshots are not committed (owner, 2026-11-13): they conflicted
  between PRs and outran the command cap; `docs/images/sonora-ui-*.png` is ignored.

## Traps
- Licence of fonts and assets must be noted.
- A texture-heavy faceplate costs time in the UI process; keep it vector
  where possible and say what is unmeasured.
