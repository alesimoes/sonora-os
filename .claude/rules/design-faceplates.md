# Design and plugin faceplates

- Plugin editors look like the original hardware they take after (layout,
  control types, ranges, colour mood), redrawn from scratch. Why: owner,
  2026-11-06.
- Our own names for every product, plugin and preset. No brand, logo, model
  number or artist name in code, assets, strings or docs (ADR-0011).
- Nothing copied or traced from proprietary screenshots or assets; assets are
  original or freely licensed, with the licence noted.
- UI direction: Pro Tools workflow improved, Logic-level modern and classic
  polish.
- Generated screenshots are not committed (owner, 2026-11-13): they conflicted
  between PRs and outran the command cap; `docs/images/sonora-ui-*.png` is ignored.
- Specs flow `visual-designer` -> `ui-engineer` / `dsp-engineer`
  (skill `faceplate-design`).
