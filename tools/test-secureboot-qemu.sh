#!/usr/bin/env bash
# Secure Boot boot test of an installed disk (ADR-0034, ADR-0099).
#
#   run 1  boots the ISO (the stick, ordinary OVMF: QEMU's -kernel is not
#          signed) and runs sonora-install onto a blank virtio disk.
#   run 2  boots ONLY the disk under OVMF secboot firmware with the
#          Microsoft-enrolled VARS. A one-shot unit on the installed kernel
#          command line runs `sonora-secureboot-check --require-on` and
#          prints it on the serial port. Asserts secure_boot=on,
#          lockdown=integrity, kernel_rt=yes.
#   run 3  the same, with a FRESH copy of the VARS (NVRAM cleared): the disk
#          must still boot, through the removable path (EFI/BOOT/BOOTX64.EFI).
#
# Never touches a real disk. Needs iso/sonora.iso, qemu-system-x86_64, docker
# (to pull the kernel out of the ISO, as tools/test-install-qemu.sh does).
#   SB_INSTALL_TIMEOUT=900  SB_BOOT_TIMEOUT=420   seconds at KVM speed
# Exit 0 pass, or UNAVAILABLE (secboot firmware files absent); 1 fail;
# 77 no qemu/docker here; 3 TCG clock ran out.
set -euo pipefail
[[ -n "${SONORA_HEAVY_HELD:-}" ]] || exec "$(dirname "${BASH_SOURCE[0]}")/build-slot.sh" "${BASH_SOURCE[0]}" "$@"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/artifacts.sh
# shellcheck disable=SC1091
. "$ROOT/tools/lib/artifacts.sh"
# shellcheck source=tools/lib/qemu-accel.sh
# shellcheck disable=SC1091
. "$ROOT/tools/lib/qemu-accel.sh"
ISO="$SONORA_ISO_DIR/sonora.iso"
WORK="$SONORA_QEMU_DIR/secureboot"
MEMORY="${QEMU_MEMORY:-4096}"
INSTALL_TIMEOUT="${SB_INSTALL_TIMEOUT:-900}"
BOOT_TIMEOUT="${SB_BOOT_TIMEOUT:-420}"
FAILED=0
CLOCK_STOPPED=0
ok() { echo "SB-TEST ok    $*"; }
bad() { echo "SB-TEST FAIL  $*"; FAILED=1; }

# Firmware first: its absence is the only reason to skip with exit 0.
SB_CODE=""; SB_VARS=""; PLAIN_CODE=""; PLAIN_VARS=""
for d in /usr/share/OVMF /usr/share/edk2/ovmf /usr/share/qemu; do
  if [[ -f "$d/OVMF_CODE_4M.secboot.fd" && -f "$d/OVMF_VARS_4M.ms.fd" ]]; then
    SB_CODE="$d/OVMF_CODE_4M.secboot.fd"; SB_VARS="$d/OVMF_VARS_4M.ms.fd"; break
  fi
done
if [[ -z "$SB_CODE" ]]; then
  echo "SB-TEST UNAVAILABLE: OVMF_CODE_4M.secboot.fd / OVMF_VARS_4M.ms.fd not found (apt install ovmf)."
  exit 0
fi
for pair in "/usr/share/OVMF/OVMF_CODE_4M.fd:/usr/share/OVMF/OVMF_VARS_4M.fd" \
            "/usr/share/qemu/edk2-x86_64-code.fd:/usr/share/qemu/edk2-i386-vars.fd"; do
  if [[ -f "${pair%%:*}" && -f "${pair#*:}" ]]; then PLAIN_CODE="${pair%%:*}"; PLAIN_VARS="${pair#*:}"; break; fi
done
[[ -n "$PLAIN_CODE" ]] || { echo "error: no plain OVMF for the install run." >&2; exit 1; }

QEMU="$(command -v qemu-system-x86_64 || true)"
if [[ -z "$QEMU" ]] || ! command -v docker >/dev/null || ! docker info >/dev/null 2>&1; then
  echo "SB-TEST SKIP: needs qemu-system-x86_64 and docker." >&2; exit "$QEMU_EXIT_SKIP"
fi
[[ -f "$ISO" ]] || { echo "error: $ISO not found. Run tools/build-iso.sh first." >&2; exit 1; }
qemu_pick_accel host 0
INSTALL_TIMEOUT="$(qemu_scale "$INSTALL_TIMEOUT")"
BOOT_TIMEOUT="$(qemu_scale "$BOOT_TIMEOUT")"
SLOW_APPEND="$(qemu_append_slow)"
mkdir -p "$WORK"
rm -f "$WORK"/*.log "$WORK"/target.qcow2 "$WORK"/*.vars.fd
echo "qemu: $QEMU  accel: ${ACCEL[*]}  secboot: $SB_CODE"

docker run --rm --label org.sonora=1 -v "$SONORA_ISO_DIR:/iso:ro" -v "$WORK:/out" \
  sonora-livebuild:trixie bash -lc '
    set -e
    K=$(xorriso -indev /iso/sonora.iso -find /live 2>/dev/null | tr -d "'"'"'" | grep -E "vmlinuz-.*-rt-" | head -1)
    I=$(xorriso -indev /iso/sonora.iso -find /live 2>/dev/null | tr -d "'"'"'" | grep -E "initrd.img-.*-rt-" | head -1)
    [ -n "$K" ] && [ -n "$I" ] || { echo "E: no -rt- kernel in the ISO" >&2; exit 1; }
    xorriso -osirrox on -indev /iso/sonora.iso -extract "$K" /out/vmlinuz -extract "$I" /out/initrd.img >/dev/null 2>&1
    chmod 0644 /out/vmlinuz /out/initrd.img'
qemu-img create -f qcow2 "$WORK/target.qcow2" 16G >/dev/null

SHIPPED_APPEND="$(sed -n 's/.*--bootappend-live "\([^"]*\)".*/\1/p' "$ROOT/iso/auto/config")"
[[ -n "$SHIPPED_APPEND" ]] || { echo "error: no --bootappend-live in iso/auto/config." >&2; exit 1; }
RUN="systemd.unit=kernel-command-line.target systemd.run_success_action=poweroff systemd.run_failure_action=poweroff"

# The installed system runs this once per boot. Output goes to the serial
# port; the rc line lets the host tell "check ran, SB off" from "never ran".
CHECK_SCRIPT='echo SBTEST-BEGIN > /dev/ttyS0
sonora-secureboot-check --require-on > /dev/ttyS0 2>&1
echo SBTEST-RC $? > /dev/ttyS0'
CHECK_B64="$(printf '%s\n' "$CHECK_SCRIPT" | base64 | tr -d '\n')"
# sonora-install writes this into /etc/default/grub between double quotes, so
# the quotes systemd.run needs are escaped.
INSTALLED_CMDLINE="quiet splash threadirqs console=ttyS0,115200${SLOW_APPEND:+ $SLOW_APPEND} $RUN systemd.run=\\\"/bin/sh -c 'echo $CHECK_B64 | base64 -d | sh'\\\""

INSTALL_SCRIPT="export SONORA_CMDLINE=\"$INSTALLED_CMDLINE\"
sonora-install --yes --target /dev/vda > /dev/ttyS0 2>&1
echo SBTEST-INSTALL-RC \$? > /dev/ttyS0"
B64="$(printf '%s\n' "$INSTALL_SCRIPT" | base64 | tr -d '\n')"
APPEND="$SHIPPED_APPEND console=ttyS0,115200${SLOW_APPEND:+ $SLOW_APPEND} $RUN systemd.run=\"/bin/sh -c 'echo $B64 | base64 -d | sh'\""
if (( ${#APPEND} > 1900 )); then echo "error: kernel command line is ${#APPEND} bytes; limit 2048." >&2; exit 1; fi

POWERED_OFF=0
# qemu_run NAME TIMEOUT STICK(0|1) CODE VARS_TEMPLATE
qemu_run() {
  local name="$1" seconds="$2" stick="$3" code="$4" vars="$5" waited=0 pid
  local machine="q35,smm=on" secure="driver=cfi.pflash01,property=secure,value=on"
  local args=(-machine "$machine" -global "$secure"
        -m "$MEMORY" -smp 2 "${ACCEL[@]}"
        -drive "if=none,id=target,format=qcow2,file=$WORK/target.qcow2" -device "virtio-blk-pci,drive=target"
        -serial "file:$WORK/$name.serial.log" -display none -monitor none -no-reboot)
  # A fresh NVRAM per run: no boot entry survives, so the disk is found by
  # the removable path alone.
  cp "$vars" "$WORK/$name.vars.fd"
  args+=(-drive "if=pflash,format=raw,readonly=on,file=$code" -drive "if=pflash,format=raw,file=$WORK/$name.vars.fd")
  if [[ "$stick" == 1 ]]; then
    args+=(-kernel "$WORK/vmlinuz" -initrd "$WORK/initrd.img" -append "$APPEND" -cdrom "$ISO")
  fi
  echo "--- $name (stick: $stick, up to ${seconds}s) ---"
  "$QEMU" "${args[@]}" < /dev/null > "$WORK/$name.qemu.log" 2>&1 &
  pid=$!
  while (( waited < seconds )) && kill -0 "$pid" 2>/dev/null; do sleep 2; waited=$((waited + 2)); done
  if kill -0 "$pid" 2>/dev/null; then
    POWERED_OFF=0; CLOCK_STOPPED=1; kill "$pid" 2>/dev/null || true; sleep 2; kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  else
    POWERED_OFF=1; wait "$pid" 2>/dev/null || true
  fi
  echo "SB-TEST info  $name: $([[ $POWERED_OFF == 1 ]] && echo "ended by itself" || echo "stopped on the clock") after ${waited}s"
  [[ -s "$WORK/$name.serial.log" ]] || { bad "$name: no serial output"; sed 's/^/    /' "$WORK/$name.qemu.log"; }
}
serial() { tr -d '\r' < "$WORK/$1.serial.log" 2>/dev/null | grep -a -- "$2" || true; }

check_boot() {
  local name="$1" label="$2"
  if serial "$name" '^SBTEST-BEGIN' | grep -q .; then ok "$label: installed system booted and ran the check"; else bad "$label: installed system never ran the check"; fi
  serial "$name" '^secure_boot=\|^lockdown=\|^kernel_rt=\|^removable=\|^nvram_entry=\|^SBTEST-RC' | sed 's/^/SB-TEST info    /'
  if serial "$name" '^secure_boot=on$' | grep -q .; then ok "$label: secure_boot=on"; else bad "$label: secure_boot is not on"; fi
  if serial "$name" '^lockdown=integrity$' | grep -q .; then ok "$label: lockdown=integrity"; else bad "$label: lockdown is not integrity"; fi
  if serial "$name" '^kernel_rt=yes$' | grep -q .; then ok "$label: kernel_rt=yes"; else bad "$label: kernel is not RT"; fi
  if serial "$name" '^SBTEST-RC 0$' | grep -q .; then ok "$label: --require-on exited 0"; else bad "$label: --require-on did not exit 0"; fi
}

# Plain OVMF for the install: -kernel is unsigned and secboot would refuse it.
qemu_run install "$INSTALL_TIMEOUT" 1 "$PLAIN_CODE" "$PLAIN_VARS"
RC="$(serial install '^SBTEST-INSTALL-RC' | tail -n 1 | awk '{print $2}')"
if [[ "$RC" == 0 ]]; then ok "sonora-install exited 0 in the guest"; else bad "sonora-install exit code '${RC:-none}'"; fi

qemu_run secboot "$BOOT_TIMEOUT" 0 "$SB_CODE" "$SB_VARS"
check_boot secboot "enrolled NVRAM"

# The same VARS template again, copied fresh by qemu_run: nothing run 2 wrote
# to the NVRAM survives.
qemu_run cleared "$BOOT_TIMEOUT" 0 "$SB_CODE" "$SB_VARS"
check_boot cleared "cleared NVRAM (removable path)"
if serial cleared '^nvram_entry=no$' | grep -q .; then ok "cleared NVRAM: no boot entry, removable path booted"; else bad "cleared NVRAM: a boot entry exists or is unreadable"; fi

echo
if [[ "$FAILED" == 0 ]]; then echo "SB-TEST RESULT pass"; else
  echo "SB-TEST RESULT fail; logs in $WORK"
  if [[ "$SLOW" -gt 1 && "$CLOCK_STOPPED" == 1 ]]; then echo "SB-TEST INCONCLUSIVE: slow mode x$SLOW ran out of clock" >&2; exit "$QEMU_EXIT_INCONCLUSIVE"; fi
  exit 1
fi
