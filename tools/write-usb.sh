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

# Where block devices are described. A variable only so the test can point
# device_identity at a tree without diskseq; nothing reads it from outside.
SYS_BLOCK=/sys/class/block

# device_identity NAME
# What the name refers to right now. diskseq (kernel 5.15+) is unique per
# attached medium and never reused, so a stick replaced by another device
# under the same name -- after a reset, a replug, a loop reattach -- changes
# it, and cannot change back. Older kernels get major:minor, size and serial,
# which a reattached loop or a same-size card swapped in a reader would
# reproduce exactly; identity_verdict decides what to do about that.
device_identity() {
  local sys="$SYS_BLOCK/$1"
  [[ -e "$sys" ]] || { echo "absent"; return 0; }
  if [[ -r "$sys/diskseq" ]]; then
    echo "diskseq=$(cat "$sys/diskseq")"
  else
    echo "dev=$(cat "$sys/dev") size=$(cat "$sys/size") serial=$(lsblk -dno SERIAL "/dev/$1" 2>/dev/null | tr -d '[:space:]')"
  fi
}

# identity_verdict IDENTITY ALLOW_WEAK
# Only diskseq proves that the device written is the device confirmed. Without
# it the user must say they accept that, rather than be protected by a check
# that cannot see the swap it is there for.
identity_verdict() {
  local identity="$1" allow="$2"
  if [[ "$identity" == diskseq=* || "$allow" == 1 ]]; then return 0; fi
  echo "this kernel has no diskseq (it arrived in Linux 5.15), so this script cannot prove that the device it writes is the one you confirm: a same-size card swapped in a reader, or a reattached loop device, looks identical. If nothing will touch the device until this finishes, pass --weak-identity."
  return 1
}

# exclusive_probe DEV
# Succeeds if nothing holds DEV exclusively: no mount, swap, md or dm on it or
# any of its partitions, in any mount namespace. lsblk only sees this
# namespace; the kernel refuses an O_EXCL open while anything claims the disk.
#
# Opened O_RDONLY|O_EXCL by perl (perl-base, Essential in Debian) and closed
# again, with no ioctl. Not wipefs --no-act: strace shows it opening O_RDWR
# and then issuing BLKRRPART anyway, which re-adds the old partitions -- and
# on a desktop that is what triggers an automount, during the prompt. Even
# without the ioctl, closing an O_RDWR descriptor makes udev re-probe the
# disk. A read-only open does neither.
#
# Cannot see a process that has the device open without O_EXCL. Only a scan
# of every /proc/*/fd could, and that is not attempted.
exclusive_probe() {
  perl -MFcntl -e 'sysopen(my $f, $ARGV[0], O_RDONLY | O_EXCL) or die "$!\n"; close $f' "$1"
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
WEAK_IDENTITY=0
ARG=""
for a in "$@"; do
  case "$a" in
    --allow-large)   ALLOW_LARGE=1 ;;
    --weak-identity) WEAK_IDENTITY=1 ;;
    -*) die "unknown option $a" ;;
    *)  [[ -z "$ARG" ]] || die "pass exactly one device."; ARG="$a" ;;
  esac
done

[[ -n "$ARG" ]] || { echo "usage: $0 [--allow-large] [--weak-identity] /dev/sdX" >&2; exit 1; }
[[ "$(id -u)" == 0 ]] || die "run as root."
[[ -f "$ISO" ]] || die "$ISO not found; run tools/build-iso.sh."
[[ -b "$ARG" ]] || die "$ARG is not a block device."

for tool in lsblk wipefs sfdisk partx blockdev mkfs.ext4 blkid cmp dd perl; do
  command -v "$tool" >/dev/null || die "$tool is required and not installed."
done
perl -MFcntl -e 1 2>/dev/null || die "perl's Fcntl module is required (Debian: perl-base)."

# /dev/disk/by-id/usb-... is the safest way to name a stick, and it is a
# symlink. Resolved once, here, and never again: from now on the device is
# known by its identity, not by a path. A content-based link such as
# /dev/disk/by-label/SONORA stops resolving the moment wipefs erases the
# label it was made from.
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

  # The kernel's own answer, for what lsblk cannot see. See exclusive_probe.
  if ! out="$(exclusive_probe "$DEV" 2>&1)"; then
    die "$DEV is in use by something this namespace cannot see (the kernel refused an exclusive open: $out). Unmount it everywhere first."
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
why="$(identity_verdict "$IDENTITY" "$WEAK_IDENTITY")" || die "$DEV: $why"
if [[ "$IDENTITY" != diskseq=* ]]; then
  echo "warning: --weak-identity: a device swapped for one of the same size during this run will not be noticed." >&2
fi
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
# What a failure at each point has already done to the device, so that the
# message says so instead of claiming nothing happened.
AT_START="Nothing was written; run again."
AFTER_WIPE="Its old partition-table and filesystem signatures were erased, so its previous contents will no longer mount, but the image was not written. Run again."
AFTER_DD="The image was only partly written, or not verified; the stick will not boot reliably. Run again."
AFTER_IMAGE="The image is written and verified and the stick boots, but it has no persistence partition. Run again to add one."

# recheck WHAT_HAS_HAPPENED
# By identity only. The path is never resolved again (see DEV above).
recheck() {
  local now
  now="$(device_identity "$NAME")"
  [[ "$now" == "$IDENTITY" ]] \
    || die "$DEV changed ($IDENTITY, now $now): it is not the device you confirmed. As for the one you confirmed: $1"
}

# fd_is_device FD -- the descriptor is the block device NAME has now.
fd_is_device() {
  local maj min
  read -r maj min < <(stat -L -c '%t %T' "/proc/self/fd/$1")
  [[ "$((16#$maj)):$((16#$min))" == "$(cat "$SYS_BLOCK/$NAME/dev")" ]]
}

recheck "$AT_START"
check_kind
check_not_in_use

# --- write ---
# Why the order below proves that every write lands on the confirmed device:
# the descriptors are opened first, then shown to be the device the name has
# now, then the name is shown to still carry the confirmed diskseq. wipefs
# opens by name, so it is bracketed by two identity checks. diskseq is never
# reused, so equal values on both sides mean the name meant the same medium
# throughout, and wipefs opened that medium. dd and the read-back use the
# descriptors, which keep meaning the device they were opened on whatever
# happens to the name. With --weak-identity none of this is proof, which is
# why that needs saying out loud.
exec 3<>"$DEV"
exec 4<"$DEV"
if ! fd_is_device 3 || ! fd_is_device 4; then
  die "the descriptors opened on $DEV are not the device it names. $AT_START"
fi
recheck "$AT_START"

# Without --force wipefs opens O_EXCL, so the first write is itself refused
# if anything has claimed the device since the check above.
#
# A stick that once carried GPT keeps a backup header in its last sectors,
# which writing over the start never touches. Some UEFI firmware then trusts
# that stale GPT over the image's MBR and finds no bootable partition.
echo "Clearing old partition-table signatures..."
wipefs --all "$DEV" >/dev/null \
  || die "wipefs failed on $DEV. If it got as far as erasing, some old signatures are gone and its previous contents may not mount; the image was not written. Run again."
recheck "$AFTER_WIPE"

echo "Writing the image..."
dd if="$ISO" bs=4M status=progress conv=fsync >&3 \
  || die "writing to $DEV failed. $AFTER_DD"

# Read it back. Counterfeit and worn-out sticks accept writes they do not
# keep, and the first symptom would otherwise be a boot failure on stage.
echo "Verifying the write..."
blockdev --flushbufs "$DEV"
cmp -n "$ISO_BYTES" "$ISO" - <&4 \
  || die "$DEV does not read back what was written; the stick is faulty. $AFTER_DD"
exec 3>&- 4<&-

# --- persistence partition ---
# The image's MBR already holds two entries: 1 is the whole ISO and 2 is the
# EFI system partition *inside* it. So the new partition is number 3, and
# formatting "${DEV}2" -- as this script once would have -- destroys UEFI boot.
# parted cannot be used either: it sees xorriso's Apple partition map, reports
# the label as unknown and refuses. sfdisk edits the MBR in place.
recheck "$AFTER_IMAGE"
START_SECTOR=$(( START_MIB * 1048576 / 512 ))
echo "Creating the persistence partition at ${START_MIB} MiB..."
# sfdisk's output is shown only on failure. On success it warns that the
# iso9660 signature "may remain" and recommends wipefs -- advice that, taken
# at this point, erases the image just written.
if ! sfdisk_out="$(echo "start=${START_SECTOR}, type=83" \
      | sfdisk --append --no-reread --no-tell-kernel --wipe never --wipe-partitions never "$DEV" 2>&1)"; then
  printf '%s\n' "$sfdisk_out" >&2
  die "sfdisk could not add the persistence partition to $DEV. $AFTER_IMAGE"
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
