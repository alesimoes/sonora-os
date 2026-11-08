# ADR-0097 - `system.hardening.ab_updates` is delivered by #398; what is left

- Status: Accepted (recommended defaults, architect 2026-11-08)
- Date: 2026-11-08
- Item: `system.hardening.ab_updates`
- Relates to: ADR-0044 (the design, binding), ADR-0035 (updates on an installed
  system), ADR-0091 and ADR-0094 (same closing pattern)

## Summary

The A/B mechanism of ADR-0044 is on main, all from PR #398:
`iso/config/includes.chroot/usr/local/lib/sonora/slots.sh`,
`iso/config/includes.chroot/usr/local/bin/sonora-update`,
`iso/config/includes.chroot/usr/local/share/sonora/grub-ab.cfg`, and the tests
`tools/test-slots.sh`, `tools/test-slots-loop.sh`, `tools/test-ab-updates.sh`
(the last one run by `.github/workflows/ci.yml`). No new design. The item
closes by reference. Three gaps are real and are named below as follow-ups,
each owned by another item.

## Decision

1. Close the item by reference to #398 and ADR-0044. This PR is bookkeeping.
2. Gap 1: `sonora-install` does not use `slots.sh` (no `sonora.slot=`, no
   `sonora-a`/`sonora-b`; `git grep` finds no slot reference in the installer
   scripts). Making the installer lay out two slots belongs to
   `system.install.rootfs`, which owns `sonora-install`; hand it over, do not
   edit the installer here.
3. Gap 2: `/usr/local/bin/sonora-session-open` does not exist, so `apply`
   always refuses (fail closed, by ADR-0044). That is the safe state: no update
   can run during a take. A follow-up must define how the engine publishes
   "session open" (a state file the engine owns, not a poll of the audio path).
4. Gap 3: release signing key, publisher and `/etc/sonora-release` in the build.

## Rationale

Re-implementing a delivered mechanism would duplicate #398 and collide with
`system.install.rootfs`. Leaving `apply` refusing is the loud, safe failure.

## Consequences

Updates cannot be applied on a real installed system until gaps 1-3 close.
The backlog line stays open in spirit; the follow-ups carry it.

## Unverified

GRUB reading `grub-ab.cfg` and `save_env` to the ESP, a real slot boot, the 60 s
confirm window and a real rollback: ADR-0044's QEMU test plan is still not run.
Hand to the researcher/tester after gap 1 lands.

## Alternatives rejected

- Build the session probe and installer integration here: touches files owned by
  an active feature and the engine's session state, which needs its own design.
- apt-only updates: no rollback of a bad kernel or image (ADR-0044).
