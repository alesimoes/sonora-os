# ADR-0216: Read-only root closure record

Status: accepted

## Context

ADR-0074 set the scope of the read-only root and its evidence. ADR-0178 closed
the item on that evidence and split the open checks into a follow-up. The
closure itself shipped as docs and a trap line in `.claude/context/os-image.md`,
with no source change. This record ties the three together so a later reader
does not reopen the item on a missing check.

## Decision

The item `system.hardening.ro_root` stays closed. Takes land on `/home`
(persistence or internal disk), never on `/`. The record adds no code and no
new check.

Two checks remain unverified and are not part of the close:

- The running-mount check on a booted image (KVM/QEMU, hardware-bound, parked).
- The full-overlay check: fill the overlay and assert the engine either keeps
  bounded buffers or fails loudly at start.

`tools/test-ro-root.sh` covers the static part (squashfs root, `persistence.conf`
is `/home` union only, journald volatile, `/tmp` size). It prints `ok` and
lists the running mounts as unverified.

## Rationale

Closing on static evidence keeps a finished item from waiting on hardware. The
loss case (a take written to `/`) is covered by the static check. The
remaining checks need KVM, which the PR gate does not have, so they belong with
the researcher and tester, not in this item.

## Consequences

- Reopening `system.hardening.ro_root` needs one of the two unverified checks
  to fail, not a re-read of this record.
- The KVM running-mount check is listed as unverified until a boot records it
  under `brain/research/`.

## Alternatives rejected

- Hold the item open until the KVM test exists: blocks a done item on
  hardware-bound work (rejected in ADR-0178).
- Drop the unverified lines to make the record read cleanly: hides a gap.
