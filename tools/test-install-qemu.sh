#!/usr/bin/env bash
# Phase 6 exit test for the installer (ADR-0032): install the image to a disk
# in QEMU, restart WITHOUT the stick, and find the installed system running
# with a session that was recorded before the install.
#
#   run 1  boots the ISO (the stick) with a blank virtio disk. A one-shot
#          script, passed on the kernel command line (systemd.run), writes a
#          session into /home/sonora/Sessions, runs
#              sonora-install --yes --target /dev/vda
#          reports the exit code on the serial port and powers the guest off.
#          The installed boot line carries the smoke test's installed-system
#          checks, so run 2 can report without a login (SONORA_CMDLINE).
#   run 2  boots ONLY the virtio disk: no CD, no -kernel, no initrd. The
#          firmware finds the installed GRUB (UEFI: the removable path, since
#          the NVRAM of run 1 is thrown away; BIOS: the MBR gap). The smoke
#          test then asserts the engine, the RT kernel, an ext4 root that is
#          not a live overlay, no live medium, and that the carried session
#          file has the SHA-256 it had on the stick.
#
# It never touches a real disk: the only block device written is the guest's
# own /dev/vda, backed by a qcow2 file under build-qemu/. Two QEMU runs, no
# more. Needs iso/sonora.iso (tools/build-iso.sh) and QEMU or Docker.
#
#   INSTALL_FIRMWARE=uefi|bios   default uefi when OVMF is found, else bios
#   INSTALL_TIMEOUT=2400         seconds for run 1 (TCG needs far more than KVM)
#   BOOT_TIMEOUT=420             seconds for run 2
set -euo pipefail
# One heavy job at a time on this machine (tools/build-slot.sh).
[[ -n "${SONORA_HEAVY_HELD:-}" ]] || exec "$(dirname "${BASH_SOURCE[0]}")/build-slot.sh" "${BASH_SOURCE[0]}" "$@"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="$ROOT/iso/sonora.iso"
WORK="$ROOT/build-qemu/install"
MEMORY="${QEMU_MEMORY:-4096}"
INSTALL_TIMEOUT="${INSTALL_TIMEOUT:-2400}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-420}"
FAILED=0
ok() { echo "INSTALL-TEST ok    $*"; }
bad() { echo "INSTALL-TEST FAIL  $*"; FAILED=1; }

host_path() {
  if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
    (cd "$1" && pwd -W)
  else
    printf '%s' "$1"
  fi
}
[[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]] && export MSYS2_ARG_CONV_EXCL='*'

[[ -f "$ISO" ]] || { echo "error: $ISO not found. Run tools/build-iso.sh first." >&2; exit 1; }
for newer in "$ROOT/iso/config" "$ROOT/iso/auto/config"; do
  if [[ "$newer" -nt "$ISO" ]]; then
    echo "error: $newer is newer than $ISO; rebuild first (tools/build-iso.sh)." >&2
    exit 1
  fi
done
mkdir -p "$WORK"
rm -f "$WORK"/*.log "$WORK"/target.qcow2

# --- QEMU on the host, or in a container ---
QEMU=""
for candidate in qemu-system-x86_64 "/c/Program Files/qemu/qemu-system-x86_64.exe" /usr/bin/qemu-system-x86_64; do
  if command -v "$candidate" >/dev/null 2>&1; then QEMU="$candidate"; break; fi
  if [[ -x "$candidate" ]]; then QEMU="$candidate"; break; fi
done
IN_CONTAINER=0
QEMU_IMAGE=sonora-qemu:trixie
CONTAINER=""
if [[ -z "$QEMU" ]]; then
  command -v docker >/dev/null || { echo "error: no qemu-system-x86_64 and no docker." >&2; exit 1; }
  IN_CONTAINER=1
  docker build -q -t "$QEMU_IMAGE" --label org.sonora=1 \
    -f "$(host_path "$ROOT/tools")/Dockerfile.qemu" "$(host_path "$ROOT/tools")" >/dev/null
fi
stop_container() { [[ -z "$CONTAINER" ]] || docker rm -f "$CONTAINER" >/dev/null 2>&1 || true; }
trap stop_container EXIT

KVM_DEV=()
ACCEL=(-accel tcg)
if [[ "$IN_CONTAINER" == 1 ]]; then
  if docker run --rm --privileged --label org.sonora=1 "$QEMU_IMAGE" test -c /dev/kvm 2>/dev/null; then
    KVM_DEV=(--device /dev/kvm); ACCEL=(-accel kvm)
  fi
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
  ACCEL=(-accel whpx -accel tcg)
elif [[ -w /dev/kvm ]]; then
  ACCEL=(-accel kvm)
fi
echo "qemu: ${QEMU:-docker:$QEMU_IMAGE}  accel: ${ACCEL[*]}"

# --- firmware ---
OVMF=""; OVMF_VARS=""
if [[ "$IN_CONTAINER" == 1 ]]; then
  if docker run --rm --label org.sonora=1 -v "$(host_path "$WORK"):/w" "$QEMU_IMAGE" \
       cp /usr/share/OVMF/OVMF_VARS_4M.fd /w/ovmf-template.vars.fd 2>/dev/null; then
    OVMF=/usr/share/OVMF/OVMF_CODE_4M.fd; OVMF_VARS="$WORK/ovmf-template.vars.fd"
  fi
else
  qdir="$(dirname "$(command -v "$QEMU" || echo "$QEMU")")"
  for pair in "$qdir/share/edk2-x86_64-code.fd:$qdir/share/edk2-i386-vars.fd" \
              "/usr/share/qemu/edk2-x86_64-code.fd:/usr/share/qemu/edk2-i386-vars.fd" \
              "/usr/share/OVMF/OVMF_CODE_4M.fd:/usr/share/OVMF/OVMF_VARS_4M.fd" \
              "/usr/share/OVMF/OVMF_CODE.fd:/usr/share/OVMF/OVMF_VARS.fd"; do
    if [[ -f "${pair%%:*}" && -f "${pair#*:}" ]]; then OVMF="${pair%%:*}"; OVMF_VARS="${pair#*:}"; break; fi
  done
fi
FIRMWARE="${INSTALL_FIRMWARE:-}"
if [[ -z "$FIRMWARE" ]]; then if [[ -n "$OVMF" ]]; then FIRMWARE=uefi; else FIRMWARE=bios; fi; fi
if [[ "$FIRMWARE" == uefi && -z "$OVMF" ]]; then echo "error: no OVMF; use INSTALL_FIRMWARE=bios." >&2; exit 1; fi
echo "firmware: $FIRMWARE"

# --- the stick's kernel and initrd, and a blank disk ---
docker run --rm --label org.sonora=1 -v "$(host_path "$ROOT/iso"):/iso:ro" -v "$(host_path "$WORK"):/out" \
  sonora-livebuild:trixie bash -lc '
    set -e
    K=$(xorriso -indev /iso/sonora.iso -find /live 2>/dev/null | tr -d "'"'"'" | grep -E "vmlinuz-.*-rt-" | head -1)
    I=$(xorriso -indev /iso/sonora.iso -find /live 2>/dev/null | tr -d "'"'"'" | grep -E "initrd.img-.*-rt-" | head -1)
    [ -n "$K" ] && [ -n "$I" ] || { echo "E: no -rt- kernel in the ISO" >&2; exit 1; }
    xorriso -osirrox on -indev /iso/sonora.iso -extract "$K" /out/vmlinuz -extract "$I" /out/initrd.img >/dev/null 2>&1
    chmod 0644 /out/vmlinuz /out/initrd.img'
if [[ "$IN_CONTAINER" == 1 ]]; then
  docker run --rm --label org.sonora=1 -v "$(host_path "$WORK"):/w" "$QEMU_IMAGE" qemu-img create -f qcow2 /w/target.qcow2 16G >/dev/null
else
  qemu-img create -f qcow2 "$WORK/target.qcow2" 16G >/dev/null
fi

SHIPPED_APPEND="$(sed -n 's/.*--bootappend-live "\([^"]*\)".*/\1/p' "$ROOT/iso/auto/config")"
[[ -n "$SHIPPED_APPEND" ]] || { echo "error: no --bootappend-live in iso/auto/config." >&2; exit 1; }
# What the installed system's GRUB will pass, as sonora-install builds it
# (the shipped words minus the live-only ones) plus the test-only words.
INSTALLED_BASE="quiet splash threadirqs"

# --- the one-shot script for run 1 ---
# It runs in the guest, so its variables are expanded there.
# shellcheck disable=SC2016
# base64 so it survives the kernel command line (2048 bytes) with no quoting.
GUEST_SCRIPT='D=/home/sonora/Sessions/Take1
mkdir -p $D
head -c 262144 /dev/urandom > $D/take1.wav
echo "{\"name\":\"Take1\"}" > "$D/session.json"
head -c 4096 /dev/urandom > "$D/my take.wav"
chown -R sonora:sonora /home/sonora/Sessions
sync
S=$(sha256sum $D/take1.wav | cut -d" " -f1)
echo INSTALLTEST-SHA $S > /dev/ttyS0
[ -d /sys/firmware/efi ] && echo INSTALLTEST-FW uefi > /dev/ttyS0 || echo INSTALLTEST-FW bios > /dev/ttyS0
export SONORA_CMDLINE="'"$INSTALLED_BASE"' console=ttyS0,115200 sonora.smoketest sonora.expect_installed sonora.expect_file=$D/take1.wav sonora.expect_sha=$S"
sonora-install --yes --target /dev/vda > /dev/ttyS0 2>&1
echo INSTALLTEST-RC $? > /dev/ttyS0'
B64="$(printf '%s\n' "$GUEST_SCRIPT" | base64 | tr -d '\n')"
RUN="systemd.unit=kernel-command-line.target systemd.run_success_action=poweroff systemd.run_failure_action=poweroff"
APPEND="$SHIPPED_APPEND console=ttyS0,115200 $RUN systemd.run=\"/bin/sh -c 'echo $B64 | base64 -d | sh'\""
if (( ${#APPEND} > 1900 )); then echo "error: kernel command line is ${#APPEND} bytes; limit 2048." >&2; exit 1; fi

# --- one QEMU run ---
# qemu_run NAME TIMEOUT STICK(0|1) [WAIT_FOR]: with STICK=1 the ISO and its
# kernel are attached; with 0 only the disk. Sets POWERED_OFF.
POWERED_OFF=0
qemu_run() {
  local name="$1" seconds="$2" stick="$3" w runner fw="" waited=0 pid
  local args=()
  if [[ "$IN_CONTAINER" == 1 ]]; then
    w=/w; fw="$OVMF"
    CONTAINER="sonora-install-qemu-$$-$name"
    runner=(docker run --rm --name "$CONTAINER" --label org.sonora=1 "${KVM_DEV[@]}"
            -v "$(host_path "$WORK"):/w" -v "$(host_path "$ROOT/iso"):/iso:ro" "$QEMU_IMAGE" qemu-system-x86_64)
  else
    w="$(host_path "$WORK")"; [[ -z "$OVMF" ]] || fw="$(host_path "$(dirname "$OVMF")")/$(basename "$OVMF")"
    runner=("$QEMU")
  fi
  args=(-m "$MEMORY" -smp 2 "${ACCEL[@]}" -device intel-hda -device hda-duplex
        -drive "if=none,id=target,format=qcow2,file=$w/target.qcow2" -device "virtio-blk-pci,drive=target"
        -serial "file:$w/$name.serial.log" -display none -monitor none -no-reboot)
  if [[ "$FIRMWARE" == uefi ]]; then
    # A fresh NVRAM for each run: run 2 must find the disk by the removable
    # path, not by a boot entry run 1 left behind.
    cp "$OVMF_VARS" "$WORK/$name.vars.fd"
    args+=(-drive "if=pflash,format=raw,readonly=on,file=$fw" -drive "if=pflash,format=raw,file=$w/$name.vars.fd")
  fi
  if [[ "$stick" == 1 ]]; then
    local iso
    if [[ "$IN_CONTAINER" == 1 ]]; then iso=/iso/sonora.iso; else iso="$(host_path "$ROOT/iso")/sonora.iso"; fi
    args+=(-kernel "$w/vmlinuz" -initrd "$w/initrd.img" -append "$APPEND" -cdrom "$iso")
  fi
  echo "--- $name (stick: $stick, up to ${seconds}s) ---"
  "${runner[@]}" "${args[@]}" < /dev/null > "$WORK/$name.qemu.log" 2>&1 &
  pid=$!
  while (( waited < seconds )) && kill -0 "$pid" 2>/dev/null; do sleep 2; waited=$((waited + 2)); done
  if kill -0 "$pid" 2>/dev/null; then
    POWERED_OFF=0; kill "$pid" 2>/dev/null || true; sleep 2; kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true; stop_container
  else
    POWERED_OFF=1; wait "$pid" 2>/dev/null || true
  fi
  CONTAINER=""
  echo "INSTALL-TEST info  $name: $([[ $POWERED_OFF == 1 ]] && echo "ended by itself" || echo "stopped on the clock") after ${waited}s"
  [[ -s "$WORK/$name.serial.log" ]] || { bad "$name: no serial output (QEMU refused its arguments?)"; sed 's/^/    /' "$WORK/$name.qemu.log"; }
}
serial() { tr -d '\r' < "$WORK/$1.serial.log" 2>/dev/null | grep -a -- "$2" || true; }

# === run 1: install from the stick ===
qemu_run install "$INSTALL_TIMEOUT" 1
RC="$(serial install '^INSTALLTEST-RC' | tail -n 1 | awk '{print $2}')"
SHA_BEFORE="$(serial install '^INSTALLTEST-SHA' | tail -n 1 | awk '{print $2}')"
FW_SEEN="$(serial install '^INSTALLTEST-FW' | tail -n 1 | awk '{print $2}')"
serial install '^STEP \|^INFO \|^sonora-install' | sed 's/^/INSTALL-TEST info    /'
if [[ "$RC" == 0 ]]; then ok "sonora-install exited 0 in the guest"; else bad "sonora-install exit code '${RC:-none}'"; fi
if [[ "$FW_SEEN" == "$FIRMWARE" ]]; then ok "installer ran under $FW_SEEN firmware"; else bad "firmware seen '$FW_SEEN', wanted $FIRMWARE"; fi
for n in 1 2 3 4 5 6 7; do serial install "^STEP $n/7 " | grep -q . || bad "STEP $n/7 never announced"; done
if [[ -n "$SHA_BEFORE" ]]; then ok "session recorded before the install, sha256 ${SHA_BEFORE:0:16}..."; else bad "no session checksum from the stick"; fi
[[ "$POWERED_OFF" == 1 ]] || bad "run 1 did not end by itself"

# === run 2: the disk alone ===
qemu_run installed "$BOOT_TIMEOUT" 0
serial installed 'SMOKETEST' | sed 's/^/INSTALL-TEST info    /' | grep -a 'FAIL\|RESULT\|root \|carried\|kernel ' || true
if serial installed 'SMOKETEST RESULT pass' | grep -q .; then ok "installed system booted from its own disk and passed the smoke test"; else bad "installed boot did not pass the smoke test"; fi
if serial installed 'SMOKETEST ok    install marker present' | grep -q .; then ok "install marker present"; else bad "no install marker after the restart"; fi
if serial installed 'SMOKETEST ok    carried-over session file matches' | grep -q .; then ok "carried session file matches its checksum"; else bad "carried session file missing or changed"; fi
SHA_AFTER="$(serial installed 'carried file' | sed -n 's/.*sha256 \([0-9a-f]*\).*/\1/p' | tail -n 1)"
if [[ -n "$SHA_BEFORE" && "$SHA_BEFORE" == "$SHA_AFTER" ]]; then ok "sha256 before the install equals sha256 after the restart"; else bad "sha256 before '$SHA_BEFORE' after '$SHA_AFTER'"; fi
if serial installed 'SMOKETEST ok    kernel is -rt-' | grep -q .; then ok "installed system runs the RT kernel"; else bad "installed system not on the RT kernel"; fi
[[ "$POWERED_OFF" == 1 ]] || bad "installed system did not power itself off after the smoke test"

echo
if [[ "$FAILED" == 0 ]]; then echo "INSTALL-TEST RESULT pass ($FIRMWARE)"; else echo "INSTALL-TEST RESULT fail ($FIRMWARE); logs in $WORK"; exit 1; fi
