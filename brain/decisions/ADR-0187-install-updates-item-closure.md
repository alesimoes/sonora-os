# ADR-0187 — Closure of the system.install.updates item

Status: accepted (closure record, 2026-11-14)
Item: `system.install.updates`
Relates to: ADR-0035 (policy), ADR-0044 (A/B updater), ADR-0101 (session-open probe contract)

## Summary
The OS side of `system.install.updates` closes. Its deliverables are on main:
the update policy (ADR-0035, checked by `tools/test-updates.sh`, delivered by
#398) and the session-open probe `sonora-session-open` with its test
`tools/test-session-open.sh` (ADR-0101, delivered by #550). ADR-0101 stands
unchanged; this record only closes the item.

## Context
#550 landed the probe as `[partial]`: the app-side writer of
`/run/sonora/session-open` was left open. That writer is a DAW-track
follow-up, not OS packaging. Until it lands, `sonora-update apply` fails
closed, which is the intended behaviour under ADR-0044, not a gap in the OS
side. Keeping the item open would hide that the OS work is finished and make
the DAW follow-up look like an OS task.

## Decision
1. The `system.install.updates` item is closed for the OS work: the policy
   files, `sonora-update`, `sonora-session-open` and their static tests.
2. The app writing `/run/sonora/session-open` is a follow-up on the DAW track.
   It is recorded as a BACKLOG line by the coordinator, not here.
3. No code, contract, protocol or format change is made by this closure.
   ADR-0101's contract (exit 0 open, 1 none, 2 cannot tell) is unchanged.

## Verification
Run on `origin/main` (153f83e1) in the closure branch:

- `tools/test-session-open.sh`: exit 0. Prints the states and the
  `sonora-update` gate checked against files; the app writer and a real
  `sonora-engine.service` are reported UNVERIFIED.
- `tools/test-updates.sh`: exit 0. Policy files agree with ADR-0035;
  UNVERIFIED on an installed machine.
- `tools/test-ab-updates.sh`: exit 0. Slot logic and `sonora-update` checked
  against files; GRUB, real slot boot and real rollback UNVERIFIED.
- `git ls-files` lists `iso/config/includes.chroot/usr/local/bin/sonora-session-open`
  and `tools/test-session-open.sh`.

## Rationale
A closure record keeps the board honest: the finished OS deliverables are
marked done, and the one open part is named where its owner can act on it.
Re-opening ADR-0101 would change a contract that the engine and updater
sides already build against.

## Alternatives rejected
- **Keep the item open until the app writer lands.** Hides finished OS work
  behind a DAW dependency and blocks the OS track's count. Rejected.
- **Edit ADR-0101 to say the item is closed.** Changes a decided contract for
  a status fact. The status belongs in this record. Rejected.
- **Re-run the real installed update now.** Needs a QEMU or hardware run
  that is parked to the end (owner, 2026-11-13). Rejected for this closure.

## Unverified
- The real installed update (apply on a running A/B system) has not run.
- The app-side writer of `/run/sonora/session-open` does not exist yet, so
  `apply` refuses on every real machine until it lands.
- `test-session-open.sh` could not check the unreadable-state-dir case under
  root (chmod 000 does not hide the directory); it prints UNVERIFIED for it.
