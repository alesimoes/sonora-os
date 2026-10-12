# 2026-10-12: the image waited at the boot menu forever

## Symptom

None that any test could see. On a real machine with nobody at the keyboard,
the appliance would have stopped at the isolinux or GRUB menu and never
started the engine.

## Root cause

live-build's isolinux template (`/usr/share/live/build/bootloaders/isolinux/
isolinux.cfg`, live-build 1:20250505+deb13u1) says `timeout 0`. In syslinux
that means no timeout: wait for a keypress. Its GRUB template
(`grub-pc/config.cfg`, which the EFI `grub.cfg` also sources) sets no
`timeout`, and GRUB then waits indefinitely too. live-build has no option for
either, and Debian's live images are interactive, so the defaults are
reasonable for them and wrong for an appliance.

No test ran a bootloader unattended. `tools/test-boot-qemu.sh` boots with
`-kernel` and never executes one. `tools/run-qemu.sh` does, but a human
presses Enter without noticing.

The "waits forever" behaviour comes from syslinux's and GRUB's documented
semantics. It was not observed by booting the old image. The fixed behaviour
was observed.

## Fix

`iso/config/bootloaders/isolinux/isolinux.cfg` (`timeout 50`, in tenths of a
second) and `iso/config/bootloaders/grub-pc/config.cfg` (`set timeout=5`),
which live-build copies over its templates. The GRUB file is the template
plus one line, so a live-build upgrade shows up as a one-line diff. Both were
previously gitignored as live-build output; `.gitignore` now admits them.

## Guard against recurrence

`tools/test-write-usb.sh` boots the written stick through its own bootloader,
under SeaBIOS and under edk2, with no input and no added kernel parameters,
then reads the persistence partition offline. The engine's session folder
must be there, which it can only be if the bootloader proceeded on its own
and the boot reached a running engine. Passed for both on 2026-10-12.

Also found on the way: `tools/run-qemu.sh uefi` passed edk2's code image to
`-bios`. QEMU rejects that file because it is not a multiple of 64 KiB, so the
UEFI path of that script had never started. It now uses pflash.
