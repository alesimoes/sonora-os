#!/usr/bin/env bash
# Installed-layout check under QEMU (ADR-0090): swap partition and data
# partition really come up on the installed system, and the engine does not
# start when the data partition is gone.
#
# It reuses tools/test-install-qemu.sh unchanged: that script installs the
# stick onto $SONORA_QEMU_DIR/install/target.qcow2 and boots it once, and
# leaves the disk and the stick's kernel and initrd behind. This script then
# boots that disk once more, with the stick's kernel (-kernel/-initrd, root
# found by label, since the installed GRUB command line is fixed by run 1)
# and a one-shot script on the command line that reports on the serial port:
#   swapon --show        names the swap partition
#   findmnt Sessions     is the data partition, ext4
#   wipefs of the data partition, then `systemctl start sonora-engine`:
#                        the unit must fail (RequiresMountsFor, ADR-0090)
#
# Exit 77 = skipped (no QEMU or Docker), 3 = TCG clock ran out, 1 = fail.
#   LAYOUT_TIMEOUT=300   seconds at KVM speed (scaled by the slow factor)
set -euo pipefail
[[ -n "${SONORA_HEAVY_HELD:-}" ]] || exec "$(dirname "${BASH_SOURCE[0]}")/build-slot.sh" "${BASH_SOURCE[0]}" "$@"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/lib/artifacts.sh
# shellcheck disable=SC1091
. "$ROOT/tools/lib/artifacts.sh"
# shellcheck source=tools/lib/qemu-accel.sh
# shellcheck disable=SC1091
. "$ROOT/tools/lib/qemu-accel.sh"
WORK="$SONORA_QEMU_DIR/install"
MEMORY="${QEMU_MEMORY:-4096}"
LAYOUT_TIMEOUT="${LAYOUT_TIMEOUT:-300}"
FAILED=0
ok() { echo "LAYOUT-TEST ok    $*"; }
bad() { echo "LAYOUT-TEST FAIL  $*"; FAILED=1; }

# 1. The install itself: the existing harness, run as it is. Its exit code
# (skip, inconclusive, fail) is ours.
rc=0
"$ROOT/tools/test-install-qemu.sh" || rc=$?
if (( rc != 0 )); then echo "LAYOUT-TEST: the install run exited $rc; nothing to inspect." >&2; exit "$rc"; fi
for f in target.qcow2 vmlinuz initrd.img; do
  [[ -s "$WORK/$f" ]] || { echo "error: $WORK/$f missing after the install run." >&2; exit 1; }
done

host_path() {
  if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then (cd "$1" && pwd -W); else printf '%s' "$1"; fi
}
[[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]] && export MSYS2_ARG_CONV_EXCL='*'

QEMU=""
for candidate in qemu-system-x86_64 "/c/Program Files/qemu/qemu-system-x86_64.exe" /usr/bin/qemu-system-x86_64; do
  if command -v "$candidate" >/dev/null 2>&1 || [[ -x "$candidate" ]]; then QEMU="$candidate"; break; fi
done
IN_CONTAINER=0; QEMU_IMAGE=sonora-qemu:trixie; CONTAINER=""; KVM_DEV=()
if [[ -z "$QEMU" ]]; then
  IN_CONTAINER=1   # the install run built the image; it is cached
  KVM_OK=0
  if docker run --rm --privileged --label org.sonora=1 "$QEMU_IMAGE" test -c /dev/kvm 2>/dev/null; then KVM_DEV=(--device /dev/kvm); KVM_OK=1; fi
  qemu_pick_accel container "$KVM_OK"
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then qemu_pick_accel msys 0
else qemu_pick_accel host 0; fi
LAYOUT_TIMEOUT="$(qemu_scale "$LAYOUT_TIMEOUT")"
SLOW_APPEND="$(qemu_append_slow)"
trap '[[ -z "$CONTAINER" ]] || docker rm -f "$CONTAINER" >/dev/null 2>&1 || true' EXIT

# The guest script. Variables expand in the guest. Under
# kernel-command-line.target nothing of fstab is started, so it starts the
# local filesystems and swap itself, exactly what a normal boot would do.
# shellcheck disable=SC2016
GUEST_SCRIPT='say() { echo "LAYOUTTEST $*" > /dev/ttyS0; }
systemctl start local-fs.target swap.target 2>/dev/null
say SWAPON $(swapon --noheadings --show=NAME,TYPE | tr -s " " | tr "\n" ";")
SRC=$(findmnt -no SOURCE /home/sonora/Sessions)
say SESSIONS $(findmnt -no SOURCE,FSTYPE /home/sonora/Sessions | tr -s " ")
say DATALABEL $(blkid -s LABEL -o value "$SRC")
say FSTAB $(grep -c "^UUID=" /etc/fstab)
systemctl stop home-sonora-Sessions.mount
wipefs -a "$SRC" > /dev/null
if systemctl start sonora-engine.service 2>/dev/null; then say ENGINE started; else say ENGINE refused; fi
say ENGINESTATE $(systemctl is-active sonora-engine.service)'
B64="$(printf '%s\n' "$GUEST_SCRIPT" | base64 | tr -d '\n')"
RUN="systemd.unit=kernel-command-line.target systemd.run_success_action=poweroff systemd.run_failure_action=poweroff"
APPEND="root=LABEL=sonora-root rootwait ro console=ttyS0,115200${SLOW_APPEND:+ $SLOW_APPEND} $RUN systemd.run=\"/bin/sh -c 'echo $B64 | base64 -d | sh'\""
if (( ${#APPEND} > 1900 )); then echo "error: kernel command line is ${#APPEND} bytes; limit 2048." >&2; exit 1; fi

# The wipe must not touch the disk the install run left: boot a copy.
cp "$WORK/target.qcow2" "$WORK/layout.qcow2"
rm -f "$WORK/layout.serial.log"
if [[ "$IN_CONTAINER" == 1 ]]; then
  w=/w; CONTAINER="sonora-layout-qemu-$$"
  runner=(docker run --rm --name "$CONTAINER" --label org.sonora=1 "${KVM_DEV[@]}" -v "$(host_path "$WORK"):/w" "$QEMU_IMAGE" qemu-system-x86_64)
else
  w="$(host_path "$WORK")"; runner=("$QEMU")
fi
args=(-machine q35 -m "$MEMORY" -smp 2 "${ACCEL[@]}"
      -drive "if=none,id=target,format=qcow2,file=$w/layout.qcow2" -device "virtio-blk-pci,drive=target"
      -kernel "$w/vmlinuz" -initrd "$w/initrd.img" -append "$APPEND"
      -serial "file:$w/layout.serial.log" -display none -monitor none -no-reboot)
echo "--- layout boot (up to ${LAYOUT_TIMEOUT}s, accel $ACCEL_NAME) ---"
"${runner[@]}" "${args[@]}" < /dev/null > "$WORK/layout.qemu.log" 2>&1 &
pid=$!
waited=0; CLOCK_STOPPED=0
while (( waited < LAYOUT_TIMEOUT )) && kill -0 "$pid" 2>/dev/null; do sleep 2; waited=$((waited + 2)); done
if kill -0 "$pid" 2>/dev/null; then
  CLOCK_STOPPED=1; kill "$pid" 2>/dev/null || true; sleep 2; kill -9 "$pid" 2>/dev/null || true
  [[ -z "$CONTAINER" ]] || docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
fi
wait "$pid" 2>/dev/null || true
CONTAINER=""
serial() { tr -d '\r' < "$WORK/layout.serial.log" 2>/dev/null | grep -a "^LAYOUTTEST $1" || true; }
serial '' | sed 's/^/LAYOUT-TEST info    /'

# check KEY REGEX OKMSG BADMSG: the guest's KEY line must match REGEX.
check() {
  if serial "$1" | grep -Eq "$2"; then ok "$3"; else bad "$4"; fi
}
check SWAPON ' /dev/vda[0-9]+ partition' "swapon --show lists a swap partition" "swapon --show shows no swap partition"
check SESSIONS ' /dev/vda[0-9]+ ext4' "Sessions is an ext4 partition of the disk" "Sessions is not on a disk partition"
check DATALABEL ' sonora-data$' "Sessions is the sonora-data partition" "Sessions is not sonora-data"
check FSTAB ' 4$' "fstab names four UUIDs" "fstab does not name four UUIDs"
check ENGINE ' refused$' "engine refuses to start with the data partition wiped" "engine started without its data partition"
check ENGINESTATE ' (inactive|failed)$' "engine is not active" "engine is active without its data partition"

echo
if [[ "$FAILED" == 0 ]]; then echo "LAYOUT-TEST RESULT pass"; exit 0; fi
echo "LAYOUT-TEST RESULT fail; logs in $WORK"
if [[ "$SLOW" -gt 1 && "$CLOCK_STOPPED" == 1 ]]; then echo "LAYOUT-TEST INCONCLUSIVE: slow mode x$SLOW ran out of clock" >&2; exit "$QEMU_EXIT_INCONCLUSIVE"; fi
exit 1
