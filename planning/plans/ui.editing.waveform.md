# Plan: ui.editing.waveform - Waveform display with an overview cache

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: given (2026-11-09)
Every section is filled or says `N/A - <reason>`.

## 1. Goal and user experience  (architect)
Closed by reference to PR #471 (commit `3e1eac31`): the engine writes a
multi-resolution min/max `.spk` overview beside each take as it closes; the
Edit window reads it, or builds a missing one on its own worker, never on the
GUI or audio thread. The BACKLOG note ("only the synthetic source exists") is
stale. Grouped items: only this one. Out of scope: the live-take follow-ups
(a)-(e) listed in the record item's BACKLOG text; they belong to that item.
Acceptance:
- `git ls-files` lists `common/audiofile/include/sonora/peak_file.h`,
  `common/audiofile/src/peak_file.cpp`, `ui/qt/src/peak_source.cpp`,
  `common/audiofile/tests/test_peak_file.cpp`, `ui/qt/tests/test_file_peaks.cpp`.
- `grep -n PeakFile ui/qt/src/peak_source.cpp` finds the reader.

## 2. Design  (visual-designer)
N/A - no screen change.

## 3. Contracts and integrations  (architect + owner engineer)
N/A - already on main. Reserved ADR 60 is unusable: ADR-0060 is
`hardware-output-paths`. No ADR is written (nothing new is decided); the file
layout is documented in `peak_file.h`. No protocol or format change.

## 4. DAW core  (rt-engineer)
N/A - done in #471 (`engine/control/src/closed_take.cpp`, control thread).

## 5. Plugins, DSP, host  (dsp-engineer, plugin-engineer)
N/A - none.

## 6. UI  (ui-engineer)
N/A - done in #471 (`ui/qt/src/peak_source.*`).

## 7. Tests  (tester)
N/A - `test_peak_file`, `test_closed_take`, `test_file_peaks` exist. No build
needed; the task only checks the files exist.

## 8. QA  (tester)
N/A - no behaviour change.

## 9. Quality checks
`tools/pre-pr.sh` only. Unverified: disk and CPU cost of building a peak file
for a very long import (researcher measurement if it matters).

## 10. Code standards
`.claude/rules/code-structure.md`, `gates-and-checks.md`.

## 11. Dependencies and order
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| close | ui-engineer | S | low | haiku | sub/ui.editing.waveform/close | feat/ui.editing.waveform | this plan |

Integrator: none (one slice).
Single PR title: Close ui.editing.waveform by reference to #471

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Files missing on main | Stop, report to the architect |
| ADR number 60 reserved but taken | No ADR; lead reserves a free number next time |

## 13. Done =
- [ ] verification greps pass
- [ ] `tools/pre-pr.sh` clean
- [ ] PR body carries the `[x]` line for ui.editing.waveform
