# Plan: system.install.adr - Installer ADR: Debian's, Calamares or our own

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08

## 1. Goal and user experience
Decide the installer. Already decided: ADR-0032 (Accepted 2026-11-04), our own
thin `sonora-install`; Debian Installer and Calamares rejected. Item is stale.
Acceptance: `grep -n system.install.adr brain/decisions/ADR-0032*.md`.

## 2. Design
N/A - no UI.
## 3. Contracts and integrations
ADR: none new (reserved 89 left unused). Protocol/format: none.
## 4. DAW core
N/A - no engine work.
## 5. Plugins, DSP, host
N/A - none.
## 6. UI
N/A - the installer screens belong to `system.install.installer`.
## 7. Tests
N/A - docs only.
## 8. QA
N/A - docs only.
## 9. Quality checks
`tools/pre-pr.sh`. Unverified: hardware boot (listed in ADR-0032 Unverified).
## 10. Code standards
`.claude/rules/*`.
## 11. Dependencies and order
Grouping: single item. One docs slice.
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| close | distro-engineer | S | low | haiku | feat/system.install.adr | main | brain/decisions/ADR-0032-installer-our-own-thin-installer.md |
