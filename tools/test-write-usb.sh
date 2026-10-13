#!/usr/bin/env bash
# Run tools/write-usb.sh for real, against a disk image instead of a stick,
# then boot what it wrote and check that /home survives a reboot.
#
# Nothing here touches a real block device. The writer runs in a privileged
# container against a loop device over a sparse file in a Docker volume, with
# SONORA_WRITE_USB_TEST_IMAGE naming that file -- the only way write-usb.sh
# accepts a loop device at all. Before writing anything, the test points the
# script at every disk the container can see, the container's own root disk
# among them, and asserts that each is refused before the prompt.
#
# Then QEMU boots the written image as a USB mass-storage device:
#
#   1. kernel boot, writes a random marker into /home/sonora, powers off
#      -- then the partition is read offline: the marker must be on it
#   2. kernel boot, reads the marker back, reports what /home is mounted from
#   3. kernel boot with the smoke test: the shipped system, persistence live
#      -- then: marker still there, engine's session folder on the stick,
#         filesystem clean
#   4. BIOS through the stick's own isolinux, shipped command line, no hands
#   5. UEFI through the stick's own GRUB and EFI partition, likewise
#      -- 4 and 5 have no serial console, so the proof is on the partition:
#         the engine's session folder appears there only if the boot got all
#         the way from the bootloader to a running engine with persistence
#
# Boots 1-3 use -kernel to pass test-only parameters, as test-boot-qemu.sh
# does; 4 and 5 are what cover the bootloaders. Each boot starts from its own
# qcow2 overlay over the written image, so they cannot contaminate each other.
#
# WRITE_USB_BOOT=0 skips the QEMU half, leaving the container half, which
# needs only Docker and runs in about a minute.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="$ROOT/iso/sonora.iso"
WORK="$ROOT/build-usb"
IMAGE=sonora-usbtest:trixie
VOLUME=sonora-usbtest-work
MEMORY="${QEMU_MEMORY:-4096}"
KERNEL_BOOT_TIMEOUT="${KERNEL_BOOT_TIMEOUT:-240}"
BOOTLOADER_SECONDS="${BOOTLOADER_SECONDS:-120}"
DO_BOOT="${WRITE_USB_BOOT:-1}"

host_path() {
  if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
    (cd "$1" && pwd -W)
  else
    printf '%s' "$1"
  fi
}

if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
  export MSYS2_ARG_CONV_EXCL='*'
fi

[[ -f "$ISO" ]] || {
  echo "error: $ISO not found. Run tools/build-iso.sh first." >&2
  exit 1
}
# Same rule as test-boot-qemu.sh: a verdict about yesterday's image is worse
# than none.
for newer in "$ROOT/iso/config" "$ROOT/iso/auto/config" "$ROOT/CMakeLists.txt" \
             "$ROOT/common" "$ROOT/contracts" "$ROOT/engine" "$ROOT/drivers" "$ROOT/ui"; do
  if [[ "$newer" -nt "$ISO" ]]; then
    echo "error: $newer is newer than $ISO. Rebuild first: tools/build-iso.sh" >&2
    exit 1
  fi
done

command -v docker >/dev/null || { echo "error: docker is required." >&2; exit 1; }
docker info >/dev/null 2>&1 || {
  echo "error: the Docker daemon is not reachable. Start Docker Desktop." >&2
  exit 1
}

mkdir -p "$WORK"
rm -f "$WORK"/*.serial.log "$WORK"/*.qemu.log "$WORK"/*.qcow2 "$WORK"/*.vars.fd

FAILED=0
ok()  { echo "WRITE-USB ok    $1"; }
bad() { echo "WRITE-USB FAIL  $1"; FAILED=1; }
# expect OK_MESSAGE FAIL_MESSAGE COMMAND...
expect() {
  local good="$1" fail="$2"
  shift 2
  if "$@"; then ok "$good"; else bad "$fail"; fi
}

# Loop devices belong to the Docker VM, not the container, and outlive it. One
# left bound keeps its deleted 8 GB image allocated until the VM restarts;
# that happened on every run until the entrypoint's cleanup was fixed.
check_loop_leaks() {
  local leaked
  leaked="$(docker run --rm --privileged "$IMAGE" losetup -a \
    | grep -E '/(stick|small|booted|decoy|big|part)\.img' || true)"
  expect "no loop devices left bound to the test's images" \
    "loop devices leaked: $leaked" \
    test -z "$leaked"
}

in_container() {
  docker run --rm --privileged \
    -v "$(host_path "$ROOT"):/src:ro" \
    -v "$(host_path "$WORK"):/out" \
    -v "$VOLUME:/work" \
    "$@"
}

echo "=== building the test image ==="
docker build -q -t "$IMAGE" \
  -f "$(host_path "$ROOT/tools")/Dockerfile.usbtest" \
  "$(host_path "$ROOT/tools")" >/dev/null
docker volume create "$VOLUME" >/dev/null

echo "=== write-usb.sh against a loop device ==="
# The disk image lives in the volume, which is ext4 inside the Linux VM:
# sparse, and a loop device over a Windows bind mount is not something to
# depend on.
if ! in_container "$IMAGE" bash /src/tools/test-write-usb-entrypoint.sh write; then
  echo "WRITE-USB TEST FAIL (writing; nothing was booted)" >&2
  exit 1
fi

if [[ "$DO_BOOT" != 1 ]]; then
  check_loop_leaks
  echo
  if [[ "$FAILED" != 0 ]]; then
    echo "WRITE-USB TEST FAIL" >&2
    exit 1
  fi
  echo "WRITE-USB TEST PASS (container half only; WRITE_USB_BOOT=0, nothing booted)"
  exit 0
fi

# --- QEMU ---
QEMU=""
for candidate in \
  qemu-system-x86_64 \
  "/c/Program Files/qemu/qemu-system-x86_64.exe" \
  "/usr/bin/qemu-system-x86_64"; do
  if command -v "$candidate" >/dev/null 2>&1; then QEMU="$candidate"; break; fi
  if [[ -x "$candidate" ]]; then QEMU="$candidate"; break; fi
done
[[ -n "$QEMU" ]] || { echo "error: qemu-system-x86_64 not found (WRITE_USB_BOOT=0 skips booting)." >&2; exit 1; }
QEMU_DIR="$(dirname "$(command -v "$QEMU" || echo "$QEMU")")"

# UEFI firmware as a code/vars pair for pflash. Not -bios: QEMU's -bios wants
# a size that is a multiple of 64 KiB, and edk2-x86_64-code.fd as shipped with
# QEMU for Windows is 0x37C000 bytes, so QEMU exits at once with "could not
# load PC BIOS". QEMU's own x86_64 firmware descriptor pairs that code with
# edk2-i386-vars.fd.
OVMF=""
OVMF_VARS=""
for pair in \
  "$QEMU_DIR/share/edk2-x86_64-code.fd:$QEMU_DIR/share/edk2-i386-vars.fd" \
  "/c/Program Files/qemu/share/edk2-x86_64-code.fd:/c/Program Files/qemu/share/edk2-i386-vars.fd" \
  "/usr/share/qemu/edk2-x86_64-code.fd:/usr/share/qemu/edk2-i386-vars.fd" \
  "/usr/share/OVMF/OVMF_CODE_4M.fd:/usr/share/OVMF/OVMF_VARS_4M.fd" \
  "/usr/share/OVMF/OVMF_CODE.fd:/usr/share/OVMF/OVMF_VARS.fd"; do
  code="${pair%%:*}"
  vars="${pair#*:}"
  if [[ -f "$code" && -f "$vars" ]]; then OVMF="$code"; OVMF_VARS="$vars"; break; fi
done

if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
  ACCEL=(-accel whpx -accel tcg)
elif [[ -w /dev/kvm ]]; then
  ACCEL=(-accel kvm)
else
  ACCEL=(-accel tcg)
fi

# The shipped command line, read from where it is defined rather than copied,
# so the kernel boots cannot drift from what the bootloaders pass.
SHIPPED_APPEND="$(sed -n 's/.*--bootappend-live "\([^"]*\)".*/\1/p' "$ROOT/iso/auto/config")"
[[ -n "$SHIPPED_APPEND" ]] || { echo "error: no --bootappend-live in iso/auto/config." >&2; exit 1; }

# boot NAME DISK MODE SECONDS [EXTRA_APPEND]
#   kernel     -kernel/-initrd from the stick, shipped parameters plus the
#              serial console and EXTRA_APPEND. Waits up to SECONDS for the
#              guest to power itself off.
#   bios|uefi  through the stick's own bootloader, nothing added. The shipped
#              image never powers off, so it runs SECONDS and is stopped.
# Sets POWERED_OFF to 1 if the guest powered off by itself.
POWERED_OFF=0
boot() {
  local name="$1" disk="$2" mode="$3" seconds="$4" extra="${5:-}"
  local w pid waited=0
  w="$(host_path "$WORK")"
  local args=(
    -m "$MEMORY" -smp 2 "${ACCEL[@]}"
    -device qemu-xhci
    -drive "if=none,id=stick,format=qcow2,file=$w/$disk"
    -device intel-hda -device hda-duplex
    -serial "file:$w/$name.serial.log"
    -display none -monitor none -no-reboot
  )
  # bootindex only for the bootloader boots: -kernel's option ROM already
  # holds index 0, and QEMU refuses to start if the stick asks for it too.
  case "$mode" in
    kernel)
      args+=(-device "usb-storage,drive=stick,removable=on"
             -kernel "$w/vmlinuz" -initrd "$w/initrd.img"
             -append "$SHIPPED_APPEND console=ttyS0,115200 $extra") ;;
    uefi)
      # A fresh vars store each time: boot entries a previous run recorded
      # would let the firmware skip the removable-media path being tested.
      cp "$OVMF_VARS" "$WORK/$name.vars.fd"
      args+=(-device "usb-storage,drive=stick,removable=on,bootindex=0"
             -drive "if=pflash,format=raw,readonly=on,file=$(host_path "$(dirname "$OVMF")")/$(basename "$OVMF")"
             -drive "if=pflash,format=raw,file=$w/$name.vars.fd") ;;
    bios)
      args+=(-device "usb-storage,drive=stick,removable=on,bootindex=0") ;;
  esac
  echo "--- boot: $name ($mode, ${seconds}s) ---"
  "$QEMU" "${args[@]}" < /dev/null > "$WORK/$name.qemu.log" 2>&1 &
  pid=$!
  while [[ "$waited" -lt "$seconds" ]] && kill -0 "$pid" 2>/dev/null; do
    sleep 2
    waited=$((waited + 2))
  done
  if kill -0 "$pid" 2>/dev/null; then
    POWERED_OFF=0
    kill "$pid" 2>/dev/null || true
    sleep 2
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  else
    POWERED_OFF=1
    wait "$pid" 2>/dev/null || true
  fi
  # A guest that "powers off" in seconds with nothing on its serial port is
  # QEMU refusing its arguments. Say so, rather than letting it read as a
  # boot that ran and failed its checks.
  if [[ "$mode" == kernel && ! -s "$WORK/$name.serial.log" ]]; then
    bad "$name: QEMU produced no serial output at all"
    sed 's/^/WRITE-USB info    /' "$WORK/$name.qemu.log"
  fi
  echo "WRITE-USB info  $name: $( [[ "$POWERED_OFF" == 1 ]] && echo "powered off after about ${waited}s" || echo "stopped after ${waited}s")"
}

# Called through expect, which shellcheck cannot follow.
# shellcheck disable=SC2317,SC2329
serial_has() { tr -d '\r' < "$WORK/$1.serial.log" 2>/dev/null | grep -a -q -- "$2"; }

inspect() {
  if ! in_container "$@" "$IMAGE" bash /src/tools/test-write-usb-entrypoint.sh inspect; then
    FAILED=1
  fi
}

# Random per run, so a marker cannot come from a previous run or the image.
NONCE="sonora-$(date +%s)-$RANDOM$RANDOM"
echo "marker for this run: $NONCE"

# systemd.run= needs systemd.unit= alongside it here. The generator links
# default.target to its own target, but the image's set-default puts a
# default.target link in /etc, which outranks generator output, so without
# systemd.unit= the command is silently never run.
RUN="systemd.unit=kernel-command-line.target systemd.run_success_action=poweroff systemd.run_failure_action=poweroff"

# --- 1. write ---
boot persist-write persist.qcow2 kernel "$KERNEL_BOOT_TIMEOUT" \
  "$RUN systemd.run=\"/bin/sh -c 'test ! -e /home/sonora/persistence-marker && echo $NONCE > /home/sonora/persistence-marker && sync && echo PERSIST-WRITE done > /dev/ttyS0'\""
expect "boot 1: guest wrote the marker into /home/sonora (and it was not there before)" \
  "boot 1: guest did not report writing the marker" \
  serial_has persist-write "PERSIST-WRITE done"
expect "boot 1: guest powered off by itself" "boot 1: guest did not power off" \
  test "$POWERED_OFF" == 1
inspect -e DISK=persist.qcow2 -e LABEL="after boot 1" -e CLEAN_SHUTDOWN=1 -e EXPECT_MARKER="$NONCE"

# --- 2. read back ---
boot persist-read persist.qcow2 kernel "$KERNEL_BOOT_TIMEOUT" \
  "$RUN systemd.run=\"/bin/sh -c 'sed s/^/PERSIST-READ:/ /home/sonora/persistence-marker > /dev/ttyS0; findmnt -no FSTYPE,OPTIONS /home | sed s/^/PERSIST-HOME:/ > /dev/ttyS0'\""
tr -d '\r' < "$WORK/persist-read.serial.log" | grep -a -o "PERSIST-[A-Z]*:.*" | sed 's/^/WRITE-USB info  boot 2: /' || true
expect "boot 2: the marker survived the reboot" \
  "boot 2: the marker did not survive the reboot" \
  serial_has persist-read "PERSIST-READ:$NONCE"
expect "boot 2: /home is an overlay whose upper layer is on the persistence partition" \
  "boot 2: /home is not an overlay on the persistence partition" \
  serial_has persist-read "PERSIST-HOME:overlay.*upperdir=/run/live/persistence/"

# --- 3. the shipped system, with persistence live ---
boot persist-smoke persist.qcow2 kernel "$KERNEL_BOOT_TIMEOUT" "sonora.smoketest"
tr -d '\r' < "$WORK/persist-smoke.serial.log" | grep -a "SMOKETEST \(FAIL\|RESULT\)" | sed 's/^/WRITE-USB info  boot 3: /' || true
expect "boot 3: the boot smoke test passes from the stick with persistence active" \
  "boot 3: the boot smoke test did not pass from the stick" \
  serial_has persist-smoke "SMOKETEST RESULT pass"
inspect -e DISK=persist.qcow2 -e LABEL="after boot 3" -e CLEAN_SHUTDOWN="$POWERED_OFF" \
  -e EXPECT_MARKER="$NONCE" -e EXPECT_SESSION=1

# --- 4 and 5. the bootloaders, unattended ---
boot bootloader-bios bios.qcow2 bios "$BOOTLOADER_SECONDS"
inspect -e DISK=bios.qcow2 -e LABEL="BIOS boot" -e EXPECT_SESSION=1

if [[ -n "$OVMF" ]]; then
  boot bootloader-uefi uefi.qcow2 uefi "$BOOTLOADER_SECONDS"
  inspect -e DISK=uefi.qcow2 -e LABEL="UEFI boot" -e EXPECT_SESSION=1
else
  bad "no OVMF firmware found; the UEFI path was not tested"
fi

check_loop_leaks

echo
if [[ "$FAILED" == 0 ]]; then
  echo "WRITE-USB TEST PASS"
  exit 0
fi
echo "WRITE-USB TEST FAIL. Serial logs: $WORK/*.serial.log" >&2
exit 1
