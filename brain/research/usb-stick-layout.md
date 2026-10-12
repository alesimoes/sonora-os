# What is on a SonoraOS stick, and how persistence actually lands

Established 2026-10-12 by running `tools/write-usb.sh` against a loop device
and booting the result in QEMU (`tools/test-write-usb.sh`). Every figure below
was measured unless it says otherwise.

Environment: Windows 11 host; Docker Desktop on WSL2 kernel
6.6.87.2-microsoft-standard-WSL2; Debian trixie container with util-linux
2.41.5 and e2fsprogs 1.47.2; QEMU 11.1.0 for Windows with WHPX. The image
was `iso/sonora.iso` as built that day, 909,115,392 bytes (exactly 867 MiB),
kernel `6.12.111+deb13-rt-amd64`, live-build 1:20250505+deb13u1.

## The ISO already has two partitions

`fdisk -l iso/sonora.iso`:

    Device      Boot Start     End Sectors  Size Id Type
    sonora.iso1 *       64 1775615 1775552  867M  0 Empty
    sonora.iso2        540    7195    6656  3.3M ef EFI (FAT-12/16/32)

Partition 2 is the EFI system partition, and it lies *inside* partition 1.
That is xorriso's isohybrid layout: UEFI firmware boots from entry 2, BIOS
boots the MBR code and isolinux.

Consequences:

- The persistence partition is **number 3**, not 2. ADR-0005's table said 2;
  a correction is appended there. A writer that formats `${DEV}2` destroys
  UEFI boot.
- MBR slot 3 (bytes 478-493 of sector 0) is all zeros in the ISO, so adding
  an entry there changes 11 bytes and nothing else. Measured: after
  `write-usb.sh`, the first 909,115,392 bytes of the device differ from the
  ISO in exactly 11 bytes, all inside that slot.
- The ISO ends exactly on a MiB boundary today, so partition 3 starts at
  867 MiB, sector 1,775,616, immediately after partition 1. Not guaranteed
  for future builds; the writer rounds up to the next MiB.

## parted cannot edit it; sfdisk can

`parted -s sonora.iso print` reports `Partition Table: unknown`, after a
warning that "the driver descriptor says the physical block size is 2048
bytes". xorriso also writes an Apple partition map, whose driver descriptor
lives in sector 0 alongside the MBR, and libparted trusts it first. So
`parted mkpart` fails, and it fails *after* `dd` has already overwritten the
stick. `sfdisk --append` reads the same sector as an MBR and works.

sfdisk then warns that "the device contains 'iso9660' signature and it may
remain" and recommends `wipefs`. Following that advice erases the image just
written. The writer hides sfdisk's output unless it fails.

## Where live-boot puts the /home overlay

With `persistence.conf` containing `/home union`, live-boot mounts the
partition at `/run/live/persistence/sda3` and builds:

    overlay rw,noatime,lowerdir=/run/live/rootfs/filesystem.squashfs//home,
      upperdir=/run/live/persistence/sda3/home/rw,
      workdir=/run/live/persistence/sda3/home/work

The changed files are under `home/rw/` on the partition, not `home/`. Anything
reading a stick offline, such as a recovery tool or a test, has to look there.
After a boot of the shipped system, the partition holds
`home/rw/sonora/Sessions/Untitled`: the engine's session folder is on the
stick, as ADR-0005 intends.

live-boot found the persistence partition on the same USB device it booted
from, with no extra parameter beyond `persistence`.

## Testing in QEMU: `systemd.run=` needs `systemd.unit=`

`systemd-run-generator` is in the image and does create
`kernel-command-line.service` and `kernel-command-line.target` from
`systemd.run=`. It runs nothing on this image, though. The generator makes the
command run by pointing `default.target` at its target, from the generator
directory, and the image's `systemctl set-default multi-user.target` puts a
`default.target` link in `/etc/systemd/system`, which wins. The system boots
normally and the command is silently ignored. Adding
`systemd.unit=kernel-command-line.target` fixes it. Seen with
`systemd.log_level=debug`: "Unit multi-user.target has alias default.target".

## Docker Desktop is a hostile place to test a disk writer

From a privileged container on this host, `lsblk` shows five virtual disks.
Their mounts live in the VM's other mount namespaces, so only two look
mounted from inside the container (`/etc/hosts` on `sde`, swap on `sdc`).
The original writer's only guard was "is it mounted?", so it would have offered
to erase `sda`, `sdb` and `sdd`, the last of them a 256 GB VM disk. The guard
now requires `TRAN=usb`/`mmc` or `RM=1`; every one of those disks reports
`TRAN=''` and `RM=0`.

New partition nodes also never appear in the container's static `/dev`. The
test mounts `devtmpfs` over it, which is what a real system has.

## Not verified

- A physical stick. Everything here is a loop device or a QEMU usb-storage
  device. Real sticks differ in removable flag, `TRAN` and sector size.
- Boot on physical firmware, whether BIOS or UEFI. QEMU's SeaBIOS and edk2
  both boot the written image unattended (see the bootloader-timeout
  postmortem of the same date), but real firmware is pickier about hybrid MBRs.
- Write and read speed of a USB stick under a recording load (ADR-0005
  already doubts it).
