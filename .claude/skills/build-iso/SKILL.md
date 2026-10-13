---
name: build-iso
description: Build the bootable SonoraOS ISO with live-build, and write it to a USB stick. Use for any change under iso/ or audio-stack/, and when asked to produce or test the image.
---

# Build the image

`live-build` needs Linux and root. It runs in a privileged container.

## Build

    tools/build-iso.sh

Output: `iso/sonora.iso`. A full build downloads a Debian base and takes tens
of minutes; the cache in `iso/cache/` makes later builds much faster.

## Write it to a USB stick

    sudo tools/write-usb.sh /dev/sdX

This destroys everything on the target device. Confirm the device with
`lsblk` and read the output back to the user **before** running it. Writing to
the wrong disk is unrecoverable, so never infer the device — have it confirmed.

The image's own MBR already holds partition 1 (the ISO) and partition 2 (the
EFI system partition, inside the ISO). The script adds partition 3, ext4,
labelled `persistence` (ADR-0005 and its correction,
`brain/research/usb-stick-layout.md`). Devices over 256 GiB need
`--allow-large`. It is tested without a real device by
`tools/test-write-usb.sh`.

## Boot testing

QEMU proves the image boots and little else. Audio latency, interface
detection and xrun counts are only meaningful on real hardware. Which QEMU
test to run, and when, is in the `test-loop` skill:

    tools/test-boot-qemu.sh     # headless, asserts, about 2 min
    tools/run-qemu.sh [uefi]    # a window, for a human to watch

What matters is the real boot. When you run one, write the result into
`brain/research/` with the machine, the interface, the buffer size and the xrun
count. Secure Boot must be off.

## Reporting

Distinguish built, booted in QEMU, and booted on hardware. They are three
different claims and only the last one means the image works.
