# Image build

`live-build` configuration for the bootable SonoraOS image. See ADR-0002 and
ADR-0005.

    tools/build-iso.sh        # produces iso/sonora.iso
    sudo tools/write-usb.sh /dev/sdX

## How the pieces fit

| Path | Role |
|---|---|
| `auto/config` | The `lb config` arguments, in the repository so the build is reproducible |
| `config/package-lists/sonora.list.chroot` | What goes in the image |
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
   shell.

## Display stack (ADR-0010, stage 1)

Off by default. With `sonora.gui` on the kernel command line,
`sonora-compositor.service` runs labwc as a kiosk on tty1, pulls in
`sonora-ui.service` (`sonora-ui --kiosk --demo`, fake engine) and
`sonora-session.service` stands down. Five failures in 60 s, or the UI's exit
code 3, run `sonora-display-fallback.service` (`chvt 2`). `sonora-ui` is not
yet staged by `tools/build-iso.sh`; until it is, the UI unit fails and falls
back.

## What is not verified

The image builds and boots in QEMU, both from the ISO (`tools/test-boot-qemu.sh`)
and from a stick image written by `tools/write-usb.sh`, through both bootloaders
(`tools/test-write-usb.sh`). It has never booted on a physical machine or from a
physical stick. See `planning/STATUS.md`.
