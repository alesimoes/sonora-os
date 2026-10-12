# 2026-10-12: tools/write-usb.sh could not have worked, and its guard was blind

## Symptom

None yet, because the script had never been run. `planning/STATUS.md` said
so. The first run, against a loop device over a disk image in a privileged
container, is what found all of this.

## What the script would have done

On any USB stick, in order:

1. `dd` the ISO over the stick. Fine.
2. `parted -s "$DEV" mkpart ...`, which fails with an unrecognised label
   because libparted reads xorriso's Apple partition map before the MBR. Under
   `set -e` the script stops here. **The stick is erased and has no
   persistence partition.** Observed, not inferred.
3. If parted had worked, the new partition would have been number 3, because
   the ISO's MBR already holds the ISO (1) and its EFI system partition (2).
   The script then formatted `${DEV}2`, **the EFI partition**, as ext4 and
   labelled it `persistence`. The stick would have stopped booting under UEFI
   and gained a persistence partition only by accident.

And before any of that, the guard that protects other disks:

4. "Looks like a partition" was the regex `[0-9]$`. It rejects whole disks:
   `/dev/mmcblk0`, every loop device, and `/dev/disk/by-id/usb-...-0:0`, the
   safest way to name a stick. It also accepts partitions:
   `/dev/disk/by-label/persistence` is partition 3 of a SonoraOS stick, and
   its name ends in a letter.
5. The only other guard was "nothing on it is mounted", read through
   `lsblk -no MOUNTPOINTS | grep -q .`. Two flaws:
   - From a container, or any mount namespace other than the host's, the
     host's mounts are invisible. Pointed at every disk Docker Desktop
     exposes, the script reached the ERASE prompt for three of them, one
     the VM's 256 GB disk. (The prompt was answered "no".)
   - If `lsblk` fails, for example on util-linux older than 2.37, which has
     no `MOUNTPOINTS` column, the pipe reads as "nothing mounted" and the
     guard passes.

## Root cause

The script was written from how a plain ISO and a plain stick behave, and an
isohybrid ISO is neither. Nothing ever ran it. A script whose failure mode is
erasing the wrong disk cannot be validated by reading it.

## Fix

`tools/write-usb.sh`:

- Asks the kernel what the device is (`lsblk TYPE`), rather than parsing the
  name, and resolves symlinks first.
- Writes only to devices reporting `TRAN=usb`, `TRAN=mmc` or `RM=1`.
- Refuses mounted devices with the lsblk output captured, so an lsblk failure
  stops the script instead of passing it. Also refuses devices with holders
  (LVM, RAID, dm-crypt), devices with sectors other than 512 bytes, and
  devices too small for the image plus 256 MiB.
- Wipes old signatures first, so no stale backup GPT survives at the end of
  the stick.
- Reads the image back and compares it, before partitioning.
- Adds partition 3 with `sfdisk --append`, finds it by start sector through
  sysfs, and checks the label after formatting.
- Accepts a loop device only when `SONORA_WRITE_USB_TEST_IMAGE` names the
  exact file behind it. In that mode it refuses everything else, real disks
  included.

## Guard against recurrence

`tools/test-write-usb.sh` runs the real script. It asserts each refusal,
including the container's root disk and every other disk Docker exposes.
Each refusal attempt answers "no" at the prompt, so a broken check cannot
write; reaching the prompt is itself the failure. It then writes a used stick
(GPT, ext4) and a SonoraOS stick, and checks the layout byte for byte. Finally
it boots the result five ways, including a reboot that must find a marker the
previous boot wrote.

## Review of the fix, same day

The code review of the first fix found the guard still too trusting. All of
these are fixed and tested in `tools/test-write-usb.sh`:

- **eMMC passed as a card.** `TRAN=mmc` was accepted, and lsblk sets it for
  soldered eMMC too. A mini-PC booted from the stick would have offered its
  Windows disk, protected only by the prompt. Now only an MMC device of
  type `SD` is accepted.
- **Nothing pinned the device across the prompt.** The checks ran, a human
  read the prompt, and then `wipefs` and `dd` reopened whatever the name
  meant by then. Now diskseq is recorded before the prompt; after it, the
  identity, the kind checks and the in-use checks all run again, and `dd`
  and the read-back go through descriptors that are opened once and checked.
  The test reattaches the loop device while the script waits at the prompt,
  answers ERASE, and asserts that nothing was written.
- **Mounts in other namespaces were invisible.** Now an `O_EXCL` probe
  (`wipefs --no-act`) runs before the prompt and again after it, and the
  first write, `wipefs` without `--force`, is exclusive itself. The test
  mounts the stick inside `unshare -m`.
- **BLKRRPART fails on desktops,** where udisks has already mounted the fresh
  image, leaving a stick without persistence and an unexplained error. The
  fallback is `partx --update --nr 3`. The test drives the script's own
  function with partition 1, the whole disk, and a stale partition 3 each
  held.
- **External drives were accepted,** with only the prompt as protection.
  Anything over 256 GiB now needs `--allow-large`, and the prompt shows the
  size.
- **The test could offer a real stick.** On a native Linux host its refusal
  loop would have pointed the script at genuine USB devices. Those are now
  listed and skipped.

What generalises: a check run before an interactive pause protects only the
moment it ran. Anything that matters must be checked again after the pause,
against an identity rather than a name.

## Second review

- The in-use probe added in the first review, `wipefs --no-act --all`,
  issued BLKRRPART. "No-act" applies to erasing, not to re-reading the
  table. The probe meant to stop an automounted stick being written could
  itself cause the automount. It is now a read-only `O_EXCL` open by perl,
  and the test straces everything before the prompt: no BLKRRPART, no
  write-mode open of the device or its partitions.
- The first write was not bracketed by identity checks, and failures after
  the first write still said "Nothing was written". Every message now says
  what has already been done to the device.
- Re-resolving the argument after the prompt broke content-based links
  (`by-label`), which `wipefs` itself removes.
- Without diskseq, the identity check cannot see the swaps it exists for, so
  the writer now refuses unless the user passes `--weak-identity`.

Lesson: read the man page for "no-act" as narrowly as it is written, and
strace any tool used as a probe.
