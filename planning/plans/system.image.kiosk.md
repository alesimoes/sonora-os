# Plan: system.image.kiosk - Kiosk compositor for the Qt shell, plus its ADR

Status: closed (2026-11-14, ADR-0186): smoketest and static check on main via #613; default flip parked for KVM
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08

## 1. Goal and user experience
ADR-0010 (labwc kiosk, Qt on Wayland) is already on main behind `sonora.gui`
(#208, #217, #241, #285, #306, #314, #398). Left: a boot where the kiosk
silently did not start must fail the smoketest, and turning the kiosk on by
default needs a written gate. Out of scope: the default flip itself, the
sd_notify watchdog (ui), plugin editors under labwc (researcher), pinning the
labwc test base image (follow-up), and files of `system.hardening.ro_root`,
`ui.shell.fullscreen`, `quality.ci.boot`.
Grouping: single open item; ADR, smoketest and README note are one PR.
Acceptance: `bash tools/test-image-config.sh` passes; shellcheck clean.

## 2. Design
N/A - no UI.

## 3. Contracts and integrations
ADR: ADR-0118 (written, indexed). Protocol/format: none.
Reserved numbers: ADR 118, protocol none, formats none.
Contract = ADR-0118 decisions 1-3 (smoketest outcomes, default-flip gate).
Acceptance: ADR present; INDEX line present.

## 4. DAW core
N/A - no engine work.

## 5. Plugins, DSP, host
N/A - none.

## 6. UI
N/A - none.

## 7. Tests
Static: `tools/test-image-config.sh` extended. Dynamic: `tools/test-boot-qemu.sh
--gui` and `--gui-broken` already exist; running them needs KVM and is
UNVERIFIED here. Do not edit `tools/test-boot-qemu.sh`.

## 8. QA
On a KVM host: `--gui` (first-frame time, ISO size, kill -9 of UI and labwc
recover in 3 s) and `--gui-broken` (fallback, `sonora-session` active on
tty1). Record numbers in the later flip PR.

## 9. Quality checks
`tools/pre-pr.sh`; shellcheck. Review focus: a missing unit must fail, not skip.

## 10. Code standards
`.claude/rules/*`; smoketest is 411 lines, split gui checks into a helper if
it passes ~500.

## 11. Dependencies and order
smoketest -> docs. Single partial PR (KVM-gated evidence); title says so.
Grades: smoketest M/high -> senior; docs S/low -> junior.
Open questions (defaults): none; the flip waits for the KVM run.

## 12. Closure (ADR-0186)
Closed 2026-11-14. Done on main: `sonora-smoketest` asserts the kiosk runs
under `sonora.gui` and the fallback under `sonora.expect_fallback`
(`git log origin/main -S unit_running` -> #613, b7aebf69);
`tools/test-image-config.sh` keeps the checks; `iso/README` documents the
opt-in kiosk and the default-flip gate. Parked, not closed here: the default
flip needs a recorded KVM `--gui` and `--gui-broken` run (owner, 2026-11-13).
