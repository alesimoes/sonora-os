# Plan: architecture.portability.session_fs - P1: session folder creation and take scan on std::filesystem

Status: draft   (approved only by architect sign-off, below)
Written by: engineer (eng-146) for the architect   Dispatched by: lead   Architect sign-off: pending
Every section is filled or says `N/A - <reason>`.

## 1. Goal and user experience  (architect)
The item is closed by reference to PR #412 (ADR-0121), which already landed
ADR-0008 P1 and P2 on main (commit `cc2d26d7`). The user sees no change: a
session folder is created and the next take number is found the same way as
before, now without throwing overloads.
Flow: none new. Out of scope: any new code, the POSIX tests under
`engine/core/tests/posix/`, and P3 to P10.
Acceptance:
- `git ls-files` lists `engine/core/tests/test_session_dirs.cpp` and
  `common/audiofile/tests/posix/test_wav_writer_full_disk.cpp`.
- `grep -rnE 'mkdir|opendir|sys/' engine/core/src engine/core/tests` finds
  nothing in `engine/core/src` and nothing at the top level of
  `engine/core/tests` (the three hits under `engine/core/tests/posix/` are
  out of scope).

## 2. Design  (visual-designer)
N/A - no screens, faceplates or assets change.

## 3. Contracts and integrations  (architect + owner engineer)
N/A - no header, wire, protocol or format change. Reserved numbers: ADR 121
(`brain/decisions/ADR-0121-session-fs-item-closed-by-412.md`); protocol none.

## 4. DAW core  (rt-engineer)
N/A - no change to `engine/core/src`. `make_dir` and `next_free_take` are on
main already and are not on the audio thread (`Session::open` and take
start), so the RT rules are not touched.

## 5. Plugins, DSP, host  (dsp-engineer, plugin-engineer)
N/A - no processor or host change.

## 6. UI  (ui-engineer)
N/A - nothing on screen changes.

## 7. Tests  (tester)
N/A - no new test. The existing tests (`session_dirs` in `engine/core`,
`wav_writer_full_disk` under `if(UNIX)` in `common/audiofile`) are the
evidence and run on the PR gate when their files change; this PR changes
only `brain/` and `planning/`.

## 8. QA  (tester)
N/A - no end-to-end scenario is new; no screenshots (no UI change).

## 9. Quality checks
- `tools/pre-pr.sh` clean (whitespace, exec bits, shellcheck, ADR number 121
  against main and open PRs, no BACKLOG edit, protocol reserved none).
- File size: the ADR and this plan are under 500 lines.
- No RT or sanitizer check is affected; nothing in the build changes.

## 10. Code standards
N/A - documentation only. Links: `.claude/rules/gates-and-checks.md` (a
check not run is unverified), `.claude/rules/planning.md`.

## 11. Dependencies and order
#412 is already on main, so no contract PR is needed. One slice, one PR.

| Slice | Agent | Difficulty (S/M/L) | Risk (low/med/high) | Tier (haiku/sonnet/opus) | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| close-by-reference (ADR-0121, this plan) | engineer (eng-146) | S | low | haiku | `sub/architecture.portability.session_fs/close` | `origin/main` (`ea0b4932`) | `brain/decisions/ADR-0121-session-fs-item-closed-by-412.md`, `planning/plans/architecture.portability.session_fs.md` |

Integrator: N/A - single slice.
Single PR title: `architecture.portability.session_fs: closed by reference to #412 (ADR-0121)`.
PR body carries the BACKLOG `[x]` line for the item and the one-line
`brain/INDEX.md` entry for ADR-0121; neither file is edited by this PR.

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Is the three-file `tests/posix/` POSIX include set a P1 leftover? | No. ADR-0008 places platform tests in `tests/posix/`; relocating them is a later step (P5/P9). |
| Does closing by reference need a code test? | No. The existing tests named in section 1 are the acceptance and already run on the gate. |
| Architect sign-off not yet given | Lead may dispatch on the recommended defaults above; the architect confirms on review. |

## 13. Done =
- [ ] ADR-0121 committed with this plan
- [ ] `git ls-files` and the grep in section 1 pass (output in the report)
- [ ] `tools/pre-pr.sh` clean
- [ ] BACKLOG `[x]` line and INDEX entry in the PR body (not in the diff)
- [ ] no file past ~500 lines
