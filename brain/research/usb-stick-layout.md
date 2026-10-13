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
now requires `TRAN=usb`, or `RM=1`, or an MMC device whose type is `SD` (see
below). Every one of those disks reports `TRAN=''` and `RM=0`.

New partition nodes also never appear in the container's static `/dev`. The
test mounts `devtmpfs` over it, which is what a real system has.

Two harness traps, measured 2026-10-13. Image tags are shared by every
worktree on the Docker host, and another checkout rebuilt
`sonora-runtime:trixie` mid-run, so the tests run containers by image ID.
And under `pipefail`, `yes | head` and `dd | grep -q` report failure when
the left side dies of SIGPIPE; the harness uses process substitution or
`grep -c` instead.

## Telling a stick from a disk you must not touch

Added after review, 2026-10-12. Each point below was measured in the same
container unless marked.

- **`TRAN=mmc` is not "removable".** lsblk reports it for every `mmcblk`
  device, including the soldered eMMC that mini-PCs and cheap laptops boot
  Windows from. The card type in `/sys/block/mmcblkN/device/type` separates
  them: `SD` is a card in a slot, `MMC` is eMMC, and `SDIO` is not storage.
  (Kernel MMC core sysfs; not observed here, because there is no MMC
  hardware in a container. The writer's decision function is tested with
  synthetic inputs instead.)
- **The kernel sees mounts that lsblk does not.** A partition mounted inside
  `unshare -m` is invisible to lsblk in the parent namespace (`MOUNTPOINT`
  empty). But an `O_EXCL` open of the whole disk fails with EBUSY, because any
  claimed partition marks the whole disk as claimed. `wipefs` without
  `--force` opens with `O_RDWR|O_EXCL` (seen with strace, util-linux 2.41.5).
- **`wipefs --no-act` is not a read-only probe.** strace shows it opening
  `O_RDWR|O_EXCL` and then calling `ioctl(BLKRRPART)`, which re-adds the
  disk's partitions. On a desktop that invites an automount during the very
  prompt the probe is guarding (suspected for GNOME and KDE, not observed
  here). Closing an `O_RDWR` descriptor also makes udev's watch re-probe the
  disk. The probe is now
  `perl -MFcntl -e 'sysopen(F, $dev, O_RDONLY|O_EXCL)'`. strace shows one
  `openat(..., O_RDONLY|O_EXCL|O_CLOEXEC)` and no ioctl. It returned EBUSY
  for a mount in another namespace and passed for a partition held open
  without `O_EXCL`. perl and `Fcntl.pm` come from `perl-base`, which is
  Essential in Debian. Of the util-linux tools, `wipefs` (listing mode) and
  `blockdev` open `O_RDONLY` without `O_EXCL`, so neither can be the probe.
  **Limitation:** a process holding the device open *without* `O_EXCL` is
  invisible to this check, and only a scan of every `/proc/*/fd` would find
  it. Mounts, swap, md and dm all claim exclusively, so they are covered.
- **diskseq tells you whether the name still means the same medium.**
  `/sys/block/<dev>/diskseq` (kernel 5.15 and later) went from 124 to 126
  when the same file was detached from a loop node and reattached to the same
  node: same name, same backing file, same size, different value. The
  fallback for older kernels (major:minor, size, serial) came out identical
  before and after the same reattach. A same-size card swapped in a USB
  reader would look the same to it, which is why the writer refuses without
  diskseq unless given `--weak-identity`.
- **Checking that the write landed on the confirmed device.** diskseq is
  never reused. The writer opens its descriptors, checks that they are the
  device the name refers to, and checks that the name still has the confirmed
  diskseq; after each write step it checks the diskseq again. Nothing that
  writes the whole disk looks up its name in `/dev` (third review,
  2026-10-13; was: `wipefs` by path between two identity checks). That is
  detection after each step, not prevention: see "Writing through
  descriptors, and what the rechecks prove" below.
- **Never re-resolve the argument.** `/dev/disk/by-label/<label>` is made
  from content and disappears when the wipe erases that content. The writer
  resolves its argument once, at the start.

## Writing through descriptors, and what the rechecks prove

Third review, 2026-10-13. Measured with strace in the same container
(util-linux 2.41.5, e2fsprogs 1.47.2, perl-base 5.40), by
`tools/test-write-usb.sh`.

- **bash's `<>` adds O_CREAT.** strace of the old `exec 3<>"$DEV"`:
  `openat(..., "/dev/loopN", O_RDWR|O_CREAT, 0666)`. Had the node vanished
  in the window, that would have made a regular file in `/dev` and written
  the image into RAM. The writer now opens fd 4 by path with `<` (O_RDONLY,
  no O_CREAT), checks it, and opens fd 3 as `/proc/self/fd/4`. The trace
  shows `"/proc/self/fd/4", O_RDWR|O_CREAT` -- harmless, the magic link
  always exists while fd 4 is open -- and no open of `/dev/loopN` with
  O_CREAT or for writing at all.
- **`/proc/self/fd/N` avoids `/dev`, not a fresh open.** A child inherits
  fd 3; opening `/proc/self/fd/3` follows the magic link to the device inode
  and opens it again. For a block device that open goes through
  `blkdev_open`, which looks the disk up by the inode's `i_rdev`: it reaches
  whatever disk holds that major:minor *at that moment*, not necessarily the
  one fd 3 is on. (Code review, 2026-10-13: the earlier wording here, "reopens
  that inode, whatever `/dev` says now", overstated it.) Used for `sfdisk
  --append` (opens it `O_RDWR|O_EXCL`), `mkfs.ext4` (`O_RDONLY|O_EXCL`, then
  plain opens), `mount -t ext4` (source shown as the real partition),
  `blkid`, `blockdev --flushbufs`, the wipe, and fd 3 itself (opened as
  `/proc/self/fd/4`).
- **What the rechecks prove.** diskseq is assigned once per disk, bumped on
  a media change, and never reused, and a disk keeps its major:minor while it
  exists. So a fresh open of that major:minor that happens between two
  identity checks which both find the confirmed diskseq reached the confirmed
  medium. The writer rechecks after opening fds 3 and 4, after the wipe,
  after dd and before sfdisk, and before formatting the partition.
- **What they do not prove.** A recheck that fails says only that the device
  changed since the last one passed. The step in between may already have
  written to whatever took the number; the run stops after the fact, with a
  message saying what was done. Held descriptors narrow the window but do not
  close it: `dd` and `cmp` use fds 3 and 4 directly, and a held descriptor
  stays on its disk (writes fail if the stick is unplugged rather than follow
  the number to a newcomer), but a card swapped in a reader is a new medium
  behind the same disk, and the descriptor reaches it. That is why the writer
  also reads the diskseq right after `dd`: if the read-back then fails, the
  message says "the device changed or the stick is faulty" instead of
  blaming the stick. Not tested: a container cannot swap a medium under a
  loop device between `dd` and `cmp`.
- **sfdisk runs with `--lock=no`.** Its open of `/proc/self/fd/3` is a new
  open file description, so had it taken its own `flock` (it does when
  `LOCK_BLOCK_DEVICE=1` is in the environment, which `sudo -E` keeps) it
  would wait forever on the lock fd 3 holds. Measured with util-linux 2.41.5
  on a file held with `flock -x`: `LOCK_BLOCK_DEVICE=1 sfdisk ...` prints
  "device already locked, waiting to get lock" and is killed by `timeout 5`;
  with `--lock=no` it exits 0. The test now writes once with
  `LOCK_BLOCK_DEVICE=1` exported.
- **The wipe is perl on `/proc/self/fd/3` with O_RDWR|O_EXCL.** It zeroes the
  first and last MiB (the reasons for each region are in `wipe_signatures`).
  The exclusive open keeps the property `wipefs` had: refused if anything
  claimed the device since the in-use check. The test seeds a pattern into
  the last MiB of a used GPT stick and finds it gone, along with the backup
  GPT header, after the write and after a rewrite.
- **flock.** The writer holds `flock -x` on fd 3 from before the wipe until
  the partition table is written (trace: `flock(3, LOCK_EX)` before the
  wipe's open, `LOCK_UN` after sfdisk), so udev does not re-probe or
  automount mid-write. It is released before the partition step, because
  `udevadm settle` would otherwise wait on events udev defers while the lock
  is held. With another process holding `flock -x` on the device during the
  prompt, the writer gives up after 10 s and writes nothing (tested). That
  udev honours the lock is systemd's documented behaviour, not observed
  here: the container runs no udev.
- **The partition is treated like the disk.** After `find_partition`, fd 5 is
  opened on the partition node, checked against the major:minor sysfs lists
  for that partition under the confirmed disk, the disk's diskseq is
  rechecked, and `mkfs.ext4` and `mount` go through `/proc/self/fd/5`.
- **errno, not "in use".** The probe, and the wipe's exclusive open after the
  prompt, report EBUSY as in use and everything else by its errno text (the
  wipe's open used to call any failure "claimed by something after the
  checks"). Measured: a missing path gives "No such file or directory";
  a node for sd major 8 minor 255 with no disk behind it gives "No such
  device or address". ENOMEDIUM (a reader with no card) cannot be produced
  in a container.
- **Write-protected.** A read-only loop (`losetup -r`) shows `ro` = 1 in
  sysfs, like an SD card with its lock switch on; the writer now refuses it
  before the prompt by saying so. The check runs after the transport check,
  because Docker Desktop's own VM disks `sda` and `sdb` also report `ro` = 1
  and should be refused as what they are.

## Re-reading the table while the fresh image is mounted

On a desktop, udev re-reads the partition table as soon as `dd` closes the
device, and udisks then mounts the image. After that:

| State | `blockdev --rereadpt` | `sfdisk` without `--no-tell-kernel` | `partx --update --nr 3` |
|---|---|---|---|
| partition 1 mounted | EBUSY | — | p3 added, start correct |
| whole disk mounted | EBUSY | exits 0, **p3 not added** | p3 added, start correct |
| stale p3 from the previous write, p1 mounted | EBUSY | — | p3 correct |
| nothing mounted | works | — | — |

`partx -u` without `--nr` also added p3, but it dropped p1 from the
kernel's view while the whole disk was mounted. The writer therefore tries
BLKRRPART first and falls back to `partx --update --nr 3`.

Not verified: with the *whole disk* mounted, formatting p3 should fail,
because the whole-disk claim blocks exclusive opens of its partitions. The
writer reports that case by name, but it was not run.

## Not verified

- A physical stick. Everything here is a loop device or a QEMU usb-storage
  device. Real sticks differ in removable flag, `TRAN` and sector size.
- Boot on physical firmware, whether BIOS or UEFI. QEMU's SeaBIOS and edk2
  both boot the written image unattended (see the bootloader-timeout
  postmortem of the same date), but real firmware is pickier about hybrid MBRs.
- Write and read speed of a USB stick under a recording load (ADR-0005
  already doubts it).
