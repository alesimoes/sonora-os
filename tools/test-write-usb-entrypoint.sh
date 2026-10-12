#!/bin/bash
# Runs inside the privileged usbtest container for tools/test-write-usb.sh.
#
#   write    assert the refusals, write a disk image through a loop device,
#            verify it byte for byte, and export it for QEMU
#   inspect  after a QEMU boot, read the persistence partition offline and
#            assert on what the guest left in /home
#
# The only disk this script ever lets write-usb.sh write is /work/stick.img.
# Every refusal is attempted with the answer "no" at the prompt, so a safety
# check that wrongly passed would still stop at the prompt -- and the test
# then reports it as a failure, because reaching the prompt is the failure.
set -euo pipefail

PHASE="${1:-}"
STICK=/work/stick.img
SMALL=/work/small.img
DECOY=/work/decoy.img
ISO=/src/iso/sonora.iso
WRITER=/src/tools/write-usb.sh
SIZE="${STICK_SIZE:-8G}"

FAILED=0
ok()   { echo "WRITE-USB ok    $1"; }
bad()  { echo "WRITE-USB FAIL  $1"; FAILED=1; }
info() { echo "WRITE-USB info  $1"; }

LOOPS=()
LOOP=""
SMALL_LOOP=""
DUMP=""
MOUNTS=()
cleanup() {
  for m in "${MOUNTS[@]}"; do umount "$m" 2>/dev/null || true; done
  for l in "${LOOPS[@]}"; do losetup -d "$l" 2>/dev/null || true; done
}
trap cleanup EXIT

# expect OK_MESSAGE FAIL_MESSAGE COMMAND...
expect() {
  local good="$1" fail="$2"
  shift 2
  if "$@"; then ok "$good"; else bad "$fail"; fi
}
dump_has() { printf '%s\n' "$DUMP" | grep -Eq "$1"; }

# attach VAR [losetup args] FILE -- sets VAR to the loop device.
# Not LOOP="$(attach ...)": a command substitution is a subshell, so the
# LOOPS+= inside it never reached the cleanup trap. Every loop device leaked,
# each pinning an 8 GB image that had already been deleted.
attach() {
  local var="$1" l
  shift
  l="$(losetup -fP --show "$@")"
  LOOPS+=("$l")
  printf -v "$var" '%s' "$l"
}

# Docker gives the container a static /dev, so the partition nodes the kernel
# creates after write-usb.sh re-reads the table would never appear, and the
# script would correctly report that it cannot find its partition. devtmpfs is
# what a real system has: the kernel populates it itself, no udev needed.
if [[ "$(stat -f -c %T /dev)" != devtmpfs ]]; then
  mount -t devtmpfs devtmpfs /dev
fi

# Expect write-usb.sh to refuse, before the prompt, with a message containing
# $2. "aborted." in the output means it reached the prompt: a failed check.
refuse() {
  local label="$1" pattern="$2" dev="$3" image="${4:-}"
  local out rc=0
  out="$(echo no | SONORA_WRITE_USB_TEST_IMAGE="$image" bash "$WRITER" "$dev" 2>&1)" || rc=$?
  if [[ "$rc" -ne 0 && "$out" == *"$pattern"* && "$out" != *aborted* ]]; then
    ok "refuses $label"
    info "  $(printf '%s\n' "$out" | grep -m1 error)"
  else
    bad "refuses $label (exit $rc)"
    printf '%s\n' "$out" | sed 's/^/WRITE-USB info    /'
  fi
}

# Every property of a correctly written stick, checked against the device.
verify_stick() {
  local loop="$1" when="$2"
  local iso_bytes start_mib start_sector p3 diffs outside

  iso_bytes="$(stat -Lc%s "$ISO")"
  start_mib=$(( (iso_bytes + 1048575) / 1048576 ))
  start_sector=$(( start_mib * 2048 ))

  DUMP="$(sfdisk -d "$loop")"
  printf '%s\n' "$DUMP" | sed 's/^/WRITE-USB info    /'

  expect "$when: partition table is MBR" "$when: partition table is not MBR" \
    test "$(blkid -p -o value -s PTTYPE "$loop")" == dos
  expect "$when: partition 1 is the ISO, unchanged" "$when: partition 1 changed" \
    dump_has "^${loop}p1 : start= *64, .*type=0, bootable"
  expect "$when: partition 2 is the EFI system partition, unchanged" "$when: partition 2 changed" \
    dump_has "^${loop}p2 : start= *540, size= *6656, type=ef"
  expect "$when: partition 3 is Linux, at ${start_mib} MiB" "$when: no Linux partition 3 at sector $start_sector" \
    dump_has "^${loop}p3 : start= *${start_sector}, .*type=83"
  expect "$when: exactly three partitions" "$when: not exactly three partitions" \
    test "$(printf '%s\n' "$DUMP" | grep -c "^${loop}p")" == 3
  expect "$when: EFI system partition is still FAT" "$when: EFI system partition is no longer FAT" \
    test "$(blkid -o value -s TYPE "${loop}p2")" == vfat

  # Byte-identical except the 16-byte MBR slot that now describes partition 3
  # (bytes 446+32 .. 446+47, 1-based 479..494 in cmp -l). The ISO has zeros
  # there, which is what makes adding the entry safe.
  diffs="$(cmp -l -n "$iso_bytes" "$ISO" "$loop" || true)"
  outside="$(printf '%s\n' "$diffs" | awk 'NF==3 && ($1 < 479 || $1 > 494)' | wc -l)"
  info "$when: $(printf '%s\n' "$diffs" | grep -c . || true) bytes differ from the ISO in its first ${iso_bytes}"
  expect "$when: ISO region identical to iso/sonora.iso apart from MBR slot 3" \
    "$when: $outside bytes differ outside MBR slot 3" \
    test "$outside" == 0
  expect "$when: MBR slot 3 was empty in the ISO" "$when: MBR slot 3 was not empty in the ISO" \
    test -z "$(printf '%s\n' "$diffs" | awk 'NF==3 && $2 != 0')"

  p3="${loop}p3"
  expect "$when: partition 3 is ext4" "$when: partition 3 is not ext4" \
    test "$(blkid -o value -s TYPE "$p3")" == ext4
  expect "$when: partition 3 is labelled persistence" "$when: partition 3 label is wrong" \
    test "$(blkid -o value -s LABEL "$p3")" == persistence

  mkdir -p /mnt/p3
  mount -o ro "$p3" /mnt/p3
  MOUNTS+=(/mnt/p3)
  expect "$when: persistence.conf is '/home union'" "$when: persistence.conf is wrong" \
    test "$(cat /mnt/p3/persistence.conf)" == "/home union"
  umount /mnt/p3

  # A stale GPT from the stick's previous life would sit in the last sector.
  if dd if="$loop" bs=512 skip=$(( $(blockdev --getsz "$loop") - 1 )) count=1 status=none \
       | grep -q "EFI PART"; then
    bad "$when: a backup GPT header survives in the last sector"
  else
    ok "$when: no backup GPT header left in the last sector"
  fi
}

case "$PHASE" in
write)
  [[ -f "$ISO" ]] || { echo "FAIL: $ISO missing" >&2; exit 1; }
  rm -f "$STICK" "$SMALL" "$DECOY"
  truncate -s "$SIZE" "$STICK"
  truncate -s 600M "$SMALL"
  truncate -s 64M "$DECOY"

  # A stick with a history: GPT, one ext4 partition. The write must leave no
  # trace of it, including the backup GPT header at the far end.
  printf 'label: gpt\n,512MiB,L\n' | sfdisk -q "$STICK"
  attach LOOP "$STICK"
  attach SMALL_LOOP "$SMALL"
  info "stick image $STICK ($SIZE, sparse) on $LOOP"
  mkfs.ext4 -q -F "${LOOP}p1"

  echo "--- refusals ---"
  # The container's root disk is the one under /etc/hosts; / itself is an
  # overlay with no device. Also try every other disk the VM exposes: their
  # mounts live in namespaces this container cannot see, which is exactly the
  # case that once reached the prompt.
  # findmnt prints a bind mount's source as /dev/sde[/path/inside].
  ROOT_SRC="$(findmnt -no SOURCE /etc/hosts | sed 's/\[.*//')"
  ROOT_DISK="/dev/$(lsblk -no PKNAME "$ROOT_SRC" 2>/dev/null | head -1)"
  [[ "$ROOT_DISK" == /dev/ ]] && ROOT_DISK="$ROOT_SRC"
  info "container root disk: $ROOT_DISK"
  refuse "the container's root disk ($ROOT_DISK)" "error:" "$ROOT_DISK"
  refuse "the root disk even in test mode" "only a loop device" "$ROOT_DISK" "$STICK"
  disks=0
  for d in $(lsblk -dnpo NAME,TYPE | awk '$2 == "disk" {print $1}'); do
    [[ "$d" == "$ROOT_DISK" ]] && continue
    refuse "host disk $d ($(lsblk -dno SIZE "$d" | tr -d ' '))" "error:" "$d"
    disks=$((disks + 1))
  done
  info "$disks other host disks tried"
  refuse "a loop device without the test variable" "is a loop device, not a USB stick" "$LOOP"
  refuse "a loop device over a different file" "is backed by" "$LOOP" "$DECOY"
  refuse "a partition" "is a partition" "${LOOP}p1" "$STICK"
  mkdir -p /mnt/seed
  mount "${LOOP}p1" /mnt/seed
  MOUNTS+=(/mnt/seed)
  refuse "a disk with a mounted partition" "is mounted" "$LOOP" "$STICK"
  umount /mnt/seed
  refuse "a device too small for image plus persistence" "need" "$SMALL_LOOP" "$SMALL"

  before="$(sha256sum < "$STICK")"
  out="$(echo no | SONORA_WRITE_USB_TEST_IMAGE="$STICK" bash "$WRITER" "$LOOP" 2>&1)" || true
  if [[ "$out" == *aborted* && "$(sha256sum < "$STICK")" == "$before" ]]; then
    ok "an answer other than ERASE aborts and writes nothing"
  else
    bad "an answer other than ERASE did not abort cleanly"
    printf '%s\n' "$out" | sed 's/^/WRITE-USB info    /'
  fi

  echo "--- write over a used stick ---"
  if echo ERASE | SONORA_WRITE_USB_TEST_IMAGE="$STICK" bash "$WRITER" "$LOOP" > /work/write1.log 2>&1; then
    ok "write-usb.sh exits 0"
  else
    bad "write-usb.sh failed"
  fi
  tr '\r' '\n' < /work/write1.log | grep -v -e "bytes .* copied" -e '^$' | sed 's/^/WRITE-USB info    /'
  verify_stick "$LOOP" "first write"

  # Rewriting a stick that already carries SonoraOS is the common case for
  # an update, and the one where partition 3 already exists.
  echo "--- rewrite over a SonoraOS stick ---"
  if echo ERASE | SONORA_WRITE_USB_TEST_IMAGE="$STICK" bash "$WRITER" "$LOOP" > /work/write2.log 2>&1; then
    ok "second write-usb.sh run exits 0"
  else
    bad "second write-usb.sh run failed"
    tr '\r' '\n' < /work/write2.log | sed 's/^/WRITE-USB info    /'
  fi
  verify_stick "$LOOP" "rewrite"

  echo "--- exporting for QEMU ---"
  # Kernel and initrd from the stick just written, not from the ISO file, so
  # the boot exercises what is actually on the device.
  mkdir -p /mnt/iso
  mount -o ro -t iso9660 "$LOOP" /mnt/iso
  MOUNTS+=(/mnt/iso)
  KERNEL="$(find /mnt/iso/live -maxdepth 1 -name 'vmlinuz-*-rt-*' | head -1)"
  INITRD="$(find /mnt/iso/live -maxdepth 1 -name 'initrd.img-*-rt-*' | head -1)"
  info "kernel $KERNEL"
  install -m 0644 "$KERNEL" /out/vmlinuz
  install -m 0644 "$INITRD" /out/initrd.img
  umount /mnt/iso
  sync
  for l in "${LOOPS[@]}"; do losetup -d "$l"; done
  LOOPS=()
  # qcow2 so the host gets ~1 GB rather than 8 GB of mostly zeros through a
  # Windows bind mount. The content is the same disk, sector for sector.
  rm -f /out/stick.qcow2
  qemu-img convert -O qcow2 "$STICK" /out/stick.qcow2
  info "exported $(du -h /out/stick.qcow2 | cut -f1) qcow2"
  # One overlay per boot sequence, so the BIOS boot cannot find a session
  # folder the UEFI boot left behind, and the written image stays pristine.
  # The backing path is relative, so QEMU on the host resolves it too.
  for overlay in persist bios uefi; do
    rm -f "/out/$overlay.qcow2"
    (cd /out && qemu-img create -q -f qcow2 -b stick.qcow2 -F qcow2 "$overlay.qcow2")
  done
  ;;

inspect)
  # Parameters, from the environment:
  #   DISK            qcow2 under /out to read (default stick.qcow2)
  #   LABEL           what this inspection is about, for the output
  #   CLEAN_SHUTDOWN  1 if the guest powered itself off; then fsck must pass
  #   EXPECT_MARKER   the nonce the guest must have written, if any
  #   EXPECT_SESSION  1 if the engine must have created its session folder
  DISK="${DISK:-stick.qcow2}"
  LABEL="${LABEL:-$DISK}"
  # A raw copy in the volume: the qcow2 on the host stays exactly as the guest
  # left it, and this copy can replay a journal without touching it.
  rm -f /work/booted.img
  qemu-img convert -O raw "/out/$DISK" /work/booted.img
  attach LOOP /work/booted.img

  if [[ "${CLEAN_SHUTDOWN:-0}" == 1 ]]; then
    # The guest powered off with the partition mounted read-write under it.
    # A filesystem that needs repair after a clean shutdown is a defect
    # worth knowing about even when the marker survived.
    if e2fsck -fn "${LOOP}p3" > /work/fsck.log 2>&1; then
      ok "$LABEL: persistence filesystem is clean after the guest powered off"
    else
      bad "$LABEL: persistence filesystem is not clean after the guest powered off"
      sed 's/^/WRITE-USB info    /' /work/fsck.log
    fi
  fi

  # Read-write so that a guest killed mid-run gets its journal replayed, as
  # the next boot of the real stick would do.
  mkdir -p /mnt/p3
  mount "${LOOP}p3" /mnt/p3
  MOUNTS+=(/mnt/p3)
  # live-boot's overlay for "/home union" keeps its upper layer in home/rw
  # and its overlayfs workdir in home/work, not in home/ itself.
  UPPER=/mnt/p3/home/rw
  info "$LABEL: on the partition: $(cd /mnt/p3 && find . -maxdepth 5 -not -path './lost+found*' | sort | tr '\n' ' ')"

  if [[ -n "${EXPECT_MARKER:-}" ]]; then
    got="$(cat "$UPPER/sonora/persistence-marker" 2>/dev/null || echo '(absent)')"
    expect "$LABEL: marker written by the guest is on the partition ($got)" \
      "$LABEL: marker on the partition is '$got', expected '$EXPECT_MARKER'" \
      test "$got" == "$EXPECT_MARKER"
  fi
  if [[ "${EXPECT_SESSION:-0}" == 1 ]]; then
    # The engine creates Sessions/Untitled when it starts. Finding it in the
    # upper layer proves the boot reached the engine and that the session
    # folder ADR-0005 promises is on the stick, not in RAM.
    expect "$LABEL: the engine's session folder is on the persistence partition" \
      "$LABEL: no Sessions/Untitled on the persistence partition" \
      test -d "$UPPER/sonora/Sessions/Untitled"
  fi
  ;;

*)
  echo "usage: $0 write|inspect" >&2
  exit 2
  ;;
esac

if [[ "$FAILED" == 0 ]]; then
  echo "WRITE-USB RESULT pass"
else
  echo "WRITE-USB RESULT fail"
  exit 1
fi
