#!/usr/bin/env bash
# Write the image to a USB stick and add the persistence partition (ADR-0005).
#
# THIS ERASES THE TARGET DEVICE. Pass the whole disk, not a partition:
#   sudo tools/write-usb.sh /dev/sdb
#   sudo tools/write-usb.sh --allow-large /dev/sdb    # a device over 256 GiB
#
# Verified by tools/test-write-usb.sh, which runs this script against a loop
# device over a disk image and boots the result. See the comment on
# SONORA_WRITE_USB_TEST_IMAGE below for the one thing that test changes.
#
# The checks are the most important part of this file. Every one of them was
# added because the version without it would have erased a disk it should
# not have; tools/test-write-usb.sh asserts each refusal. They run twice:
# before the prompt, and again after it, immediately before the first write,
# because a human answering a prompt can take long enough for the device
# behind the name to change.

# The smallest persistence partition worth creating. Below this a session
# fills it in minutes, which is a worse failure than refusing now.
MIN_PERSISTENCE_MIB=256
# Above this the target is more likely an external backup drive than a stick,
# and the prompt alone is not enough protection. 256 GiB rather than GB so a
# stick sold as "256 GB" (about 256.1e9 bytes) still passes.
MAX_BYTES_WITHOUT_OVERRIDE=$(( 256 * 1024 * 1024 * 1024 ))

die() { echo "error: $*" >&2; exit 1; }

# --- decisions, kept free of side effects so the test can call them ---

# transport_verdict TRAN RM MMC_TYPE
# Succeeds if a device reporting these is a stick or a removable card. On
# failure, prints why.
#
# MMC_TYPE is /sys/block/mmcblkN/device/type, empty for non-MMC devices.
# lsblk reports TRAN=mmc for every mmcblk device, and that includes the
# soldered eMMC that mini-PCs and laptops boot Windows from -- so TRAN=mmc
# alone says nothing about removability. The card's own type does: "SD" is a
# card in a slot, "MMC" is eMMC (or, rarely, an old MMC card), "SDIO" is not
# storage at all. Only SD is accepted.
transport_verdict() {
  local tran="$1" rm="$2" mmc_type="$3"
  if [[ "$tran" == mmc || -n "$mmc_type" ]]; then
    if [[ "$mmc_type" == SD ]]; then return 0; fi
    echo "it is an MMC device of type '${mmc_type:-unknown}', not an SD card -- most likely the machine's internal eMMC."
    return 1
  fi
  if [[ "$tran" == usb || "$rm" == 1 ]]; then return 0; fi
  # An internal SATA or NVMe disk reports neither, and nor does a virtual
  # disk -- which matters, because from inside a container the host's disks
  # are visible but their mounts are not.
  echo "it is not removable and not on USB (TRAN='${tran}', RM=${rm})."
  return 1
}

# size_verdict BYTES ALLOW_LARGE
size_verdict() {
  local bytes="$1" allow="$2"
  if (( bytes <= MAX_BYTES_WITHOUT_OVERRIDE )) || [[ "$allow" == 1 ]]; then return 0; fi
  echo "it is $(( bytes / 1000000000 )) GB ($bytes bytes), larger than any stick this is meant for. If it really is the stick, pass --allow-large."
  return 1
}

# device_identity NAME
# What the name refers to right now. diskseq (kernel 5.15+) is unique per
# attached medium and never reused, so a stick replaced by another device
# under the same name -- after a reset, a replug, a loop reattach -- changes
# it. Older kernels: major:minor, size and serial, which catches most swaps.
device_identity() {
  local sys="/sys/class/block/$1"
  [[ -e "$sys" ]] || { echo "absent"; return 0; }
  if [[ -r "$sys/diskseq" ]]; then
    echo "diskseq=$(cat "$sys/diskseq")"
  else
    echo "dev=$(cat "$sys/dev") size=$(cat "$sys/size") serial=$(lsblk -dno SERIAL "/dev/$1" 2>/dev/null | tr -d '[:space:]')"
  fi
}

# add_partition_to_kernel DEV
# After sfdisk has written partition 3 to disk, make the kernel see it.
# BLKRRPART re-reads the whole table, and fails with EBUSY if any partition is
# in use -- which is the normal state on a desktop, where udev re-reads the
# table as soon as dd closes the device and udisks mounts the fresh image.
# partx --update --nr 3 then adds or corrects just that one partition through
# BLKPG, which works with partitions 1 or 2, or the whole disk, mounted.
# (sfdisk without --no-tell-kernel does not do this: measured, it leaves the
# kernel without partition 3 and still exits 0.)
add_partition_to_kernel() {
  local dev="$1"
  if blockdev --rereadpt "$dev" 2>/dev/null; then return 0; fi
  partx --update --nr 3 "$dev"
}

# find_partition NAME START_SECTOR -- prints /dev/<partition> or nothing.
# Found by where it starts, not by guessing its name: sdb3, mmcblk0p3 and
# loop0p3 are all the same partition. sysfs counts in 512-byte units.
find_partition() {
  local name="$1" start="$2" dir part=""
  for _ in $(seq 1 40); do
    for dir in /sys/class/block/"$name"/"$name"*; do
      [[ -f "$dir/start" && "$(cat "$dir/start")" == "$start" ]] && part="/dev/$(basename "$dir")"
    done
    [[ -n "$part" && -b "$part" ]] && break
    if command -v udevadm >/dev/null; then udevadm settle 2>/dev/null || true; fi
    sleep 0.25
  done
  if [[ -n "$part" && -b "$part" ]]; then printf '%s' "$part"; fi
}

# tools/test-write-usb.sh sources this file to test the decisions above with
# inputs a container cannot produce, such as an eMMC. Everything below runs
# only when the script is executed.
if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then return 0; fi

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="$ROOT/iso/sonora.iso"

# Test-only. When set to the path of a regular file, the target must be a loop
# device backed by exactly that file, and nothing else is accepted -- not a
# loop over some other file, and not a real disk. It replaces the removable/USB
# check, which a loop device can never pass, and relaxes nothing else.
TEST_IMAGE="${SONORA_WRITE_USB_TEST_IMAGE:-}"

ALLOW_LARGE=0
ARG=""
for a in "$@"; do
  case "$a" in
    --allow-large) ALLOW_LARGE=1 ;;
    -*) die "unknown option $a" ;;
    *)  [[ -z "$ARG" ]] || die "pass exactly one device."; ARG="$a" ;;
  esac
done

[[ -n "$ARG" ]] || { echo "usage: $0 [--allow-large] /dev/sdX" >&2; exit 1; }
[[ "$(id -u)" == 0 ]] || die "run as root."
[[ -f "$ISO" ]] || die "$ISO not found; run tools/build-iso.sh."
[[ -b "$ARG" ]] || die "$ARG is not a block device."

for tool in lsblk wipefs sfdisk partx blockdev mkfs.ext4 blkid cmp dd; do
  command -v "$tool" >/dev/null || die "$tool is required and not installed."
done

# /dev/disk/by-id/usb-... is the safest way to name a stick, and it is a
# symlink. Every check below must look at the real node.
DEV="$(readlink -f "$ARG")"
NAME="$(basename "$DEV")"

# --- is this a device we may write at all ---
check_kind() {
  local type rm tran mmc_type="" why backing bytes

  # Ask the kernel rather than pattern-match the name. "Ends in a digit"
  # called /dev/mmcblk0 and /dev/disk/by-id/usb-...-0:0 partitions, and called
  # /dev/disk/by-label/persistence -- partition 3 of a SonoraOS stick -- a disk.
  type="$(lsblk -dno TYPE "$DEV")" || die "lsblk cannot describe $DEV."
  case "$type" in
    disk) ;;
    part) die "$DEV is a partition. Pass the whole disk, e.g. /dev/sdb." ;;
    loop) [[ -n "$TEST_IMAGE" ]] || die "$DEV is a loop device, not a USB stick." ;;
    *)    die "$DEV is a '$type' device, not a disk." ;;
  esac

  if [[ -n "$TEST_IMAGE" ]]; then
    [[ "$type" == loop ]] || die "SONORA_WRITE_USB_TEST_IMAGE is set, so only a loop device over that file is accepted, and $DEV is a $type."
    [[ -f "$TEST_IMAGE" ]] || die "SONORA_WRITE_USB_TEST_IMAGE=$TEST_IMAGE is not a regular file."
    backing="$(losetup -nO BACK-FILE "$DEV" 2>/dev/null | sed 's/[[:space:]]*$//')"
    [[ -n "$backing" && "$(readlink -f "$backing")" == "$(readlink -f "$TEST_IMAGE")" ]] \
      || die "$DEV is backed by '${backing:-nothing}', not by $TEST_IMAGE."
  else
    rm="$(lsblk -dno RM "$DEV" | tr -d '[:space:]')"
    tran="$(lsblk -dno TRAN "$DEV" | tr -d '[:space:]')"
    if [[ -r "/sys/class/block/$NAME/device/type" ]] && [[ "$NAME" == mmcblk* ]]; then
      mmc_type="$(tr -d '[:space:]' < "/sys/class/block/$NAME/device/type")"
    fi
    why="$(transport_verdict "$tran" "$rm" "$mmc_type")" \
      || die "$DEV: $why This script only writes USB sticks and SD cards."
  fi

  bytes="$(blockdev --getsize64 "$DEV")"
  why="$(size_verdict "$bytes" "$ALLOW_LARGE")" || die "$DEV: $why"
}

# --- is anything using it ---
check_not_in_use() {
  local mounts holder out

  # Refuse anything mounted, which is how people overwrite their root.
  # Capture lsblk's output rather than piping it into grep: if lsblk fails,
  # the pipe would read as "nothing mounted" and the check would pass on the
  # very error it exists to catch.
  mounts="$(lsblk -nlo MOUNTPOINT "$DEV")" || die "cannot list the mounts on $DEV."
  if [[ -n "$(printf '%s' "$mounts" | tr -d '[:space:]')" ]]; then
    echo "error: $DEV or one of its partitions is mounted or in use as swap. Unmount it first." >&2
    lsblk -o NAME,SIZE,MODEL,MOUNTPOINT "$DEV" >&2
    exit 1
  fi

  # An unmounted LVM volume or an open LUKS mapping on the disk is still a
  # filesystem someone is using; it shows up as a holder, not a mount.
  shopt -s nullglob
  for holder in /sys/class/block/"$NAME"/holders/* /sys/class/block/"$NAME"/"$NAME"*/holders/*; do
    die "$DEV is held by $(basename "$holder") (LVM, RAID or dm-crypt). Deactivate it first."
  done
  shopt -u nullglob

  # The kernel's own answer. lsblk only sees mounts in this mount namespace;
  # a mount in a container, a sandbox or a desktop session's private namespace
  # is invisible to it. But every mount, swap, md and dm claims the device
  # exclusively in the kernel, and an O_EXCL open of the whole disk fails with
  # EBUSY while any partition is claimed. wipefs without --force opens with
  # O_RDWR|O_EXCL (checked with strace, util-linux 2.41), and -n makes it
  # write nothing. It cannot see a process that merely has the device open
  # without O_EXCL; nothing short of scanning every /proc/*/fd can.
  if ! out="$(wipefs --no-act --all "$DEV" 2>&1 >/dev/null)"; then
    die "$DEV is in use by something this namespace cannot see (the kernel refused an exclusive open: ${out##*: }). Unmount it everywhere first."
  fi
}

check_kind
check_not_in_use

# --- will the image fit, and will its partition table mean what it says ---
SECTOR="$(blockdev --getss "$DEV")"
# The hybrid MBR inside the ISO counts in 512-byte sectors. On a 4K-sector
# device every entry would point eight times too far.
[[ "$SECTOR" == 512 ]] || die "$DEV has ${SECTOR}-byte sectors; the image's partition table assumes 512."
ISO_BYTES="$(stat -Lc%s "$ISO")"
DEV_BYTES="$(blockdev --getsize64 "$DEV")"
START_MIB=$(( (ISO_BYTES + 1048575) / 1048576 ))
NEED_MIB=$(( START_MIB + MIN_PERSISTENCE_MIB ))
(( DEV_BYTES / 1048576 >= NEED_MIB )) \
  || die "$DEV is $(( DEV_BYTES / 1048576 )) MiB; the image and the smallest useful persistence partition need ${NEED_MIB} MiB."

IDENTITY="$(device_identity "$NAME")"
[[ -n "$TEST_IMAGE" ]] && echo "TEST MODE: writing to $DEV, a loop device over $TEST_IMAGE."
echo "Target device ($(( DEV_BYTES / 1000000000 )) GB):"
lsblk -o NAME,SIZE,TYPE,RM,TRAN,VENDOR,MODEL,SERIAL "$DEV"
echo
read -r -p "Erase this device completely? Type ERASE to continue: " reply
[[ "$reply" == "ERASE" ]] || { echo "aborted."; exit 1; }

# --- everything again, now that time has passed ---
# The answer above is about the device that was shown. A stick that reset or
# was swapped during the prompt can hand its name to another disk, unmounted
# and USB -- a backup drive passes every check. Only identity catches that.
recheck() {
  [[ "$(readlink -f "$ARG")" == "$DEV" ]] \
    || die "$ARG no longer points at $DEV. Nothing was written; run again."
  [[ "$(device_identity "$NAME")" == "$IDENTITY" ]] \
    || die "$DEV changed while waiting ($IDENTITY, now $(device_identity "$NAME")): it is not the device you confirmed. Nothing was written; run again."
}
recheck
check_kind
check_not_in_use

# --- write ---
# Without --force wipefs opens O_EXCL, so the first write is itself refused
# if anything has claimed the device since the check above.
#
# A stick that once carried GPT keeps a backup header in its last sectors,
# which writing over the start never touches. Some UEFI firmware then trusts
# that stale GPT over the image's MBR and finds no bootable partition.
echo "Clearing old partition-table signatures..."
wipefs --all "$DEV" >/dev/null || die "could not claim $DEV exclusively; nothing was written."

# dd and the read-back go through descriptors opened once, here, and checked
# against the identity: a path is looked up again on every open, a
# descriptor keeps meaning the device it was opened on.
exec 3<>"$DEV"
exec 4<"$DEV"
recheck

echo "Writing the image..."
dd if="$ISO" bs=4M status=progress conv=fsync >&3

# Read it back. Counterfeit and worn-out sticks accept writes they do not
# keep, and the first symptom would otherwise be a boot failure on stage.
echo "Verifying the write..."
blockdev --flushbufs "$DEV"
cmp -n "$ISO_BYTES" "$ISO" - <&4 || die "$DEV does not read back what was written. The stick is faulty."
exec 3>&- 4<&-

# --- persistence partition ---
# The image's MBR already holds two entries: 1 is the whole ISO and 2 is the
# EFI system partition *inside* it. So the new partition is number 3, and
# formatting "${DEV}2" -- as this script once would have -- destroys UEFI boot.
# parted cannot be used either: it sees xorriso's Apple partition map, reports
# the label as unknown and refuses. sfdisk edits the MBR in place.
recheck
START_SECTOR=$(( START_MIB * 1048576 / 512 ))
echo "Creating the persistence partition at ${START_MIB} MiB..."
# sfdisk's output is shown only on failure. On success it warns that the
# iso9660 signature "may remain" and recommends wipefs -- advice that, taken
# at this point, erases the image just written.
if ! sfdisk_out="$(echo "start=${START_SECTOR}, type=83" \
      | sfdisk --append --no-reread --no-tell-kernel --wipe never --wipe-partitions never "$DEV" 2>&1)"; then
  printf '%s\n' "$sfdisk_out" >&2
  die "sfdisk could not add the persistence partition to $DEV."
fi

NO_PERSISTENCE="The stick boots, but without persistence."
add_partition_to_kernel "$DEV" \
  || die "the kernel would not take the new partition on $DEV. $NO_PERSISTENCE Unplug it, plug it back in, and run this again."
PART="$(find_partition "$NAME" "$START_SECTOR")"
[[ -n "$PART" ]] || die "the kernel does not show the new partition on $DEV. $NO_PERSISTENCE Unplug it, plug it back in, and run this again."

# A desktop that mounted the whole disk rather than partition 1 holds every
# partition on it; mkfs then fails with "apparently in use by the system".
mkfs.ext4 -q -F -L persistence "$PART" \
  || die "could not format $PART. $NO_PERSISTENCE If the desktop mounted the stick, unmount it and run this again."

MNT="$(mktemp -d)"
trap 'umount "$MNT" 2>/dev/null || true; rmdir "$MNT" 2>/dev/null || true' EXIT
mount -t ext4 "$PART" "$MNT"
# Only /home is unioned, so a damaged overlay cannot stop the system booting.
echo "/home union" > "$MNT/persistence.conf"
umount "$MNT"
sync

[[ "$(blkid -o value -s LABEL "$PART")" == persistence ]] \
  || die "$PART does not carry the persistence label after formatting."

echo
echo "Done: image on $DEV, persistence on $PART."
echo "Boot with the 'persistence' kernel parameter (the default entry already"
echo "sets it). Secure Boot must be disabled."
