#!/usr/bin/env bash
# Write the image to a USB stick and add the persistence partition (ADR-0005).
#
# THIS ERASES THE TARGET DEVICE. Pass the whole disk, not a partition:
#   sudo tools/write-usb.sh /dev/sdb
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="$ROOT/iso/sonora.iso"
DEV="${1:-}"

[[ -n "$DEV" ]]      || { echo "usage: $0 /dev/sdX" >&2; exit 1; }
[[ -b "$DEV" ]]      || { echo "error: $DEV is not a block device." >&2; exit 1; }
[[ -f "$ISO" ]]      || { echo "error: $ISO not found; run tools/build-iso.sh." >&2; exit 1; }
[[ "$(id -u)" == 0 ]] || { echo "error: run as root." >&2; exit 1; }

# Refuse a partition rather than a disk: writing an ISO to /dev/sdb1 produces
# a stick that does not boot and an error that is hard to read.
if [[ "$DEV" =~ [0-9]$ ]]; then
  echo "error: $DEV looks like a partition. Pass the whole disk, e.g. /dev/sdb." >&2
  exit 1
fi

# Refuse anything currently mounted, which is how people overwrite their root.
if lsblk -no MOUNTPOINTS "$DEV" | grep -q .; then
  echo "error: $DEV has mounted partitions. Unmount them first." >&2
  lsblk -o NAME,SIZE,MODEL,MOUNTPOINTS "$DEV" >&2
  exit 1
fi

echo "Target device:"
lsblk -o NAME,SIZE,MODEL,TRAN "$DEV"
echo
read -r -p "Erase this device completely? Type ERASE to continue: " reply
[[ "$reply" == "ERASE" ]] || { echo "aborted."; exit 1; }

echo "Writing the image..."
dd if="$ISO" of="$DEV" bs=4M status=progress conv=fsync

ISO_END=$(( ($(stat -c%s "$ISO") / 1048576) + 1 ))
echo "Creating the persistence partition after ${ISO_END} MiB..."
# The ISO occupies the start of the disk; the new partition goes after it.
parted -s "$DEV" mkpart primary ext4 "${ISO_END}MiB" 100%
partprobe "$DEV"
sleep 2

PART="${DEV}2"
[[ -b "$PART" ]] || PART="${DEV}p2"
mkfs.ext4 -F -L persistence "$PART"

MNT=$(mktemp -d)
mount "$PART" "$MNT"
# Only /home is unioned, so a damaged overlay cannot stop the system booting.
echo "/home union" > "$MNT/persistence.conf"
umount "$MNT"
rmdir "$MNT"
sync

echo
echo "Done. Boot with the 'persistence' kernel parameter (the default entry"
echo "already sets it). Secure Boot must be disabled."
