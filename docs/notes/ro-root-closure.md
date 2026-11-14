# Read-only root: closure note

Item: `system.hardening.ro_root`. Closed on evidence; record in
`brain/decisions/ADR-0216-ro-root-closure-record.md`.

- Scope and evidence: ADR-0074 (read-only root scope and evidence).
- Close-out: ADR-0178 (takes land on `/home`, never `/`; no source change).
- Static check: `tools/test-ro-root.sh` prints `ok` (squashfs root,
  `persistence.conf` is `/home` union only, journald volatile 64M, `/tmp` 512M).
- Trap line: `.claude/context/os-image.md`, read-only root entry.

## Unverified

- KVM/QEMU running-mount check on a booted image. Hardware-bound, parked; not
  run here. Not a pass.
- Full-overlay check (fill the overlay; the engine keeps bounded buffers or
  fails loudly at start). Follow-up from ADR-0178; needs KVM.
