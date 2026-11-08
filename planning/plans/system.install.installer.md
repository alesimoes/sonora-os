# Plan: system.install.installer - closed by reference to #372

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08
Every section not listed is N/A - already delivered on main; docs only.

## 1. Goal and user experience
Backlog says partition, copy, bootloader, operator account, session carry-over. All exist: `sonora-install` 7 steps + `sonora-install-tty` (#353, #372, #398); account is the image's `sonora` user (ADR-0032 s2). Grouped: single item. Deliverable: ADR-0100 recording the closure.
Out of scope: layout/swap (rootfs), Secure Boot, Qt page.
## 3. Contracts
ADR-0100 (reserved). No protocol, no format. brain/INDEX.md line added by the coordinator chore (file held by other features).
## 7. Tests
None new. Existing `tools/test-installer.sh`, `tools/test-installer-tty.sh` cover it; not run by the architect (unverified).
## 8. QA
Optional: run both tests on main; real-hardware install remains unverified (ADR-0032 list, researcher).
## 11. Dependencies and order
| Slice | Agent | Grade | Tier | Files |
|---|---|---|---|---|
| adr | doc writer | S/low | junior | brain/decisions/ADR-0100-installer-item-closed-by-372.md |
Open questions with defaults: none.
