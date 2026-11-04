# ADR-0044 — A/B updates of the image

- Status: Accepted (recommended default, taken with no owner available,
  2026-11-04)
- Date: 2026-11-04
- Item: `system.hardening.ab_updates`
- Relates to: ADR-0002 (Debian base), ADR-0005 (USB persistence), ADR-0032
  (installer), ADR-0035 (updates on an installed system: apt now, A/B later;
  this ADR replaces its mechanism and keeps its rules). Number 0044 was
  reserved by the coordinator.

## Summary

The installed disk gets two root slots, `sonora-a` and `sonora-b`, an ESP and
one data partition (`sonora-data`, mounted on `/home`, holding the sessions).
An update is a signed bundle (kernel, initrd, squashfs, `VERSION`,
`SHA256SUMS` and its detached signature) fetched over https or read from a
stick. `sonora-update apply` verifies it, writes it to the slot that is not
running, and arms a trial: GRUB boots the new slot up to three times, and the
engine's health confirms it after it has stayed up for 60 s. If it never
confirms, GRUB returns to the old slot by itself. An update never reboots,
never touches the running slot or the data partition, and never runs while a
session is open (the ADR-0035 gate). The slot logic is a set of POSIX shell
functions tested against file images and a fake grubenv; GRUB reading the env
and a real slot boot are unverified.

## Decision

1. **Layout** (GPT, created by the installer from `slots_layout_script`):
   ESP 512 MiB; `sonora-a` and `sonora-b`, 6 GiB each (ext4, label = name);
   `sonora-data`, the rest of the disk, ext4, mounted on `/home` by LABEL.
   A bigger disk grows the sessions, never the slots. Minimum disk 16.5 GiB.
   Each slot holds `live/vmlinuz`, `live/initrd.img`,
   `live/filesystem.squashfs` and `live/VERSION`, the same files the stick
   boots, so a slot is the tested image and not a second package set. The
   squashfs is read-only as ever; `/home` is the only writable persistent
   path (ADR-0005), now a partition instead of the stick's persistence file.
2. **State** is four GRUB variables in `EFI/sonora/grubenv` on the ESP:
   `sonora_slot` (committed), `sonora_trial`, `sonora_tries`,
   `sonora_valid_a/b`. The ESP is read-only in normal running and remounted
   read-write for one write, then back; if it cannot be remounted read-only
   the tool stops. Writes go through a temporary and `mv`, so a power cut
   leaves the old block or the new one.
3. **Boot counter and rollback.** `grub-ab.cfg` (mirrored by
   `slots_boot_choice`): with a trial set and tries left, spend one try
   *before* booting and boot the trial slot; with none left, mark that slot
   invalid, clear the trial and boot the committed slot. A hang, a kernel
   panic or a power cut counts as a failed try. The menu entry pins its slot
   (`live-media=/dev/disk/by-label/sonora-X`, `sonora.slot=X`): both slots
   carry the same file names and live-boot would otherwise take either.
4. **Confirm.** `sonora-confirm.service` (conditioned on `sonora.slot` on the
   command line, so inert on the live stick) sleeps 60 s after
   `sonora-session.service` and runs `sonora-update confirm`, which succeeds
   only if the engine is still active and the running slot is the trial slot.
   Confirm sets `sonora_slot` to the trial slot and clears the trial. The old
   slot stays valid, which is what manual rollback uses.
5. **Fetch and verify.** `--url BASE` (https only, `curl --proto =https`) or
   `--from DIR` (a stick). Order: `SHA256SUMS` and `SHA256SUMS.sig`;
   signature with `gpgv` against `/usr/share/keyrings/sonora-release.gpg`
   (a missing key refuses, never skips); manifest names checked (no slash, no
   leading dot); `VERSION` hash, then version must be newer than
   `/etc/sonora-release` (`--allow-downgrade` to override); only then the big
   files, each against the signed hashes. Nothing is written before all of it
   passes. The key lives in the image, so a key rotation is an image update
   signed by the old key.
6. **Write.** Target is always the slot that is not committed, and only while
   the running slot is the committed one: during a trial the old slot is the
   only known-good copy and a second update is refused (exit 2). Order:
   `sonora_valid_X=0`, build the filesystem (`mke2fs -d`, no mount), then
   `slots_mark_written` (valid, tries, trial). A write that dies half-way
   leaves an invalid slot GRUB never boots.
7. **What an update never does during a session.** It does not start unless
   `sonora-session-open` (the engine's probe; exit 0 open, 1 none) says 1;
   a missing or erroring probe refuses. It then creates
   `/var/lib/sonora/updates-allowed` for its own run only (the ADR-0035 flag)
   and removes it on exit. It does not reboot; the message says reboot when
   the session is finished. It writes only to the inactive slot and the ESP
   env, never to `/home`, so a take on disk cannot be affected. `rollback`
   and `confirm` only flip the env and are not gated.
8. **Rollback.** `sonora-update rollback`: cancels a trial, or else commits
   the other slot if it holds a verified image. Automatic rollback is item 3.
9. **Dry run.** `apply --dry-run` runs the gate check, fetches and verifies
   the manifest and `VERSION`, prints the plan and writes and arms nothing.
10. **apt.** ADR-0035's apt path is retired once the installer lays out slots.
   Its rules carry over: no unattended updates, nothing mid-session, signed
   offline stick, the RT kernel moves only with a Sonora release.

## Rejected

- **Whole-disk image dd.** Needs the data partition moved or kept across
  writes; a slot per squashfs keeps sessions out of the update path.
- **RAUC / SWUpdate / Mender.** Fit, but bring a daemon, bundle format and
  their own bootloader integration for what is about 200 lines of shell here.
  The escape hatch if hardware needs more (multiple bootloaders, delta).
- **systemd-boot `boot assessment`.** Same idea (`+3` counters) and a better
  mechanism, but the installer already makes GRUB entries (ADR-0032) and the
  RT kernel flow is GRUB-based; revisit with Secure Boot (ADR-0034).
- **Confirming from inside the engine.** Couples the engine to the OS image.
  A service watching the engine's unit keeps the engine unaware.

## Unverified

- GRUB parsing `grub-ab.cfg` and persisting `save_env` to the ESP; the ladder
  is the weak point (`SLOTS_TRIES` is tied to 3 by hand).
- A real boot of either slot, the `live-media=` label pin with live-boot, the
  60 s confirm window, and a real rollback after a bad image.
- `/usr/local/bin/sonora-session-open` does not exist yet; until it does
  `apply` always refuses (fail closed).
- The signing key, the release publisher, and `/etc/sonora-release` written by
  the build do not exist yet; the tests use a fake verifier.
- Disk space of the scratch directory (`/home/.sonora-update`) is not checked.

## Follow-ups

- `sonora-install` sources `slots.sh`, lays out the disk with
  `slots_layout_script`, writes the first image into slot a with
  `slots_write_image`, `slots_env_init` on the ESP, includes `grub-ab.cfg`,
  adds `LABEL=sonora-data /home` to fstab. Separate PR (another agent owns
  the installer).
- `sonora-session-open`, the release key and `/etc/sonora-release` in the
  build; a release publisher script.
- QEMU test plan (below), run by the owner.

## QEMU test plan (listed, not run)

Install to a qcow2 with the installer once it uses the slots, then:

1. Boot: `/proc/cmdline` has `sonora.slot=a`; `sonora-update status`
   shows `sonora_slot=a`, no trial.
2. `apply --from` a stick image with a newer `VERSION`: refuses while a
   session is open, then succeeds with none; slot b label and files present;
   no reboot happened.
3. Reboot: `sonora.slot=b`; after 60 s `status` shows `sonora_slot=b`, no
   trial.
4. Bad image (squashfs that panics): reboot four times; third failure falls
   back to `sonora.slot=a`, `sonora_valid_b=0`.
5. Power cut mid-write (kill QEMU during `apply`): next boot is slot a.
6. Engine that crashes on boot: no confirm, rollback after three tries.
7. Tampered `SHA256SUMS.sig` and tampered squashfs: refused, slot b unchanged.
8. Sessions in `/home` byte-identical (checksums) across 3 to 7.
9. `sonora-update rollback` after a confirmed update returns to slot a.

Privileged file-image test, for the coordinator:

    docker run --rm --privileged -v "$PWD":/src -w /src debian:trixie sh -c \
      'apt-get update -qq && apt-get install -y -qq fdisk e2fsprogs util-linux gpgv && tools/test-slots-loop.sh'
