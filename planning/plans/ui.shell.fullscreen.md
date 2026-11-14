# Plan: ui.shell.fullscreen - Qt shell fullscreen, no desktop visible

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08

## 1. Goal and user experience  (architect)
Already delivered: ADR-0010 sections 1-2 (#208). labwc rule maximizes `sonora-ui` undecorated and always-on-bottom; no root menu. Out of scope: any new code.
Acceptance: `grep -n Maximize iso/config/includes.chroot/usr/share/sonora/labwc/rc.xml`.

## 2. Design
N/A - no UI change.

## 3. Contracts and integrations
ADR-0092 (reserved, written by the architect). No protocol or format change.
Reserved numbers: ADR 92, protocol none, format none.
Acceptance: `grep ADR-0092 brain/INDEX.md`.

## 4. DAW core
N/A.

## 5. Plugins, DSP, host
N/A.

## 6. UI
N/A - the compositor, not Qt, makes the window fullscreen (ADR-0010 section 2).

## 7. Tests
N/A - `tools/test-labwc-kiosk.sh` and `tools/test-image-config.sh` exist.

## 8. QA
Unverified: look on real hardware / current ISO. Run `tools/test-boot-qemu.sh` on a fresh ISO with a screenshot (coordinator, release/nightly). Red = new fix task.

## 9. Quality checks
`tools/pre-pr.sh`. Docs only.

## 10. Code standards
`.claude/rules/gates-and-checks.md`.

## 11. Dependencies and order
Grouped: only this item (no other open item in the area is related).
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| close | distro-engineer | S | low | junior | feat/ui.shell.fullscreen | main | brain/decisions/ADR-0092-shell-fullscreen-item-closed-by-208.md, brain/INDEX.md |
Single PR title: Close ui.shell.fullscreen by reference to ADR-0010 (ADR-0092)

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Does it look right on real hardware? | Check on next ISO; fix task if not |
