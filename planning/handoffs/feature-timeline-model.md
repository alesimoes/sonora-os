# feature/timeline-model

- **Goal:** `common/json` and `common/model` (ADR-0009, ADR-0007 M8) as pure
  libraries with tests; `ui/keymap` onto `common/json` (BACKLOG: keymap
  strtod under de_DE).
- **State:** in review.
- **Commits:** see `git log origin/main..feature/timeline-model`.

## Verify
    tools/build-engine.sh    # expected: 100% tests passed, 0 tests failed out of 13

The shared `sonora-build:trixie` tag can be re-tagged by another worktree
from main's Dockerfile (no locales), and then the de_DE tests print SKIP.
`docker build -t sonora-build:trixie -f tools/Dockerfile.build tools` first.

## Done
- common/json: strict parser + canonical writer, from_chars numbers, caps —
  evidence: `json` ctest
- ui/keymap parses through common/json; de_DE regression — evidence:
  `keymap_locale` ctest failed on the strtod parser, passes now; `keymap` unchanged
- common/model: snapshots, apply(), History with byte budget and floor,
  session codec v1 with migration hook — evidence: `model_apply`,
  `model_history`, `model_session_file`, `model_random_edits` ctests
- ASan/UBSan and TSan over the whole tree: 13/13 (TSan needs
  `setarch -R` under `--security-opt seccomp=unconfined` on this host)

## Open
- Plan builder and engine/control wiring (Document thread, saver,
  write_file_durably) — later task, not this branch.
- Undo memory measurement (ADR-0009 Unverified 3) not done.

## Pointers
- brain/specs/timeline-model.md, brain/specs/session-format.md, ADR-0009
