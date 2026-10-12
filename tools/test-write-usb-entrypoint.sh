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
BIG=/work/big.img
PARTTEST=/work/part.img
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
BIG_LOOP=""
PT_LOOP=""
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
  shift 4 2>/dev/null || shift $#
  local out rc=0
  out="$(echo no | SONORA_WRITE_USB_TEST_IMAGE="$image" bash "$WRITER" "$@" "$dev" 2>&1)" || rc=$?
  if [[ "$rc" -ne 0 && "$out" == *"$pattern"* && "$out" != *aborted* ]]; then
    ok "refuses $label"
    info "  $(printf '%s\n' "$out" | grep -m1 error)"
  else
    bad "refuses $label (exit $rc)"
    printf '%s\n' "$out" | sed 's/^/WRITE-USB info    /'
  fi
}

# Decisions a container cannot produce real inputs for -- there is no eMMC
# here -- are tested by calling write-usb.sh's own functions, which it
# exposes when sourced. In a subshell, because they are free to exit.
# The writer is checked in its own right; CI runs shellcheck without -x.
# shellcheck source=/dev/null
in_writer() { ( source "$WRITER"; "$@" ); }
accepts() {
  local label="$1"
  shift
  if in_writer "$@" >/dev/null 2>&1; then ok "accepts $label"; else bad "wrongly refuses $label"; fi
}
# device_identity against a different sysfs tree.
# SYS_BLOCK is write-usb.sh's, read by device_identity.
# shellcheck disable=SC2034
sh_identity() { SYS_BLOCK="$1"; device_identity "$2"; }
# write-usb.sh's partition step, as its main body runs it.
add_and_find() { add_partition_to_kernel "$1" && find_partition "$2" "$3"; }
rejects() {
  local label="$1" why
  shift
  if why="$(in_writer "$@" 2>&1)"; then
    bad "wrongly accepts $label"
  else
    ok "refuses $label"
    info "  $why"
  fi
}

# The GPT and ext4 the stick was seeded with. Still there means nothing was
# written.
seed_intact() {
  [[ "$(blkid -p -o value -s PTTYPE "$LOOP")" == gpt \
     && "$(blkid -p -o value -s TYPE "${LOOP}p1")" == ext4 ]]
}

# Mounts partition 1 of the stick in a private mount namespace, where this
# shell's lsblk cannot see it: what a container, a sandbox or another
# session's namespace looks like from here.
NS_PID=""
mount_elsewhere() {
  mkdir -p /mnt/ns
  unshare -m --propagation private sh -c "mount ${LOOP}p1 /mnt/ns && exec sleep 600" &
  NS_PID=$!
  sleep 1
}
unmount_elsewhere() {
  [[ -n "$NS_PID" ]] && { kill "$NS_PID" 2>/dev/null || true; wait "$NS_PID" 2>/dev/null || true; }
  NS_PID=""
  sleep 0.5
}

# Reattach the same file to the same loop node: same name, same backing file,
# same size, but a new medium as far as the kernel is concerned -- what a
# stick reset or swap looks like.
swap_device() {
  losetup -d "$LOOP"
  sleep 0.5
  losetup -P "$LOOP" "$STICK"
  sleep 0.5
}

# during_prompt LABEL PATTERN ACTION
# Start write-usb.sh on the stick, wait until it is at the prompt, run ACTION,
# then answer ERASE. The writer must refuse with PATTERN and write nothing.
# The answer really is ERASE here, so a recheck that failed would erase the
# test image -- and the seed check would say so.
during_prompt() {
  local label="$1" pattern="$2" action="$3" out pid
  rm -f /work/answer /work/prompt.log
  mkfifo /work/answer
  ( SONORA_WRITE_USB_TEST_IMAGE="$STICK" bash "$WRITER" "$LOOP" < /work/answer > /work/prompt.log 2>&1
    echo "exit=$?" >> /work/prompt.log ) &
  pid=$!
  exec 7>/work/answer
  for _ in $(seq 1 200); do
    grep -q "Target device" /work/prompt.log 2>/dev/null && break
    sleep 0.1
  done
  "$action"
  echo ERASE >&7
  exec 7>&-
  wait "$pid" || true
  out="$(cat /work/prompt.log)"
  if [[ "$out" == *"$pattern"* && "$out" != *"exit=0"* ]] && seed_intact; then
    ok "refuses after the prompt: $label"
    info "  $(printf '%s\n' "$out" | grep -m1 error)"
  else
    bad "after the prompt: $label"
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
  rm -f "$STICK" "$SMALL" "$DECOY" "$BIG" "$PARTTEST"
  truncate -s "$SIZE" "$STICK"
  truncate -s 600M "$SMALL"
  truncate -s 64M "$DECOY"
  truncate -s 300G "$BIG"

  # A stick with a history: GPT, one ext4 partition. The write must leave no
  # trace of it, including the backup GPT header at the far end.
  printf 'label: gpt\n,512MiB,L\n' | sfdisk -q "$STICK"
  attach LOOP "$STICK"
  attach SMALL_LOOP "$SMALL"
  attach BIG_LOOP "$BIG"
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
  # Only disks that are not sticks. On a native Linux host this container sees
  # the real disks, and a genuine stick plugged in there is a device the
  # script is supposed to accept -- offering it would either report a false
  # failure or, with a broken check, put a real stick one answer away from
  # erasure. Such devices are listed and skipped, never offered.
  disks=0
  skipped=0
  for d in $(lsblk -dnpo NAME,TYPE | awk '$2 == "disk" {print $1}'); do
    [[ "$d" == "$ROOT_DISK" ]] && continue
    n="$(basename "$d")"
    d_tran="$(lsblk -dno TRAN "$d" | tr -d '[:space:]')"
    d_rm="$(lsblk -dno RM "$d" | tr -d '[:space:]')"
    d_mmc=""
    if [[ "$n" == mmcblk* && -r "/sys/class/block/$n/device/type" ]]; then
      d_mmc="$(tr -d '[:space:]' < "/sys/class/block/$n/device/type")"
    fi
    if in_writer transport_verdict "$d_tran" "$d_rm" "$d_mmc" >/dev/null 2>&1; then
      info "not offered: $d is removable or USB on this host (TRAN='$d_tran', RM=$d_rm)"
      skipped=$((skipped + 1))
      continue
    fi
    refuse "host disk $d ($(lsblk -dno SIZE "$d" | tr -d ' '))" "error:" "$d"
    disks=$((disks + 1))
  done
  info "$disks other host disks tried, $skipped removable ones skipped"

  echo "--- decisions, with inputs a container cannot produce ---"
  # Review finding: lsblk reports TRAN=mmc for every mmcblk device, soldered
  # eMMC included, so TRAN=mmc must not be enough.
  accepts "a USB stick (TRAN=usb, RM=1)" transport_verdict usb 1 ""
  accepts "a USB SSD that reports RM=0 (TRAN=usb)" transport_verdict usb 0 ""
  accepts "an SD card in a built-in slot (TRAN=mmc, type SD)" transport_verdict mmc 0 SD
  rejects "an internal eMMC (TRAN=mmc, type MMC)" transport_verdict mmc 0 MMC
  rejects "an eMMC even if flagged removable (TRAN=mmc, RM=1, type MMC)" transport_verdict mmc 1 MMC
  rejects "an SDIO device (type SDIO)" transport_verdict mmc 0 SDIO
  rejects "an MMC device whose type cannot be read" transport_verdict mmc 0 ""
  rejects "an internal SATA disk (TRAN=sata, RM=0)" transport_verdict sata 0 ""
  rejects "an NVMe disk (TRAN=nvme, RM=0)" transport_verdict nvme 0 ""
  rejects "a virtual disk (no TRAN, RM=0)" transport_verdict "" 0 ""
  accepts "a 256 GB stick (256,060,514,304 bytes)" size_verdict 256060514304 0
  rejects "a 500 GB drive without --allow-large" size_verdict 500107862016 0
  accepts "a 500 GB drive with --allow-large" size_verdict 500107862016 1
  # Second review: without diskseq the identity cannot see a swap.
  accepts "a diskseq identity" identity_verdict "diskseq=42" 0
  rejects "an identity without diskseq (kernel < 5.15)" identity_verdict "dev=7:2 size=16777216 serial=" 0
  accepts "an identity without diskseq, with --weak-identity" identity_verdict "dev=7:2 size=16777216 serial=" 1
  # And why that is refused: on a kernel without diskseq the fallback is
  # major:minor, size and serial. A loop reattached to the same node gives
  # the same three values. Shown on a copy of this loop's sysfs entry with
  # diskseq left out, taken before and after a real reattach.
  weak_identity_now() {
    local fake=/tmp/fakesys
    rm -rf "$fake"
    mkdir -p "$fake/$(basename "$LOOP")"
    cp "/sys/class/block/$(basename "$LOOP")/dev" "/sys/class/block/$(basename "$LOOP")/size" "$fake/$(basename "$LOOP")/"
    in_writer sh_identity "$fake" "$(basename "$LOOP")"
  }
  weak_before="$(weak_identity_now)"
  strong_before="$(cat "/sys/class/block/$(basename "$LOOP")/diskseq")"
  swap_device
  weak_after="$(weak_identity_now)"
  strong_after="$(cat "/sys/class/block/$(basename "$LOOP")/diskseq")"
  info "fallback identity before and after a reattach: '$weak_before' / '$weak_after'"
  info "diskseq before and after the same reattach: $strong_before / $strong_after"
  expect "the fallback identity cannot see a reattach, which is why it needs --weak-identity" \
    "the fallback identity changed on reattach; the refusal may be stricter than needed" \
    test "$weak_before" == "$weak_after" -a "$strong_before" != "$strong_after"
  refuse "a loop device without the test variable" "is a loop device, not a USB stick" "$LOOP"
  refuse "a loop device over a different file" "is backed by" "$LOOP" "$DECOY"
  refuse "a partition" "is a partition" "${LOOP}p1" "$STICK"
  mkdir -p /mnt/seed
  mount "${LOOP}p1" /mnt/seed
  MOUNTS+=(/mnt/seed)
  refuse "a disk with a mounted partition" "is mounted" "$LOOP" "$STICK"
  umount /mnt/seed
  refuse "a device too small for image plus persistence" "need" "$SMALL_LOOP" "$SMALL"

  # Review finding: an external backup drive passes every other check.
  refuse "a 300 GiB device without --allow-large" "pass --allow-large" "$BIG_LOOP" "$BIG"
  out="$(echo no | SONORA_WRITE_USB_TEST_IMAGE="$BIG" bash "$WRITER" --allow-large "$BIG_LOOP" 2>&1)" || true
  expect "--allow-large admits a 300 GiB device as far as the prompt, showing its size" \
    "--allow-large did not reach the prompt for a 300 GiB device" \
    test "$(printf '%s\n' "$out" | grep -c -e "Target device (322 GB)" -e "aborted.")" == 2

  # Review finding: lsblk is blind to other mount namespaces. The kernel's
  # exclusive-open check is not.
  mount_elsewhere
  info "lsblk here sees mounts on $LOOP: [$(lsblk -nlo MOUNTPOINT "$LOOP" | tr -d '[:space:]')]"
  refuse "a disk mounted in another mount namespace" "in use by something this namespace cannot see" "$LOOP" "$STICK"
  unmount_elsewhere

  # Review finding: nothing pinned the device across the prompt. Each of these
  # answers ERASE, so they would erase the image if the recheck failed.
  during_prompt "the device was replaced while the prompt waited" "it is not the device you confirmed. As for the one you confirmed: Nothing was written" swap_device
  # Second review: the old probe, wipefs --no-act, issued BLKRRPART. On a
  # desktop that re-adds the stick's partitions and gets them automounted
  # while the prompt waits, so the recheck refuses and every retry repeats.
  # Everything before the prompt must leave the device alone.
  echo no | SONORA_WRITE_USB_TEST_IMAGE="$STICK" \
    strace -f -qq -e trace=openat,ioctl -o /work/probe.strace bash "$WRITER" "$LOOP" > /dev/null 2>&1 || true
  info "opens of $LOOP before the prompt: $(grep -c "\"$LOOP\"" /work/probe.strace)"
  expect "before the prompt the device is opened O_RDONLY|O_EXCL (the in-use probe)" \
    "no O_RDONLY|O_EXCL open of the device before the prompt" \
    grep -q "\"$LOOP\", O_RDONLY|O_EXCL" /work/probe.strace
  expect "before the prompt nothing issues BLKRRPART" \
    "something re-read the partition table before the prompt: $(grep -m1 BLKRRPART /work/probe.strace)" \
    sh -c "! grep -q BLKRRPART /work/probe.strace"
  expect "before the prompt nothing opens the device or its partitions for writing" \
    "a write-mode open before the prompt: $(grep -m1 -E "\"$LOOP(p[0-9]+)?\", O_(RDWR|WRONLY)" /work/probe.strace)" \
    sh -c "! grep -q -E '\"$LOOP(p[0-9]+)?\", O_(RDWR|WRONLY)' /work/probe.strace"

  during_prompt "the device was mounted elsewhere while the prompt waited" \
    "in use by something this namespace cannot see" mount_elsewhere
  unmount_elsewhere

  # Review finding: on a desktop, udev re-reads the table when dd closes the
  # device and udisks mounts the fresh image, so BLKRRPART fails with EBUSY
  # and the stick was left without persistence. This drives write-usb.sh's
  # own partition step in exactly that state.
  echo "--- adding partition 3 while the fresh image is in use ---"
  iso_bytes="$(stat -Lc%s "$ISO")"
  p3_mib=$(( (iso_bytes + 1048575) / 1048576 ))
  p3_start=$(( p3_mib * 2048 ))
  # The stale partition 3 is 1 MiB later than the new one, as after a
  # previous image of a different size, so partx must move it rather than
  # find it already right.
  for held in "partition 1" "the whole disk" "a stale partition 3 at a different start, and partition 1"; do
    rm -f "$PARTTEST"
    truncate -s 2G "$PARTTEST"
    dd if="$ISO" of="$PARTTEST" bs=4M conv=notrunc status=none
    if [[ "$held" == *stale* ]]; then
      # The kernel keeps the old partition 3 when dd rewrites the MBR.
      echo "start=$(( p3_start + 2048 )), type=83" | sfdisk -q --append --no-reread --no-tell-kernel --wipe never "$PARTTEST" >/dev/null 2>&1
      attach PT_LOOP "$PARTTEST"
      dd if="$ISO" of="$PT_LOOP" bs=4M conv=fsync status=none
      stale_start="$(cat "/sys/class/block/$(basename "$PT_LOOP")/$(basename "$PT_LOOP")p3/start" 2>/dev/null || echo none)"
      expect "precondition: the kernel holds a stale partition 3 at sector $stale_start" \
        "precondition failed: the kernel's partition 3 is at '$stale_start', not $(( p3_start + 2048 ))" \
        test "$stale_start" == "$(( p3_start + 2048 ))"
    else
      attach PT_LOOP "$PARTTEST"
    fi
    mkdir -p /mnt/held
    if [[ "$held" == "the whole disk" ]]; then mount -o ro "$PT_LOOP" /mnt/held; else mount -o ro "${PT_LOOP}p1" /mnt/held; fi
    echo "start=${p3_start}, type=83" | sfdisk -q --append --no-reread --no-tell-kernel --wipe never --wipe-partitions never "$PT_LOOP" >/dev/null 2>&1
    if blockdev --rereadpt "$PT_LOOP" 2>/dev/null; then
      bad "with $held mounted, BLKRRPART unexpectedly succeeded; the fallback is not being exercised"
    else
      info "with $held mounted, BLKRRPART fails as it does on a desktop"
    fi
    pt_name="$(basename "$PT_LOOP")"
    found="$(in_writer add_and_find "$PT_LOOP" "$pt_name" "$p3_start" 2>&1 || true)"
    expect "partition 3 reaches the kernel with $held mounted ($found)" \
      "partition 3 did not reach the kernel with $held mounted: $found" \
      test "$found" == "${PT_LOOP}p3"
    umount /mnt/held
    losetup -d "$PT_LOOP"
  done

  before="$(sha256sum < "$STICK")"
  out="$(echo no | SONORA_WRITE_USB_TEST_IMAGE="$STICK" bash "$WRITER" "$LOOP" 2>&1)" || true
  if [[ "$out" == *aborted* && "$(sha256sum < "$STICK")" == "$before" ]]; then
    ok "an answer other than ERASE aborts and writes nothing"
  else
    bad "an answer other than ERASE did not abort cleanly"
    printf '%s\n' "$out" | sed 's/^/WRITE-USB info    /'
  fi

  # Second review: a device named by a content-based link, such as
  # /dev/disk/by-label/<label>, loses that link the moment wipefs erases the
  # label, and the script used to re-resolve its argument after the prompt.
  # Here the link is removed while the prompt waits; the write must go ahead,
  # because the device itself has not changed. Traced, to check the order of
  # opens that makes the write provably land on the confirmed device.
  echo "--- write over a used stick, named by a link that vanishes during the prompt ---"
  mkdir -p /tmp/by-label
  ln -sfn "$LOOP" /tmp/by-label/OLDSTICK
  rm -f /work/answer
  mkfifo /work/answer
  ( SONORA_WRITE_USB_TEST_IMAGE="$STICK" strace -f -qq -e trace=openat,ioctl -o /work/write1.strace \
      bash "$WRITER" /tmp/by-label/OLDSTICK < /work/answer > /work/write1.log 2>&1
    echo "exit=$?" >> /work/write1.log ) &
  w_pid=$!
  exec 7>/work/answer
  for _ in $(seq 1 200); do
    grep -q "Target device" /work/write1.log 2>/dev/null && break
    sleep 0.1
  done
  rm -f /tmp/by-label/OLDSTICK
  echo ERASE >&7
  exec 7>&-
  wait "$w_pid" || true
  expect "write-usb.sh exits 0 although the link it was given is gone" \
    "write-usb.sh failed after its link vanished" \
    grep -q "^exit=0$" /work/write1.log
  tr '\r' '\n' < /work/write1.log | grep -v -e "bytes .* copied" -e '^$' | sed 's/^/WRITE-USB info    /'

  # The script's own descriptor (O_RDWR without O_EXCL, from exec 3<>) must
  # be opened before wipefs's O_RDWR|O_EXCL open, so that the identity check
  # between them covers both.
  first_fd="$(grep -n "\"$LOOP\", O_RDWR" /work/write1.strace | grep -v O_EXCL | head -1 | cut -d: -f1)"
  wipefs_open="$(grep -n "\"$LOOP\", O_RDWR|O_EXCL" /work/write1.strace | head -1 | cut -d: -f1)"
  info "strace line of the script's write descriptor: ${first_fd:-none}; of wipefs's exclusive open: ${wipefs_open:-none}"
  expect "the write descriptors are opened before wipefs, inside the identity bracket" \
    "wipefs opened the device before the script's descriptors" \
    test -n "$first_fd" -a -n "$wipefs_open" -a "${first_fd:-0}" -lt "${wipefs_open:-0}"
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
  # Some were detached already by the tests above; the host checks for leaks.
  for l in "${LOOPS[@]}"; do losetup -d "$l" 2>/dev/null || true; done
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
