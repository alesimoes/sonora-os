# Plan: system.install.secure_boot - UEFI entry and Secure Boot

Status: approved
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-08
Every section not listed below is N/A - OS packaging item, no design/DAW/plugin/UI work.

## 1. Goal and user experience  (architect)
Already on main (ADR-0034, PR 356/398): shim-signed + signed GRUB packages, `grub-install --uefi-secure-boot` for NVRAM and `--removable`, installer self-check that the ESP holds shim, GRUB and the removable loader. What is missing is a way to *verify at runtime* that the installed disk really boots under Secure Boot, and a QEMU test of it. Delivered: `sonora-secureboot-check` and an OVMF test. Secure Boot off stays a supported fallback (ADR-0034 s7), so the check fails only on a missing ESP file, or with `--require-on`.
Out of scope: own keys, custom kernel signing, MOK automation, TPM, LUKS (ADR-0034 s8); editing `sonora-install` (held by system.install.rootfs).
Grouped: only this one backlog item; its two slices are one PR.
Acceptance: see section 11 task accepts.

## 2. Design: N/A - no UI.
## 3. Contracts and integrations
ADR-0099 (reserved) amends ADR-0034; no protocol or format. Contract = output keys and exit codes of `sonora-secureboot-check` (exit 0/3/4), fake-root test. Merged first in the same feature branch.
## 4. DAW core: N/A - no engine change, nothing on the RT path.
## 5. Plugins: N/A.
## 6. UI: N/A.
## 7. Tests
`tools/test-secureboot-check.sh` (no Docker); `tools/test-secureboot-qemu.sh` (privileged, OVMF secboot). ctest label: none (shell tests); `tools/pre-pr.sh` run before push.
## 8. QA
Boot with Secure Boot on and enrolled MS keys; boot with NVRAM cleared; Secure Boot off boots the same files.
## 9. Quality checks
shellcheck as CI runs it, exec bits 100755, files well under 500 lines. Reviewer focus: the check never writes and never treats SB-off as failure.
Unverified (assumptions, hand to researcher): that Debian's `linux-image-*-rt-amd64` in the pinned suite is signed and accepted by shim without enrolment; lockdown=integrity under SB; availability of `OVMF_CODE_4M.secboot.fd` + MS-enrolled vars in the QEMU container (`tools/Dockerfile.qemu`). The QEMU test answers these when it runs. brain/INDEX.md line for ADR-0099 is added by the coordinator chore (file held by another feature).
## 10. Code standards
`.claude/rules/code-structure.md`, `testing-and-flakes.md`, `gates-and-checks.md` (a missing OVMF is reported UNAVAILABLE, never turned into a pass for a failed boot).
## 11. Dependencies and order
contract PR content -> qemu -> one PR.
| Slice | Agent | Diff | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| check | distro-engineer | M | med | sonnet | feat/system.install.secure_boot-check | main | ADR-0099, sonora-secureboot-check, tools/test-secureboot-check.sh |
| qemu | distro-engineer | M | med | sonnet | feat/system.install.secure_boot-qemu | check | tools/test-secureboot-qemu.sh |
Open questions with defaults: check wired into boot/acceptance? Default no (separate item; sonora-acceptance not touched).
