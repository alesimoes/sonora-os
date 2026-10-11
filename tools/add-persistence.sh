#!/usr/bin/env bash
# Add the ext4 persistence partition to a stick that already carries the image.
#
# This exists because Windows cannot create an ext4 filesystem, so
# tools/write-usb-windows.ps1 writes a bootable stick without persistence and
# leaves this to be run from inside the booted system -- which already has
# parted and mkfs.ext4.
#
# Run it on the booted SonoraOS machine, against the stick it booted from:
#   sudo tools/add-persistence.sh /dev/sda
#
# The system runs from a squashfs in RAM and the ISO partition is mounted
# read-only, so repartitioning the free space after it is safe while running.
set -euo pipefail

DEV="${1:-}"

[[ -n "$DEV" ]]       || { echo "usage: $0 /dev/sdX" >&2; exit 1; }
[[ -b "$DEV" ]]       || { echo "error: $DEV is not a block device." >&2; exit 1; }
[[ "$(id -u)" == 0 ]] || { echo "error: run as root." >&2; exit 1; }

if [[ "$DEV" =~ [0-9]$ ]]; then
  echo "error: $DEV looks like a partition. Pass the whole disk." >&2
  exit 1
fi

# Refuse a disk that already has persistence, rather than making a second one
# that live-boot would have to choose between.
if lsblk -no LABEL "$DEV" | grep -qx "persistence"; then
  echo "error: $DEV already has a partition labelled persistence." >&2
  lsblk -o NAME,SIZE,LABEL,MOUNTPOINTS "$DEV" >&2
  exit 1
fi

echo "Target:"
lsblk -o NAME,SIZE,LABEL,MOUNTPOINTS "$DEV"
echo

# The ISO occupies the start of the disk. Find where it ends so the new
# partition goes after it rather than over it.
LAST_END="$(parted -sm "$DEV" unit MiB print 2>/dev/null \
  | awk -F: '/^[0-9]+:/ { gsub("MiB","",$3); last=$3 } END { print int(last) }')"
[[ -n "$LAST_END" && "$LAST_END" -gt 0 ]] || {
  echo "error: could not work out where the existing data ends." >&2
  parted -s "$DEV" print >&2
  exit 1
}

TOTAL="$(parted -sm "$DEV" unit MiB print 2>/dev/null \
  | awk -F: 'NR==2 { gsub("MiB","",$2); print int($2) }')"
FREE=$(( TOTAL - LAST_END ))
echo "Existing data ends at ${LAST_END} MiB; ${FREE} MiB free of ${TOTAL} MiB."

# A 32-track session at 24-bit 48 kHz is about 4.6 MB/s, so 1 GiB is roughly
# four minutes. Less than that is not worth the confusion of having it.
if [[ "$FREE" -lt 1024 ]]; then
  echo "error: only ${FREE} MiB free. That holds under four minutes of 32-track" >&2
  echo "       audio; use a larger stick." >&2
  exit 1
fi

read -r -p "Create a ${FREE} MiB persistence partition on $DEV? [y/N] " reply
[[ "$reply" == "y" || "$reply" == "Y" ]] || { echo "aborted."; exit 1; }

parted -s "$DEV" mkpart primary ext4 "${LAST_END}MiB" 100%
partprobe "$DEV"
sleep 2

# Find the partition we just made rather than assuming a suffix: /dev/sdb2 and
# /dev/nvme0n1p2 and /dev/mmcblk0p2 all differ.
PART="$(lsblk -lno NAME,TYPE "$DEV" | awk '$2=="part" { print $1 }' | tail -1)"
PART="/dev/$PART"
[[ -b "$PART" ]] || { echo "error: cannot find the new partition." >&2; exit 1; }
echo "New partition: $PART"

mkfs.ext4 -F -L persistence "$PART"

MNT="$(mktemp -d)"
mount "$PART" "$MNT"
# Only /home is unioned, so a damaged overlay cannot stop the machine booting:
# the worst case is that settings revert, not that the studio is down.
echo "/home union" > "$MNT/persistence.conf"
mkdir -p "$MNT/home/sonora/Sessions"
chown -R 1000:1000 "$MNT/home"
umount "$MNT"
rmdir "$MNT"
sync

echo
echo "Done. Reboot and sessions in /home/sonora/Sessions will survive."
echo "The default boot entry already passes the 'persistence' parameter."
