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

`config/includes.chroot/usr/share/sonora/` holds the tuning files, which the
hook then installs to their real locations. They are copied rather than placed
directly so that `audio-stack/` stays the single documented source for them —
`tools/sync-audio-stack.sh` refreshes the copies.

## Boot flow

1. ISOLINUX or GRUB loads the RT kernel with `threadirqs` and `persistence`.
2. `multi-user.target`. No display manager.
3. The `sonora` user lingers, so the PipeWire user session starts at boot.
4. `sonora-session.service` takes tty1, waits for the audio server, and
   launches the engine. It restarts on exit, so the operator never reaches a
   shell.

## What is not verified

Nothing in this directory has been built or booted yet. See
`planning/STATUS.md`.
