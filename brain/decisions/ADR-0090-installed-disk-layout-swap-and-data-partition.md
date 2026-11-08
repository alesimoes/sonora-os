# ADR-0090 - Installed disk layout: separate data partition, sized swap partition

- Status: Accepted (recommended defaults, architect 2026-11-08)
- Date: 2026-11-08
- Item: `system.install.rootfs`
- Relates to: ADR-0032 (installer; this fills in the layout table its step 2
  leaves to this item), ADR-0010 (OnFailure fallback), `audio-stack/sysctl-sonora.conf`

## Summary

`sonora-install` lays the disk out as BIOS-boot (BIOS only), ESP 512 MiB, root
ext4 16 GiB, swap partition `clamp(RAM/2, 2, 8 GiB)`, and `sonora-data` ext4
for the rest, mounted at `/home/sonora/Sessions`. No `nofail` on the data
mount: a missing data partition stops boot loudly. No zram, no swap file.
Layout arithmetic lives in a sourced, testable library, not in the 416-line
script.

## Context

ADR-0032 installs one root filesystem with sessions under it and defers a data
partition, swap and journal tuning to this item. Three things matter to a
recorder: a full session disk must not be able to break boot or the system;
memory pressure must not kill the engine mid-take; and nothing may add a disk
read inside the audio callback.

## Decision

1. **Layout, in this order.** (BIOS boot 1 MiB, BIOS machines only,) ESP FAT32
   512 MiB, `sonora-root` ext4 16 GiB, `sonora-swap`, `sonora-data` ext4 (the
   rest). Data is last so it takes whatever is left. Disk smaller than
   ESP + root + swap + 8 GiB data is refused before anything is written (exit 4,
   named reason).
2. **Root fit is checked, not hoped.** Before copying, the squashfs's
   uncompressed size times 1.5 must fit the root partition, else exit 4.
3. **Swap is a partition.** Size `clamp(RAM_MiB / 2, 2048, 8192)`, rounded down
   to 256 MiB. `vm.swappiness = 10` (already shipped) keeps it a last resort.
   No hibernation: no `resume=`. Reason for having swap at all: an OOM kill of
   the engine loses the take; a swapping box is slow but the take survives.
4. **Data partition.** `mkfs.ext4 -m 1 -T largefile -L sonora-data`; fstab
   `UUID=... /home/sonora/Sessions ext4 noatime,errors=remount-ro 0 2`. The
   journal stays on (ordered): a crash must not cost the session. No `discard`
   mount option (latency); `fstrim.timer` is enabled instead. Root keeps
   `noatime,errors=remount-ro`.
5. **Fail loudly.** The data mount has no `nofail`, and the engine unit gets
   `RequiresMountsFor=/home/sonora/Sessions`. Otherwise a missing partition
   lets the engine record into the hidden mount-point directory on root, which
   is silent degradation. Boot goes to the ADR-0010 fallback instead.
6. **Self-check grows.** fstab names root, ESP, swap and data UUIDs and all
   resolve; the data partition is mounted at the Sessions path and holds the
   carried-over session with matching checksums; `swapon --show` lists the
   swap partition after the first boot in the QEMU test.
7. **Code shape.** Size arithmetic and the sfdisk layout text live in
   `usr/local/lib/sonora/install-layout.sh` (pure functions, no disk access),
   sourced by `sonora-install`. A unit test runs it with fake RAM and disk
   sizes. Exit codes and `STEP n/N` lines are unchanged except `N`.

## Rationale

- *Cost later.* A single root means a session that fills the disk also fills
  `/var` and `/tmp`; the OS breaks mid-take. A separate data partition costs a
  fixed 16 GiB root and an installer that refuses very small disks. Accepted.
- Data separate also lets a later read-only root (`system.hardening.ro_root`)
  keep exactly one writable place for sessions.
- A swap file on the data or root filesystem competes with recording writes and
  needs the filesystem mounted first; a partition is independent of both.
- Keeping ext4 (ADR-0032) with `-m 1` avoids the 5% reserve wasting tens of GiB
  on a large drive; the reserve protects nothing on a data-only filesystem.

## Consequences

- `sonora-install` partition/format/configure/self-check change; fstab has four
  lines; carry-over targets the data partition.
- `tools/test-installer.sh` and `tools/test-install-qemu.sh` extend to the new
  layout. Installed disks made before this ADR are not migrated (the installer
  is new; nothing ships yet).
- 16 GiB root is a guess about image growth; step 2's fit check makes a too-big
  image fail at install, not at 3 a.m.

## Unverified

- That ext4 on a separate data partition sustains 32 tracks x 24-bit x 96 kHz on
  the target SSD/HDD (assumption). Measurement for the researcher: `fio`
  sequential 32-stream write on a data partition built exactly this way, on the
  UMC1820 laptop's disk, with the engine's dirty-page settings.
- That the engine locks its working set (`mlockall`), so swap cannot page the
  callback. Measurement for the researcher: `grep VmLck /proc/<engine>/status`
  during a session, and a stress run with `stress-ng --vm` while recording.
- That RAM/2 is enough swap on 4 GiB machines. Not measured.

## Alternatives rejected

- *Single root, no swap* (ADR-0032 as written): a full session disk or a
  memory spike takes the system or the take with it.
- *zram swap:* no disk I/O, but it takes RAM from the plug-in budget and adds
  CPU decompression on page-in, which can land in a callback. Rejected until a
  measurement says otherwise.
- *Swap file on root:* shares the journal and the writes with recording.
- *`nofail` on the data mount:* boots a machine that records into the wrong
  directory. Silent degradation is the worst outcome in the product.
- *XFS or btrfs for data:* a different filesystem to carry through the
  installer, initramfs and fsck tooling before any measurement says ext4
  falls short.
