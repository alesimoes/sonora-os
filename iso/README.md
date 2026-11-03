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
`ERASE DATA E` (its drive letters, or `P1P2...` without letters). A blank
stick, or one written by this script before, needs only the first. Anything
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
the disk that was confirmed. Exit code 0 is a verified stick; 2 is a
refusal, nothing written.

Why not WSL: `wsl --mount` does not take USB sticks, and passing one through
needs usbipd-win installed on the host.

## How the pieces fit

| Path | Role |
|---|---|
| `auto/config` | The `lb config` arguments, in the repository so the build is reproducible |
| `config/package-lists/sonora.list.chroot` | What goes in the image |
| `config/archives/sonora-backports.*.chroot` | PipeWire and WirePlumber from a dated trixie-backports snapshot, at exact versions (build-time only) |
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
which come from trixie-backports through
`snapshot.debian.org/archive/debian/20261101T000000Z`. The RT kernel is
trixie's. To move PipeWire: change the snapshot date, both versions in
`config/archives/sonora-backports.pref.chroot` and the two
`SONORA_*_VERSION` lines in the hook, in one commit, then run
`tools/test-image-config.sh` and `tools/test-quantum-pin.sh`.
`tools/Dockerfile.runtime` reads the same two files, so the container tests
follow.

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

## What is not verified

The image builds and boots in QEMU, both from the ISO (`tools/test-boot-qemu.sh`)
and from a stick image written by `tools/write-usb.sh`, through both bootloaders
(`tools/test-write-usb.sh`, nightly in CI without the boots). It has never
booted on a physical machine or from a physical stick, and
`tools/write-usb.ps1` has written only fake disks (regular files), never a
real stick. See `planning/STATUS.md`.
