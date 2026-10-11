# 2026-10-11 — the image booted the non-RT kernel by default

## Symptom

None. That is the point.

The first complete ISO built cleanly, contained the engine, enabled the
service, installed every tuning file, and would have booted and recorded
audio. Inspecting it found two kernels:

    /live/vmlinuz-6.12.111+deb13-amd64      <- default boot entry
    /live/vmlinuz-6.12.111+deb13-rt-amd64   <- secondary menu entry only

Both the ISOLINUX default label and the first GRUB menuentry pointed at the
generic kernel. `/live/vmlinuz` was a copy of the generic one.

## Root cause

`linux-image-rt-amd64` was listed in `config/package-lists/sonora.list.chroot`.
That installs the RT kernel into the image, which is why it was present, but
the kernel live-build treats as *the* kernel comes from `--linux-flavours`,
which still defaulted to `amd64`. live-build therefore also pulled the generic
kernel, made it the bootstrap flavour, and built the boot entries around it.

Installing a kernel and booting it are different things, and the package list
only does the first.

## Fix

`--linux-flavours rt-amd64` in `iso/auto/config`, and the package list entry
removed so there is one source of truth. The rebuilt image contains only the
RT kernel and both bootloaders default to it. The ISO also dropped from 1.2 GB
to 867 MB, which is the second kernel no longer being there.

## Why this one matters more than the others

Every other defect so far announced itself: a compile error, a failed test, a
build that stopped. This one produced a working appliance. It would have
booted, started the engine, armed tracks and written valid files. The only
symptom would have been xruns during a real session, with the whole latency
premise of the project quietly absent and nothing in the system saying so.

A test for "does it boot and record" would have passed.

## Guard against recurrence

The boot smoke test already in the backlog is not enough -- it would have
passed. It must also assert the running kernel:

    uname -r | grep -q -- -rt-

Added to the backlog as part of that item. Until it exists, the check is
manual: after any change to `auto/config`, list the kernels in the built ISO
and read the default boot entry.
