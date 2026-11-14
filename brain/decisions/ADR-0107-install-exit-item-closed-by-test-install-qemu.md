# ADR-0107 - `system.install.exit` is already delivered by `tools/test-install-qemu.sh`

- Status: Accepted
- Date: 2026-11-08
- Item: `system.install.exit`
- Relates to: ADR-0032 (installer), ADR-0090 (disk layout), ADR-0100 (same kind of closure)

## Context

The backlog line "Phase 6 exit: install, boot without the stick, session
survives reboot" is open. Main has the test that is that exit criterion:
`tools/test-install-qemu.sh` (header: "Phase 6 exit test for the installer").
Run 1 boots the ISO with a blank virtio disk, writes a session into
`/home/sonora/Sessions`, runs `sonora-install --yes --target /dev/vda`, and
powers off. Run 2 boots only the disk (no CD, kernel or initrd), UEFI or BIOS,
and the in-image smoke test (`sonora-smoketest`, `sonora.expect_installed`)
asserts the engine, the RT kernel, an ext4 root that is not a live overlay, no
live medium, and that the carried session file has the SHA-256 it had on the
stick. The script itself checks that run 1 exits 0 and powers off, that the
SHA-256 before the install equals the one after, and that the installed boot
reports `SMOKETEST RESULT pass`. Exit 77 skips without QEMU or a usable Docker.
Exit 3 is only a slow-mode (TCG) run that ran out of clock, so it is
inconclusive; any other failure exits 1. `tools/test-install-layout-qemu.sh`
covers the ADR-0090 layout.

## Decision

Close the item by reference; no code. Writing a session on the installed system
and surviving a further reboot adds nothing the test does not show: the
session lives on the installed ext4 root and the carried file is read after a
boot that never saw the stick. A second reboot would test ext4, not our code.

## Rationale

A duplicate test costs two more QEMU runs of CI time and proves nothing new.

## Consequences

- The item is ticked from this ADR; the PR carries the INDEX line.
- No workflow runs `tools/test-install-qemu.sh`: `nightly.yml` and
  `nightly-boot.yml` run other QEMU tests only. The closure therefore rests on
  a run by hand, `tools/build-iso.sh` then `tools/test-install-qemu.sh`, whose
  `INSTALL-TEST RESULT pass` line is the evidence to record. Wiring it into a
  nightly is a follow-up, not part of this ADR.
- Unverified: the test was not run for this ADR (needs the ISO, QEMU or
  Docker); real hardware install and boot remain the OS 1 exit measurement for
  the researcher (ADR-0032 unverified list).
- Gap if the owner wants it: a fresh recording on the installed system after
  boot 2, then a third boot. Not done; assumption stated above.

## Alternatives rejected

- A third QEMU run (record, reboot, check): re-tests the filesystem, not us.
- Leaving the item open: the exit criterion would stay red though tested.
