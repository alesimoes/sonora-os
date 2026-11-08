# Plan: system.hardening.crash_recovery - Session recovered intact after power pulled mid-take, on hardware

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08
Grouping: one open item, one PR. Software half already on main (#359, tools/crash-recovery-kill.py); this PR adds the hardware protocol and script.

## 1. Goal and user experience  (architect)
No UI. A person with an installed unit can pull the power mid-take and get a PASS/FAIL per take.
Out of scope: fault injection in CI (dm-flakey), engine changes, fsync policy changes.
Acceptance: ADR and script merged; the item is NOT ticked until a hardware run is reported.

## 2. Design  N/A - no UI.
## 3. Contracts and integrations
ADR-0102 (reserved). No protocol or format. Pass criteria and witness-on-another-device rule are in the ADR.
## 4. DAW core  N/A - no engine change; if the hardware run fails, a new item is raised.
## 5. Plugins, DSP, host  N/A.
## 6. UI  N/A.
## 7. Tests
`tools/crash-recovery-hw.sh --self-test` (fake backend, SIGKILL) proves the script on Linux. Not added to ci.yml (it would duplicate crash-recovery-kill.py).
## 8. QA
The hardware run itself: owner or hardware tester, procedure in the ADR. Result reported in the PR or a handoff line.
## 9. Quality checks
shellcheck, exec bit, `tools/pre-pr.sh`. Do not edit brain/INDEX.md: put the line in the PR body.
## 10. Code standards
See `.claude/rules/*.md`.
## 11. Dependencies and order
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| adr | distro-engineer | S | low | junior | sub/system.hardening.crash_recovery/adr | feat/system.hardening.crash_recovery | brain/decisions/ADR-0102-crash-recovery-software-half-closed-hardware-protocol.md |
| hwscript | distro-engineer | M | med | pleno | sub/system.hardening.crash_recovery/hwscript | feat/system.hardening.crash_recovery | tools/crash-recovery-hw.sh |
Single PR title: crash recovery: hardware power-pull protocol and script (ADR-0102) - partial, hardware run pending

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Acceptable loss window on a stick? | Take the figure from session-format.md; if the hardware run exceeds it, raise an item, do not widen it |
| Who runs the hardware test? | Owner; waits on the owner in the handoff, work moves on |
| Fault injection in CI? | Separate future item (privileged, dm-flakey) |

## 13. Done =
- [ ] ADR-0102 committed, INDEX line in PR body
- [ ] script self-test output quoted in the PR (or listed unverified)
- [ ] `tools/pre-pr.sh` clean
- [ ] item left unticked; PR body says hardware run pending
