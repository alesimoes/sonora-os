# Plan: drivers.usb.virtual_card - Virtual-card QEMU test

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08

## 1. Goal and user experience  (architect)
Already delivered by #354 (e6f1e033): `tools/test-virtualcard-qemu.sh` and `sonora-virtualcard-test` on main. Out of scope: any new test code.
Acceptance: `git ls-files | grep -i virtualcard` lists both files.

## 2. Design
N/A - no UI.

## 3. Contracts and integrations
ADR-0091 (reserved, written by the architect). No protocol or format change.
Reserved numbers: ADR 91, protocol none, format none.
Acceptance: ADR-0091 file exists; its INDEX line is added by the coordinator at merge (brain/INDEX.md is claimed by plugins.graph.parallel, so this task does not touch it).

## 4. DAW core
N/A - no engine change.

## 5. Plugins, DSP, host
N/A.

## 6. UI
N/A.

## 7. Tests
N/A - the existing QEMU test needs an ISO; release/nightly only (ADR-0061).

## 8. QA
Unverified: run `tools/test-virtualcard-qemu.sh` on a fresh ISO (privileged, coordinator). Red = new fix task.

## 9. Quality checks
`tools/pre-pr.sh` (ADR numbers, whitespace). Docs only.

## 10. Code standards
`.claude/rules/gates-and-checks.md`.

## 11. Dependencies and order
Grouped: only this item (nothing else of the area is open and related).
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| close | distro-engineer | S | low | junior | feat/drivers.usb.virtual_card | main | brain/decisions/ADR-0091-virtual-card-item-closed-by-354.md, brain/INDEX.md |
Single PR title: Close drivers.usb.virtual_card by reference to #354 (ADR-0091)

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Does the test still pass on today's image? | Run on next ISO; open a fix task if red |
