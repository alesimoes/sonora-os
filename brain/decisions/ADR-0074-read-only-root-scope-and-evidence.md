# ADR-0074 — Read-only root: scope and evidence

- Status: Accepted
- Date: 2026-11-08

## Context

The stick boots a live image whose system is a squashfs on the ISO. The owner
asked that the root be read-only, so a stray write cannot corrupt the system
and a pulled stick cannot leave a half-written system behind. ADR-0005 already
decided the persistence layout: a `persistence` partition whose
`persistence.conf` unions `/home` and nothing else. The question left open was
what "read-only root" means for each of the two images we ship, what counts as
proof, and whether the installed system should be read-only too.

Two commits on `main` carry the work:

- `e8052ec9` (#351) caps the RAM-backed writable paths (journald volatile and
  capped, `/tmp` a capped tmpfs with `nosuid,nodev`) and adds
  `tools/test-ro-root.sh`, a static check of the configuration.
- `f4daa342` (#398) adds the running check `root is overlay over read-only
  squashfs` to `sonora-policycheck`, which `tools/test-policycheck-qemu.sh`
  reads on a QEMU boot.

## Decision

1. **Scope of the live image.** The LIVE/stick image runs `/` as overlayfs
   over a read-only squashfs (`boot=live`). Only `/home` is persisted, on the
   stick's `persistence` partition (ADR-0005). Writes to anything else land in
   the RAM upper layer, and that layer is bounded: journald is volatile with a
   `RuntimeMaxUse` cap, and `/tmp` is a tmpfs with a `size=` cap.
2. **The installed system is deliberately not read-only.** `sonora-install`
   writes an ext4 root with `errors=remount-ro`. It is protected by A/B slots
   and rollback (ADR-0044, ADR-0035), not by a read-only mount. A read-only
   installed root is rejected (see Alternatives).
3. **Evidence is split by what can prove it.** The static test
   `tools/test-ro-root.sh` proves the configuration: `boot=live` is on the
   kernel line, `persistence.conf` is exactly `/home union`, journald and
   `/tmp` are capped. It cannot prove a running mount. The QEMU policy check
   proves the running mount: `root is overlay over read-only squashfs` reads
   `findmnt` on a booted image. Only a boot can prove that, so the static test
   never claims it.

## Rationale

- A read-only root is only useful if the writes it forbids are the ones that
  would otherwise corrupt the system. `/home` is the only place a user writes
  data they want kept, so it is the only place that is persisted.
- Capping the RAM layer matters because an overlay upper layer lives in RAM.
  An unbounded journal or `/tmp` would fill it and fail writes system-wide.
- The installed root needs writes on purpose: Debian package upgrades, `/etc`
  edits and the update slots all write to it. A read-only root would make the
  first `apt` or update fail. Keeping it writable and rolling back with A/B
  turns a bad update into a reboot, not a broken machine.
- The static test and the running check fail in different ways. The static
  test catches a config change before a build. The running check catches a
  config that builds but mounts differently (a missing `boot=live`, a
  persistence volume that is not `/home`). Keeping both is cheap; dropping
  either leaves a gap that only a boot shows.

## Consequences

- The live image's `/` cannot be written beyond the RAM cap. A take is written
  to `/home`, so a take must land on the persistence partition or an attached
  internal disk, never on `/`.
- Writes to the installed root can fail at runtime. `errors=remount-ro` makes
  an ext4 error remount the root read-only mid-session. That failure is
  visible only while recording, which is why the full-overlay case below is
  open.
- **How the running check is run.** `tools/test-policycheck-qemu.sh --static`
  runs in CI (`.github/workflows/ci.yml`). The running check (without
  `--static`) boots QEMU and needs KVM. Neither `ci.yml` nor `nightly.yml`
  runs it. It is run by hand on the KVM machine. The claim "proven in CI"
  therefore covers the static test only. Wiring the QEMU boot into the
  nightly is a follow-up.

## Unverified

- **Full `/home` or full overlay upper layer while recording.** Nothing tests
  what happens when the persistence partition or the RAM upper layer fills
  during a take. The expected behaviour is that the engine keeps recording
  into a bounded buffer or fails loudly at start, never silently drops
  samples. That is unproven. Follow-up: a QEMU test that fills the overlay and
  asserts one of those two outcomes. Not built here; hand to researcher and
  tester.

## Alternatives rejected

- **`toram`.** Copies the whole squashfs into RAM at boot. It needs RAM of
  the size of the image plus the upper layer, which fails on small machines we
  have not seen, and it adds boot time. `test-ro-root.sh` rejects it by name.
- **tmpfs root.** A RAM root loses everything at shutdown, including the
  `/home` persistence we need. It does not fit ADR-0005.
- **Read-only installed root.** Rejected, see Decision 2 and Rationale: package
  upgrades, `/etc` and update slots need writes, and an unwritable root fails
  at runtime during a take, not at start-up.
- **`fstab ro`.** Marking the root read-only in `fstab` does not work for a
  live image, where `live-boot` owns the root mount. The static test rejects an
  `fstab` line that names `/`.
