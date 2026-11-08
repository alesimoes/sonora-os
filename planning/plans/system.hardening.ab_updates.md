# Plan: system.hardening.ab_updates - A/B updates of the image

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08
Grouping: one open item, one PR. Delivered already by #398 (see ADR-0097).

## 1. Goal and user experience  (architect)
No user-visible change. Re-verify the mechanism is on main and record closure.
Out of scope: installer slots, session probe, signing (follow-ups).
Acceptance: `git ls-files` lists slots.sh, sonora-update, grub-ab.cfg, tools/test-ab-updates.sh.

## 2. Design  N/A - no UI.
## 3. Contracts and integrations
ADR-0097 (drafted by the architect, committed by the task). No protocol or format.
Reserved numbers: ADR 97.
Acceptance: file exists.
## 4. DAW core  N/A - no engine change.
## 5. Plugins, DSP, host  N/A.
## 6. UI  N/A.
## 7. Tests
None new. `tools/test-ab-updates.sh` and `tools/test-slots.sh` already run in CI; run them once if Linux is available, else list as unverified.
## 8. QA  N/A - docs only.
## 9. Quality checks
Docs only. Do not edit brain/INDEX.md (another feature owns it): put the line in the PR body for the coordinator.
## 10. Code standards
See `.claude/rules/*.md`.
## 11. Dependencies and order
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| close | distro-engineer | S | low | junior | sub/system.hardening.ab_updates/close | feat/system.hardening.ab_updates | brain/decisions/ADR-0097-ab-updates-item-closed-by-398.md |
Single PR title: Close system.hardening.ab_updates by reference to #398 (ADR-0097)

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Who builds sonora-session-open? | New BACKLOG line to the engine/os track; apply stays fail-closed meanwhile |
| Installer slots? | `system.install.rootfs` follow-up |

## 13. Done =
- [ ] ADR-0097 committed, INDEX line in PR body
- [ ] `tools/pre-pr.sh` clean
- [ ] PR body lists the three follow-up gaps
