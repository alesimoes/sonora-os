# Plan: architecture.portability.process_setup - P3: process-setup seam (memory lock, quit handlers, session root)

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: given (2026-11-09)
Every section is filled or says `N/A - <reason>`.

## 1. Goal and user experience  (architect)
Closed by reference to PR #412 (commit `cc2d26d7`), which landed ADR-0008 P3
in `common/platform` (not `engine/app`, as the BACKLOG text says). No user
change. Grouped items: only this one (the other P-items are different areas
and files). Out of scope: any code, P5 to P10.
Acceptance:
- `git ls-files` lists `common/platform/include/sonora/platform/process_setup.h`,
  `common/platform/src/posix/process_setup.cpp`, `common/platform/src/unsupported/process_setup.cpp`
  and the tests `test_process_setup.cpp`, `test_memory_lock.cpp`, `test_quit_signals.cpp`.
- `grep -rnE 'mlockall|sigaction' engine/app/src` finds nothing.

## 2. Design  (visual-designer)
N/A - no screens.

## 3. Contracts and integrations  (architect + owner engineer)
N/A - no header, wire or format change. Reserved: ADR 133
(`brain/decisions/ADR-0133-process-setup-closed-by-412.md`); protocol none.

## 4. DAW core  (rt-engineer)
N/A - no code change; the seam runs at start-up, not on the audio thread.

## 5. Plugins, DSP, host  (dsp-engineer, plugin-engineer)
N/A - none.

## 6. UI  (ui-engineer)
N/A - nothing on screen.

## 7. Tests  (tester)
N/A - existing tests are the evidence; this PR changes only `brain/` and `planning/`.

## 8. QA  (tester)
N/A - no scenario, no screenshots.

## 9. Quality checks
`tools/pre-pr.sh` clean (ADR number 133, no BACKLOG edit). Files under 500 lines.

## 10. Code standards
N/A - documentation only.

## 11. Dependencies and order

| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| close-by-reference | distro-engineer | S | low | haiku | `sub/architecture.portability.process_setup/close` | `origin/main` | `brain/decisions/ADR-0133-process-setup-closed-by-412.md`, `planning/plans/architecture.portability.process_setup.md` |

Single PR: `architecture.portability.process_setup: closed by reference to #412 (ADR-0133)`.
The PR body carries the `[x]` BACKLOG line (noting the seam lives in
`common/platform`) and the INDEX line for ADR-0133; neither file is edited.

## 12. Risks and open questions
| Question | Default |
|---|---|
| Should the seam move to `engine/app`? | No; `common/platform` is the right module (ADR-0007 DAG). |

## 13. Done =
- [ ] ADR-0133 committed with this plan
- [ ] the `git ls-files` and grep checks pass (output in the report)
