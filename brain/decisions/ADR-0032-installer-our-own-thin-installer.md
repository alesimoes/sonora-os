# ADR-0032 — The installer is our own thin one, over Debian's tools

- Status: Accepted (recommended default, taken with no owner available,
  2026-11-04)
- Date: 2026-11-04
- Item: `system.install.adr` (OS 3, "Installable to disk")
- Closes backlog item system.install.adr
- Relates to: ADR-0002 (Debian base), ADR-0005 (USB persistence), ADR-0010
  (kiosk compositor; its Phase 6 note about Calamares)

## Summary

`system.install.installer` is built as `sonora-install`: a small POSIX shell
program of about 300 lines over Debian's own tools (`sfdisk`, `mkfs.ext4`,
`unsquashfs`, `grub-install`, `efibootmgr`, `update-initramfs`). It copies the
very squashfs the stick booted, so the installed system is the tested image
and not a second package set. It has one job: whole disk, erase, copy, boot
entry, carry the sessions over. No partitioning choices, no desktop-install
decision tree. The logic lives in a script with a fixed exit-code contract and
a `--yes --target DISK` mode that CI drives in QEMU; the screens (a tty2 text
front end first, a Qt page in `sonora-ui` later) only call it. Debian
Installer (with `live-installer`) and Calamares were rejected; Calamares is
the named escape hatch if real hardware needs more than whole-disk (LUKS,
dual boot, RAID), because the script contract lets it call our steps as its
own jobs. Nothing is verified on hardware yet; see Unverified.

## Context

The owner's requirement (2026-10-11, ROADMAP OS 3): install on a laptop and
run from there. The installed system, not the live stick, is the target, tuned
for low plugin latency; the live medium is only for trying it. The
`--debian-installer none` setting in `iso/auto/config` leaves the question
open on purpose.

What the installer has to guarantee, in the order that matters:

1. The installed machine boots the same way every time, on hardware we have
   not seen: the RT kernel by default (postmortem 2026-10-11), `threadirqs`,
   the same PipeWire and limits drop-ins, the same kiosk units (ADR-0010).
2. Nothing that can lose a take: it never touches a disk the operator did not
   name, never the disk it booted from, never deletes the stick's sessions.
3. It fails loudly. An install that half-worked must say so at the end and
   must not leave a machine that boots into something other than the product.

## Decision

1. **Own installer, thin.** `sonora-install` in `usr/local/bin` of the image
   (`iso/config/includes.chroot`), mode 100755. Steps, each checked, any
   failure stops with a named reason and a non-zero code:
   1. Select the target. Candidates are internal, non-removable, non-USB
      block devices by default; the boot medium is excluded by its mount;
      a USB target needs an explicit `--allow-removable`. The operator types
      the device name to confirm the erase.
   2. Partition GPT: ESP (FAT32, 512 MiB), root ext4 (the rest, label
      `sonora-root`, mounted `noatime`, UUIDs in fstab). One root, sessions
      under `/home/sonora/Sessions`. A separate data partition, swap sizing
      and journal tuning belong to `system.install.rootfs`, which changes
      `sonora-install` only in the layout table.
   3. Copy `/run/live/medium/live/filesystem.squashfs` with `unsquashfs` into
      the target. The kernel cmdline is rebuilt from `iso/auto/config` minus
      `boot=live`, `components` and `persistence`, keeping `threadirqs`.
   4. In the target chroot: remove `live-boot` and `live-config`, write
      fstab and the machine id, `update-initramfs -u -k all`, `grub-install`
      for UEFI with the NVRAM entry **and** the `--removable` fallback path,
      or for BIOS onto the disk's MBR gap on a BIOS machine.
   5. Carry over sessions: copy `/home/sonora/Sessions` of the running stick
      (its persistence overlay) to the target, compare file sizes and
      SHA-256, and never remove the source.
   6. Self-check before the success line: the target's default boot entry
      names the `rt` kernel, the initramfs for it exists, fstab UUIDs resolve,
      the ESP holds the loader, session checksums match. Only then a marker
      `/var/lib/sonora/installed` is written, last. Any check failing prints
      what failed and exits non-zero; the success banner is not reachable
      otherwise.
2. **The contract is the script, not the screen.** Exit codes and the
   machine-readable progress lines (`STEP n/N name`) are the interface. The
   tty2 front end (owner of the keyboard, `OnFailure` fallback of ADR-0010
   already lands there) is first; a Qt page in `sonora-ui` may follow without
   changing the script. CI runs `sonora-install --yes --target` against a
   blank QEMU disk and boots the result with the stick removed.
3. **Updates and Secure Boot are separate items.** This ADR does not decide
   them. The installer writes GRUB with Debian's signed `shim-signed` and
   `grub-efi-amd64-signed` present in the image when `system.install.secure_boot`
   asks for it; until then the install is unsigned-tolerant, as the stick is.
4. **Scope limits, stated.** Whole disk only. No LUKS, no dual boot, no RAID,
   no network install, no language or timezone questions (the operator
   account and locale are the image's). A request for any of them reopens this
   ADR and moves to Calamares (see Alternatives).

## Rationale

- *The installed system equals the tested image.* Copying the squashfs means
  every test run against the ISO (boot smoke, RT kernel check, package list)
  speaks about the installed machine. A package-based install builds a second
  system from the archive at install time, on whatever mirror state the
  operator's network has, and the RT kernel default would have to be proven
  again (the failure of the 2026-10-11 postmortem).
- *Fail loudly.* A 300-line script with every step checked and a closing
  self-check is auditable in one sitting. A general installer's jobs can
  succeed and still leave a different bootloader default than we wanted.
- *Cost later.* Own code means we own bugs on hardware (odd NVRAM
  implementations, drives with 4Kn sectors, NVMe naming). That is the price;
  `--removable` fallback and the self-check bound it, and the QEMU job
  covers UEFI and BIOS. The script is small enough that the price is days,
  not a framework to learn.
- *Appliance shape.* The ROADMAP says the installer "has one job and should
  not offer a desktop-install decision tree". Every general installer is
  built around that tree and would be configured down to it.

## Consequences

- Image gains `sonora-install`, `sfdisk`, `squashfs-tools`, `efibootmgr`,
  `grub-efi-amd64-bin`, `grub-pc-bin`, `dosfstools`, `rsync` (a few MiB). The
  image-config test checks the packages and that the script is executable.
- New BACKLOG slices for `system.install.installer`: (a) script and QEMU
  UEFI/BIOS test, (b) session carry-over with checksum, (c) tty2 front end,
  (d) Qt page (later). None of these is started by this ADR.
- The tty2 text front end and the later Qt page both exec the script; neither
  holds install logic.
- Every installed-disk figure (ext4 write throughput at 32 tracks, boot time)
  is taken by the researcher after (a), not assumed.

## Unverified

- That GRUB with `--removable` plus an NVRAM entry boots on the target
  laptops. An assumption from Debian's own documented behaviour. Measurement:
  boot the installed disk in QEMU (OVMF, SeaBIOS) in CI, then on the two real
  machines of the OS 1 exit criterion. Hand to the researcher.
- That `update-initramfs` in the chroot, after removing `live-boot`, produces
  an initramfs that mounts a root by UUID on NVMe and SATA controllers we
  have not seen (modules are `MODULES=most` in Debian by default; the
  assumption is that this stays true). Settled by the QEMU job with virtio,
  NVMe and SATA disk models, then hardware.
- Whether `unsquashfs` of a roughly 2 GB image to a slow internal disk takes
  an acceptable time. Measure on the laptop; the answer changes only the
  progress display.
- The state of `live-installer` in trixie (not read in the source; searched
  only at the live-manual level). It does not change the decision, which rests
  on the tested-image argument, but it should be read before a future
  reversal.
- Calamares's installed size and dependency list on trixie (3.3.14-1, Qt 6
  per Debian's package index, checked by search only). Needed only if the
  escape hatch is used.

## Alternatives rejected

- **Debian Installer with `live-installer`** (`lb config --debian-installer
  live`, preseed under `config/includes.installer/`; published in the Debian
  Live Manual's "Customizing Debian Installer"). It can copy the live
  filesystem, and preseeding can answer most questions. Rejected because: its
  text UI and partman are a decision tree we would suppress question by
  question through preseed keys, each a place a silent default can reappear on
  a Debian update; the installer runs in its own initrd environment, a second
  runtime with its own kernel, so the RT kernel and our drop-ins are not the
  ones under test; custom steps are udebs, a packaging format only this part
  of the project would use. Kept as the reference for how Debian handles
  `grub-install` options.
- **Calamares** (Debian trixie source package 3.3.14-1, Qt 6; the ADR-0010
  note that it would be another client of the same compositor holds). Its
  `unpackfs`, `partition`, `bootloader` and `users` modules do these steps and
  it is maintained by others. Rejected for now because: it brings KPMcore and
  KDE Frameworks to an image that otherwise has Qt only for the product; its
  partition module is the decision tree we want absent, configured away rather
  than not present; the post-copy self-check and session carry-over would be
  custom Python or shell jobs in any case, so the thin script is still
  written. **It stays the escape hatch**: if LUKS, dual boot or RAID are
  required, the script's steps become Calamares `shellprocess` jobs and
  only the front end changes.
- **A package-based install (`debootstrap` + `apt install sonora` on the
  target).** Rejected: needs a network, drifts from the tested image, and the
  RT-kernel default has to be re-proven on every install.
- **`dd` the whole stick image to the disk and grow the partition.**
  Rejected: copies the live-boot configuration, the read-only layout and the
  persistence label (ADR-0005), which are exactly what an installed system
  must not have, and gives no UUID-based fstab or disk-specific bootloader.
