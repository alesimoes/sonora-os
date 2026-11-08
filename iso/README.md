# Image build

`live-build` configuration for the bootable SonoraOS image. See ADR-0002 and
ADR-0005.

    tools/build-iso.sh        # produces iso/sonora.iso and sonora.iso.sha256
    sudo tools/write-usb.sh /dev/sdX                 # from Linux
    tools/make-stick-image.sh; tools\write-usb.ps1   # from Windows, below

## Writing the stick from Windows

`tools/write-usb.ps1` (Windows PowerShell 5.1, nothing to install) copies an
image to the stick byte for byte. It cannot make the ext4 persistence
partition, so that part is done first, in Docker, by `tools/write-usb.sh`
itself against a file: `tools/make-stick-image.sh` produces
`iso/sonora-stick.img` (the ISO, then a 4096 MiB persistence partition) and
its `.sha256`. Without it the script writes `iso/sonora.iso`, which boots but
keeps nothing across a reboot.

From Git Bash, with Docker Desktop running:

    tools/build-iso.sh
    tools/make-stick-image.sh                          # or --persistence-mib 16000

Then plug in the stick and open **PowerShell as administrator** in the
repository:

    powershell -ExecutionPolicy Bypass -File tools\write-usb.ps1 -SelfTest
    powershell -ExecutionPolicy Bypass -File tools\write-usb.ps1 -List
    powershell -ExecutionPolicy Bypass -File tools\write-usb.ps1 -DiskNumber N -DryRun
    powershell -ExecutionPolicy Bypass -File tools\write-usb.ps1 -DiskNumber N

`-SelfTest` runs `tools/test-write-usb.ps1`, the whole flow against fake disks
(regular files). `-List` checks the image against its sidecar and lists only
USB disks, each marked "can be written", "has data" (with each volume's
letter, filesystem, label, size and used space) or with the reason it is
refused: the system or boot disk, a disk with a page file, a BitLocker
volume, a Windows installation or boot loader, a system or boot partition,
the disk the image is on, any bus but USB (an internal SD reader or eMMC
included), over 256 GiB without `-AllowLarge`, too small, or sectors other
than 512 bytes.

`-DryRun` says so first, goes through every check, shows the target and the
text a real run would ask for, and asks for nothing. The last command shows
the disk's model, size, serial and partitions, says what will be erased, and
asks you to type `ERASE N XXXX`, where `XXXX` is the last four letters or
digits of that disk's serial. A disk that reports no serial gets its model
and exact size in bytes instead, and can only be confirmed at the prompt.
A disk that holds data -- any volume with a drive letter or a filesystem
Windows reads, or a large partition it cannot read -- also asks for
`ERASE DATA E` (its drive letters, or `P1P2...` without letters), and
`ERASE DATA DISK` when Windows cannot list its partitions or it has none but
its first MiB is not zero (a filesystem on the whole device). Only a blank
stick (no partitions, first MiB zero) or one this script wrote before needs
just the first: that is recognised by its bytes (our MBR, the ISO label
`SONORA`, an ext4 partition 3 labelled `persistence`), read read-only, and
with nothing larger than the EFI partition carrying a letter or a
filesystem Windows reads. Anything
else stops it. A script passes the first text with `-Token "ERASE N XXXX"`
and accepts the data loss with `-AllowDataVolumes`; without them a
non-interactive run is refused. A misspelt parameter (`-DryRn`) is an error
before anything is shown, never a real run.

**After writing:** Windows sees the ext4 persistence partition as
unformatted and may offer to format it. Click Cancel. Formatting it erases
the persistence partition, and the stick then boots without it.

The write locks and dismounts every volume on the stick, zeroes its first and
last MiB, writes the image from the second MiB on and the first MiB last (an
interrupted write leaves a blank stick, not half an image with a valid
partition table), checks the SHA-256 of what it read against the sidecar
before that last step, flushes, and reads back the first and last 64 MiB and
the zeroed last MiB through an unbuffered handle. Before the first byte it
asks the open device handle for its length, bus and serial, which must match
the disk that was confirmed.

Exit code 0 means: every write completed with no short write, the image
matched its sidecar, and the first 64 MiB, the last 64 MiB and the zeroed
last MiB of the disk read back correctly. That is a sample. The middle of the
image is not read back, so a bad block 1 GiB in still exits 0. The real check
is booting it: `tools/test-write-usb.sh` boots a written image in QEMU, and
for the stick itself, boot a machine from it. If the stick does not boot,
write it again (another port or stick if it fails twice). Exit code 1 is an
error; 2 is a refusal, nothing written.

Why not WSL: `wsl --mount` does not take USB sticks, and passing one through
needs usbipd-win installed on the host.

## How the pieces fit

| Path | Role |
|---|---|
| `auto/config` | The `lb config` arguments, in the repository so the build is reproducible |
| `config/package-lists/sonora.list.chroot` | What goes in the image |
| `config/archives/sonora-backports.*.chroot` | PipeWire and WirePlumber from a dated trixie-backports snapshot, at exact versions (build-time only) |
| `config/archives/sonora-snapshot.conf.chroot` | Turns off apt's Release expiry check for the dated snapshot (build-time only) |
| `config/includes.chroot/` | Files copied into the filesystem verbatim |
| `config/hooks/normal/` | Scripts run inside the chroot at build time |
| `config/bootloaders/` | Overrides for live-build's isolinux and GRUB templates: a 5 s menu timeout instead of waiting forever |

`config/includes.chroot/usr/share/sonora/` holds the tuning files, which the
hook then installs to their real locations. They are copied rather than placed
directly so that `audio-stack/` stays the single documented source for them —
`tools/sync-audio-stack.sh` refreshes the copies.

## Boot flow

1. ISOLINUX (BIOS) or GRUB (UEFI) shows its menu for 5 seconds, then loads
   the RT kernel with `threadirqs` and `persistence`. live-boot mounts the
   stick's partition 3, label `persistence`, as an overlay on `/home`.
2. `multi-user.target`. No display manager.
3. The `sonora` user lingers, so the PipeWire user session starts at boot.
4. `sonora-session.service` takes tty1, waits for the audio server, and
   launches the engine. It restarts on exit, so the operator never reaches a
   shell. logind starts no getty on the other VTs (`NAutoVTs=0`), so
   Ctrl+Alt+F2 shows an empty console, not a passwordless login.

## Versions

Everything comes from trixie except PipeWire 1.6.9 and WirePlumber 0.5.12,
which come from trixie-backports. All of it, trixie, trixie-updates,
trixie-security and backports, is read from one
`snapshot.debian.org` timestamp, `20261101T000000Z` (ADR-0002), so two
builds of one commit get the same RT kernel. The shipped system's own
sources name the live `deb.debian.org` and `security.debian.org`. To move the
base and the kernel: change `SONORA_SNAPSHOT` in `auto/config` and the date in
`config/archives/sonora-backports.list.chroot` together;
`tools/test-image-config.sh` fails while they differ, and `tools/build-iso.sh`
drops its package cache on the next build. To move PipeWire: change the snapshot
date (both places), both versions in
`config/archives/sonora-backports.pref.chroot` and the two
`SONORA_*_VERSION` lines in the hook, in one commit, then run
`tools/test-image-config.sh` and `tools/test-quantum-pin.sh`.
`tools/Dockerfile.runtime` reads the same two files, so the container tests
follow.

Why WirePlumber is pinned to a backport: trixie ships 0.5.8 with PipeWire
1.4.2. WirePlumber 0.5.12 travels with PipeWire 1.6.9 from trixie-backports
(ADR-0002 takes PipeWire from backports), and `brain/research/quantum-pin.md`
("Re-run on PipeWire 1.6.9") re-ran the quantum pin on that pair. Backports
keeps only its newest upload, so the pin resolves only through the dated
snapshot, and the hook fails the build on any other version.

## Build robustness

- `tools/build-iso.sh --check` resolves every package list entry, the
  PipeWire/WirePlumber pins and `linux-image-rt-amd64` in a `debian:trixie`
  container with `apt-get -s`, in about a minute, and builds nothing. A normal
  build runs the same check first, so an unreachable snapshot or a pin the
  snapshot no longer has stops the build in a minute with a named cause, not
  after the bootstrap. Fetches retry with backoff (5, 15, 45 s;
  `SONORA_RETRY_DELAY` changes the first step).
- When snapshot.debian.org is down for hours, a development build can use
  another timestamp: `SONORA_DEV_SNAPSHOT=20261025T000000Z tools/build-iso.sh`.
  The pins must still resolve there (the check says if not). The result is
  `iso/sonora-dev.iso`, its BUILD_ID ends in `-dev`, and `SONORA_RELEASE=1`
  refuses the combination. The committed timestamp is unchanged.

## Display stack (ADR-0010, stage 1)

Off by default. With `sonora.gui` on the kernel command line,
`sonora-compositor.service` runs labwc as a kiosk on tty1, pulls in
`sonora-ui.service` (`sonora-ui --kiosk --demo`, fake engine) and
`sonora-session.service` stands down. Five failures in 60 s, or the UI's exit
code 3, run `sonora-display-fallback.service`. It creates
`/run/sonora-display-failed`, stops the compositor and the UI, and starts
`sonora-session.service`, whose conditions are `!sonora.gui` *or* that flag,
so tty1 gets the engine and its terminal UI as on a plain boot. The flag is on
tmpfs, so the next boot tries the display again. (ADR-0010's `chvt 2` to a
terminal UI on tty2 is stage 2.) `tools/build-iso.sh` stages `sonora-ui` only
with `SONORA_WITH_UI=1`; without it the UI unit fails and falls back.
`tools/test-display-fallback.sh` runs this hand-back under systemd in a
container.

## Installing to disk (ADR-0032)

The installed system is the target; the stick is for trying it. Boot the stick
with `sonora.install` added at the boot menu (press `e` in GRUB, `Tab` in
ISOLINUX): `sonora-installer.service` puts a text screen on tty2
(`sonora-install-tty`) and switches to it. It lists the internal disks,
asks for the disk's name to be typed, runs `sonora-install --yes --target`,
shows `STEP n/7` and ends with INSTALLED or the named failure. Sessions on the
stick are copied to the new system and checked file by file with SHA-256; the
stick's copy is never removed. The unit does nothing on an installed system.

Everything the screen does goes through the script's contract, so the later Qt
page in `sonora-ui` replaces `sonora-install-tty` and nothing else:
`sonora-install --list` (`DEVICE<TAB>BYTES<TAB>MODEL`), the `STEP n/7 name` and
`INFO ...` lines on stdout, and the exit codes in the script's header (11 is a
session carry-over that did not verify). A page asks for the typed device name
itself before passing `--yes`.

Tests: `tools/test-installer-tty.sh` (the screen, a stub installer, no root),
`tools/test-installer.sh` (the script against a file image, privileged
container), `tools/test-install-qemu.sh` (the exit test: install to a virtio
disk from the ISO, restart without the stick, find the session). The last two
have not been run in CI; see the PR that added them.

### Installed layout (ADR-0090)

Partitions on the installed disk, in order (GPT, 1 MiB aligned):

| # | Label | Size | Filesystem | Mount |
|---|---|---|---|---|
| 1 | bios (BIOS installs only) | 1 MiB | none | none |
| 2 | ESP | 512 MiB | FAT32 | `/boot/efi` |
| 3 | sonora-root | 16 GiB | ext4 | `/` |
| 4 | sonora-swap | clamp(RAM/2, 2 GiB, 8 GiB), rounded down to 256 MiB | swap | enabled at boot |
| 5 | sonora-data | the rest (at least 8 GiB) | ext4, `-m 1`, largefile | `/home/sonora/Sessions` |

- **Swap.** The installer formats swap but never enables it on the machine it
  runs on, so the installing host is not paged onto the target disk. The
  installed `fstab` enables it at boot. An engine killed by the OOM killer loses
  the take, so swap is sized to keep that from happening on small RAM.
- **Minimum disk.** The installer refuses a disk too small for the layout before
  it erases anything (exit 4) and prints the smallest size for that RAM.
- **Data mount has no `nofail`.** Sessions are the product. Without the data
  partition the engine must not record into the empty mount point on root, so
  the `fstab` line is not optional and the engine unit carries a
  `RequiresMountsFor=` drop-in. The install self-check refuses a `fstab` whose
  data line says `nofail`.
- **When the data partition is missing** (disk swapped, partition deleted, or
  its UUID no longer resolves): boot waits for it, then systemd drops to
  emergency mode, a root shell. This follows from the missing `nofail` and is
  deliberate: the box does not record into the root disk. Recover from that
  shell: `blkid -L sonora-data` finds the partition; if it is on the disk, run
  `mount LABEL=sonora-data /home/sonora/Sessions` and then `systemctl default`.
  If the partition is gone, the sessions on it are gone: reinstall with
  `sonora-install`, which erases the target disk. Do not reformat a disk you
  have not identified.
- The arithmetic is `install-layout.sh`; its tests are
  `tools/test-install-layout.sh` and `tools/test-installer.sh` (privileged
  container). This layout is not yet checked on hardware or under QEMU.

## Hardware acceptance

One session on the real laptop with the UMC1820 gives evidence for
system.boot_tests.hardware, system.hardening.soak, system.hardening.crash_recovery,
drivers.umc1820.* and system.usb.physical. Nothing here has been run on
hardware yet; `tools/test-sonora-acceptance.sh` checks the kit's decisions
against a fake machine only.

1. Write the stick (`tools/write-usb.ps1`), plug in the UMC1820, boot the laptop
   from the stick. At the boot menu press `e` (GRUB) or Tab (ISOLINUX) and add
   `systemd.debug-shell=1` to the kernel line, then boot. The engine starts as
   usual; press Ctrl+Alt+F9 for a root shell.
2. `sonora-acceptance quick` (about 2 minutes). Prints PASS, FAIL or SKIP per
   check with its evidence: RT kernel, `threadirqs`, rtprio and memlock, CPU
   governor, UMC1820 USB ID and 18/20 channels, PipeWire pinned at 128/48000,
   a 60 s 18-channel take with its xrun count, and the take played back.
3. `sonora-acceptance soak 8` (or the hours you have). Logs a line a minute to
   `/home/sonora/Acceptance/soak-*.log`, stops itself if free space drops under
   2 GB, prints a summary.
4. `sonora-acceptance powercut`, wait for the notice, pull the power (charger
   out, battery out or hold the power button), boot again with the same
   `systemd.debug-shell=1`, then `sonora-acceptance recover-check`.
5. `sonora-acceptance report`. Copy
   `/home/sonora/Acceptance/report-*.md` off the stick (it lives on the
   persistence partition) into `brain/research/` and open the PR. The report
   names the machine, the kernel and command line, and every result.

`quick` and `soak` stop `sonora-session.service` and start it again at the end.
For the quanta below 128 use `tools/umc-measure.sh`.

## What is not verified

The image builds and boots in QEMU, both from the ISO (`tools/test-boot-qemu.sh`)
and from a stick image written by `tools/write-usb.sh`, through both bootloaders
(`tools/test-write-usb.sh`, on every merge to main in CI without the boots). It has never
booted on a physical machine or from a physical stick, and
`tools/write-usb.ps1` has written only fake disks (regular files), never a
real stick. See `planning/STATUS.md`.
