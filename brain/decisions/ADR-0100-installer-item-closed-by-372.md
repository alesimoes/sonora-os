# ADR-0100 - `system.install.installer` is already delivered by #372

- Status: Accepted
- Date: 2026-11-08
- Item: `system.install.installer`
- Relates to: ADR-0032 (installer), ADR-0089 and ADR-0090 (numbers reserved
  by the coordinator; neither file is on main at this commit, so they are
  cited by number only)

## Context

The backlog line "Installer: partition, copy, bootloader, operator account,
carry over sessions" is open, but main already has it. What exists on main,
checked against the code (`git grep origin/main`):

- `iso/config/includes.chroot/usr/local/bin/sonora-install` (mode 100755,
  from #353 `414bbcc3`, end to end in #372 `1c227315`, consolidated in #398
  `f4daa342`). It runs seven steps, each printed as `STEP n/7 name`, in the
  order select, partition, format, copy, configure, bootloader, selfcheck.
- Session carry-over: the sessions of the running stick
  (`/home/sonora/Sessions`) are copied to the target and compared by SHA-256
  (`sha256sum -c`). A checksum failure exits 11.
- `iso/config/includes.chroot/usr/local/bin/sonora-install-tty`, the tty2
  front end. It turns the `STEP` lines and the exit code into what is on
  screen.
- `iso/config/includes.chroot/etc/systemd/system/sonora-installer.service`,
  the unit that starts the front end.
- `tools/test-installer.sh` and `tools/test-installer-tty.sh`, the tests
  that cover both scripts.
- The operator account is the image's own `sonora` user (uid 1000, created by
  `useradd --create-home --uid 1000` in
  `iso/config/hooks/normal/0100-sonora-system.hook.chroot`). ADR-0032 decision
  4 states that account and locale are the image's, so the installer does not
  create one.

## Decision

No code and no new design. The item closes by reference to #372 and #398
(with #353 as the first slice) and to ADR-0032. Open parts stay under their
own items: layout and swap (`system.install.rootfs`, ADR-0090), the Secure
Boot check (`system.install.secure_boot`), the Qt install page (a UI item).

## Rationale

Re-implementing would duplicate a tested script and edit `sonora-install`,
which `system.install.rootfs` holds.

## Consequences

The PR is docs only. No file changes on main.

Unverified (not run by the author; each stays open under the item named):

- The two test scripts, `tools/test-installer.sh` and
  `tools/test-installer-tty.sh`, were not run for this ADR. A QA pass on main
  should run them.
- That the operator account is copied with the squashfs into the target. This
  is read from the design in ADR-0032 decision 1 step 4, not from a run.
- A run on real hardware and on a fresh ISO (ADR-0032 "Unverified" list):
  GRUB with `--removable` and the NVRAM entry, `update-initramfs` on NVMe and
  SATA controllers not yet seen, and the time of `unsquashfs` to an internal
  disk.
- ADR-0089 and ADR-0090 are cited by number only; their files are not on main
  at this commit.

## Alternatives rejected

- A fresh implementation task: duplicates #372 and collides with rootfs.
- Leaving it open: it would be dispatched again.
