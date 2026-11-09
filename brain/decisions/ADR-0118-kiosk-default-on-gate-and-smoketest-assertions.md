# ADR-0118 — Kiosk: what is left of ADR-0010, and the gate for turning it on by default

Status: accepted (architect, 2026-11-08)

## Summary
ADR-0010's display stack is on main behind `sonora.gui` (#208, #217, #241,
#285, #306, #314, #398). The item `system.image.kiosk` closes on two
deliverables: the smoketest asserts the UI is really running, and the default
flip (compositor without `sonora.gui`) has a written, mechanical gate. The
flip itself is not made here.

## Context
The smoketest's `sonora-ui.service` check passes if the unit is missing, and
the `sonora.gui` branch never asserts `sonora-ui` active (#217 review). A
kiosk that silently never started would pass the boot test, and the operator
would find a bare tty instead of a screen. The QEMU checks (`--gui`,
`--gui-broken`) exist in `tools/test-boot-qemu.sh` but have never run: the
authoring hosts have no `/dev/kvm`.

## Decision
1. Under `sonora.gui` and without `sonora.expect_fallback`, `sonora-smoketest`
   FAILS if `sonora-ui.service` or `sonora-compositor.service` is absent
   (`LoadState=not-found`) or not active. Under `sonora.expect_fallback` it
   FAILS unless `sonora-session.service` is active and
   `/run/sonora-display-failed` exists. Without `sonora.gui`, unchanged.
2. `tools/test-image-config.sh` gains a static check that the smoketest
   contains these assertions and that all three units exist, so the check
   cannot rot without CI noticing.
3. Default flip gate. The compositor's `ConditionKernelCommandLine=sonora.gui`
   is removed (and `sonora-session` made the fallback only) in one later PR,
   and only when a recorded `test-boot-qemu.sh --gui` AND `--gui-broken` run
   passes on a host with KVM, with first-frame time and ISO size in the PR.
   Until then the image boots as today. Why: a wrong default leaves every
   stick without a usable console, and losing the screen before a take is the
   failure the fallback exists for.

## Rationale
Failing loudly at start-up beats degrading silently. The flip is the one
hard-to-undo step, so it waits for evidence, not for time.

## Consequences
- The item ships as a partial PR: the QEMU runs and the flip are KVM-host
  work, listed as unverified and kept as the open BACKLOG line "Stage 1 QEMU
  checks".
- ADR-0010 Unverified items 1, 3, 5, 6, 8 stay with the researcher.

## Unverified
Boot time with the stack, labwc renderer on QEMU `-vga std`, and the logind
session from a system unit. Measurement: `test-boot-qemu.sh --gui` on KVM.

## Alternatives rejected
- Flip the default now: no real boot has ever run it.
- Add a new QEMU harness: `test-boot-qemu.sh` already has both modes; the gap
  is a host, not a script.
- Drop the assertion and rely on the QEMU run: the PR gate never boots QEMU.
