# Plan: system.install.updates - Updates on an installed system

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08

## 1. Goal and user experience
The operator updates an installed machine only when they ask, never during a session, never the kernel by accident. ADR-0035 policy (Debian stable pins, kernel hold, apt-daily conditioned off) is already on main and checked by `tools/test-updates.sh`; ADR-0044's `sonora-update` (A/B, #398) is the updater. What is missing is `sonora-session-open`, so `apply` always refuses.
Grouping: single open item; the probe, its test and the ADR are one PR.
Out of scope: apt updater (rejected), installer two-slot layout (rootfs), release key/publisher, the app-side writer.
Acceptance: `bash tools/test-session-open.sh`, `tools/test-updates.sh`, `tools/test-ab-updates.sh` exit 0.

## 2. Design
N/A - no UI.
## 3. Contracts and integrations
ADR-0101 (reserved), no protocol, no format. Contract: `/usr/local/bin/sonora-session-open`, exit 0 open, 1 none, 2 cannot tell; state file `/run/sonora/session-open`. Brain/INDEX line added by the coordinator chore.
## 4. DAW core
N/A in this PR - the app writing the state file is a follow-up BACKLOG line for the DAW track (apply stays fail closed meanwhile).
## 5. Plugins, DSP, host
N/A - none.
## 6. UI
N/A - none.
## 7. Tests
New `tools/test-session-open.sh` (no Docker, fake systemctl). Not run by the architect.
## 8. QA
Real installed update never run: unverified (researcher: QEMU plan in ADR-0044).
## 11. Dependencies and order
| Slice | Agent | Grade | Tier | Files |
|---|---|---|---|---|
| probe | distro-engineer | S/high | senior | sonora-session-open, tools/test-session-open.sh |
| adr | distro-engineer | S/low | junior | ADR-0101 |
Order: probe, adr. PR is partial (title says so).
Open questions with defaults: state file under /run (default) vs /var/lib - /run, a stale file cannot survive a reboot.
