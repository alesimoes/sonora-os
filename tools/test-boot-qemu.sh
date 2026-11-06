#!/usr/bin/env bash
# Boot the image in QEMU and assert what a container cannot tell us.
#
# This is the only test that can see systemd and the running kernel. The
# container recording test is faster and covers the engine, but it shares the
# host kernel, so the defect where the image installed the RT kernel and
# booted the generic one would pass it. That is the gap this closes.
#
# It does NOT execute the bootloader. We boot the kernel out of the ISO
# directly with -kernel/-initrd, because the smoke test needs a parameter the
# shipped command line does not carry and editing a built ISO's bootloader is
# worse than not testing it. So a broken menu entry would not be caught here.
# What we do instead is assert statically, below, that both bootloader
# configs point at an RT kernel -- cheap, and it covers the regression that
# actually happened.
set -euo pipefail
# One heavy job at a time on this machine (tools/build-slot.sh).
[[ -n "${SONORA_HEAVY_HELD:-}" ]] || exec "$(dirname "${BASH_SOURCE[0]}")/build-slot.sh" "${BASH_SOURCE[0]}" "$@"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="$ROOT/iso/sonora.iso"
WORK="$ROOT/build-qemu"
LOG="$WORK/serial.log"
MEMORY="${QEMU_MEMORY:-4096}"
TIMEOUT="${QEMU_TIMEOUT:-300}"

host_path() {
  if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
    (cd "$1" && pwd -W)
  else
    printf '%s' "$1"
  fi
}

[[ -f "$ISO" ]] || {
  echo "error: $ISO not found. Run tools/build-iso.sh first." >&2
  exit 1
}

# Modes (ADR-0010 stage 1), each one boot:
#   (none)        the shipped boot: session on tty1, display stack skipped
#   --gui         sonora.gui: labwc and the UI run; the smoke test times the
#                 first frame and SIGKILLs the UI, then labwc, expecting both
#                 back within 3 s
#   --gui-broken  sonora.gui sonora.break_ui: the UI is made to fail
#                 repeatedly; the fallback must end on tty1 with the session
case "${1:-}" in
  "") ;;
  --gui) QEMU_APPEND_EXTRA="sonora.gui${QEMU_APPEND_EXTRA:+ $QEMU_APPEND_EXTRA}" ;;
  --gui-broken) QEMU_APPEND_EXTRA="sonora.gui sonora.break_ui${QEMU_APPEND_EXTRA:+ $QEMU_APPEND_EXTRA}" ;;
  *) echo "usage: $0 [--gui | --gui-broken]" >&2; exit 2 ;;
esac

# The compressed size is a recorded number (ADR-0010 unverified item 7). A
# ceiling is enforced only when SONORA_ISO_MAX_MB is set, since the ADR names
# none.
ISO_BYTES="$(wc -c <"$ISO" | tr -d '[:space:]')"
ISO_MB=$(( ISO_BYTES / 1048576 ))
echo "iso size: ${ISO_MB} MiB (${ISO_BYTES} bytes)"
if [[ -n "${SONORA_ISO_MAX_MB:-}" && "$ISO_MB" -gt "$SONORA_ISO_MAX_MB" ]]; then
  echo "BOOT TEST FAIL: the ISO is ${ISO_MB} MiB, over SONORA_ISO_MAX_MB=${SONORA_ISO_MAX_MB}." >&2
  exit 1
fi

# A verdict about yesterday's ISO is worse than no verdict. This project has
# already shipped one script that reported success for a failed build.
for newer in "$ROOT/iso/config" "$ROOT/iso/auto/config" "$ROOT/CMakeLists.txt" \
             "$ROOT/common" "$ROOT/contracts" "$ROOT/engine" "$ROOT/drivers" "$ROOT/ui"; do
  if [[ "$newer" -nt "$ISO" ]]; then
    echo "error: $newer is newer than $ISO." >&2
    echo "       The test would report on a stale image. Rebuild first:" >&2
    echo "       tools/build-iso.sh" >&2
    exit 1
  fi
done

# --- locate QEMU ---
QEMU=""
for candidate in \
  qemu-system-x86_64 \
  "/c/Program Files/qemu/qemu-system-x86_64.exe" \
  "/usr/bin/qemu-system-x86_64"; do
  if command -v "$candidate" >/dev/null 2>&1; then QEMU="$candidate"; break; fi
  if [[ -x "$candidate" ]]; then QEMU="$candidate"; break; fi
done
# No QEMU on the host: run it in a container. On Windows the Docker Desktop VM
# usually has /dev/kvm even when the host has no QEMU at all, so this is the
# faster path there, not a degraded one.
IN_CONTAINER=0
QEMU_IMAGE=sonora-qemu:trixie
if [[ -z "$QEMU" ]]; then
  command -v docker >/dev/null || {
    echo "error: qemu-system-x86_64 not found, and no docker to run it in." >&2
    exit 1
  }
  IN_CONTAINER=1
  QEMU="docker:$QEMU_IMAGE"
fi
echo "qemu: $QEMU"

mkdir -p "$WORK"
rm -f "$LOG"

# --- pull the kernel and initrd out of the ISO ---
# The smoke test needs an extra kernel parameter, and editing a bootloader
# config inside a built ISO is worse than booting the same kernel directly.
# live-boot still finds its squashfs on the attached CD.
if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
  export MSYS2_ARG_CONV_EXCL='*'
fi
if [[ "$IN_CONTAINER" == 1 ]]; then
  docker build -q -t "$QEMU_IMAGE" --label org.sonora=1 \
    -f "$(host_path "$ROOT/tools")/Dockerfile.qemu" "$(host_path "$ROOT/tools")" >/dev/null
fi
# --- what the bootloaders would actually load ---
# isolinux loads /live/vmlinuz; grub loads the versioned name. Both must be
# the RT kernel. This is the static half of the bootloader coverage the
# -kernel boot below cannot provide.
echo "checking what the bootloaders point at..."
docker run --rm -v "$(host_path "$ROOT/iso"):/iso:ro"   sonora-livebuild:trixie bash -lc '
    set -e
    cd /tmp
    xorriso -osirrox on -indev /iso/sonora.iso       -extract /isolinux/live.cfg /tmp/live.cfg       -extract /boot/grub/grub.cfg /tmp/grub.cfg >/dev/null 2>&1

    ISO_KERNEL=$(grep -m1 -E "^[[:space:]]*linux" /tmp/live.cfg | awk "{print \$2}")
    GRUB_KERNEL=$(grep -m1 -E "^[[:space:]]*linux" /tmp/grub.cfg | awk "{print \$2}")
    echo "isolinux default: $ISO_KERNEL"
    echo "grub default:     $GRUB_KERNEL"

    case "$GRUB_KERNEL" in
      *-rt-*) ;;
      *) echo "E: the grub default entry is not an RT kernel." >&2; exit 1 ;;
    esac

    # isolinux uses the unversioned /live/vmlinuz, so compare its bytes with
    # the RT kernel rather than trusting the name.
    RT=$(xorriso -indev /iso/sonora.iso -find /live 2>/dev/null       | tr -d "'"'"'" | grep -E "vmlinuz-.*-rt-" | head -1)
    xorriso -osirrox on -indev /iso/sonora.iso       -extract "$ISO_KERNEL" /tmp/k-default       -extract "$RT" /tmp/k-rt >/dev/null 2>&1
    if cmp -s /tmp/k-default /tmp/k-rt; then
      echo "isolinux default is byte-identical to the RT kernel."
    else
      echo "E: isolinux would boot a kernel that is not the RT one." >&2
      exit 1
    fi
  '

echo "extracting the kernel and initrd..."
docker run --rm \
  -v "$(host_path "$ROOT/iso"):/iso:ro" \
  -v "$(host_path "$WORK"):/out" \
  sonora-livebuild:trixie bash -lc '
    set -e
    KERNEL=$(xorriso -indev /iso/sonora.iso -find /live 2>/dev/null \
      | tr -d "'"'"'" | grep -E "vmlinuz-.*-rt-" | head -1)
    INITRD=$(xorriso -indev /iso/sonora.iso -find /live 2>/dev/null \
      | tr -d "'"'"'" | grep -E "initrd.img-.*-rt-" | head -1)
    if [ -z "$KERNEL" ] || [ -z "$INITRD" ]; then
      echo "E: no -rt- kernel in the ISO. That is the bug, not a build problem." >&2
      xorriso -indev /iso/sonora.iso -find /live 2>/dev/null >&2
      exit 1
    fi
    echo "using $KERNEL"
    xorriso -osirrox on -indev /iso/sonora.iso \
      -extract "$KERNEL" /out/vmlinuz \
      -extract "$INITRD" /out/initrd.img >/dev/null 2>&1
    chmod 0644 /out/vmlinuz /out/initrd.img
  '

# --- acceleration, if the host offers any ---
ACCEL=()
if [[ "$IN_CONTAINER" == 1 ]]; then
  # Decided inside the container, where /dev/kvm is the VM's, not the host's.
  ACCEL=(-accel kvm -accel tcg)
elif [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
  # Windows Hypervisor Platform. Falls back silently to TCG if unavailable,
  # which is slow but still correct.
  ACCEL=(-accel whpx -accel tcg)
elif [[ -w /dev/kvm ]]; then
  ACCEL=(-accel kvm)
else
  ACCEL=(-accel tcg)
fi

echo "booting (timeout ${TIMEOUT}s)..."
# -nographic puts the serial console on stdout, which is where the smoke test
# writes. intel-hda gives PipeWire a card to find, so the audio stack is
# exercised rather than skipped.
# Two notes on the invocation below, kept out of the command because a
# comment inside a backslash continuation turns the rest of the logical
# line into a comment and silently drops every argument after it. That
# happened here: -append and -serial were swallowed, QEMU opened a window
# and the test waited 300s for output that could never arrive.
#
# -append mirrors the shipped --bootappend-live from iso/auto/config, plus
# the two parameters that exist only for the test. persistence is kept
# deliberately: it decides whether /home/sonora is the squashfs copy or a
# volume on the medium, the session directory lives there, and that is a
# class of boot defect this test exists to catch.
#
# -serial to a file rather than -nographic: -nographic multiplexes the
# serial line and the QEMU monitor onto stdio, and with stdin on the
# invoking terminal QEMU puts it into raw mode. Ctrl-C stops working and a
# kill -9 on timeout leaves the shell with no echo.
#
# In the container the same files are reached through /work and /iso, and the
# container is named so a timeout can remove it: killing the docker client
# alone would leave QEMU running in the VM.
CONTAINER=""
if [[ "$IN_CONTAINER" == 1 ]]; then
  CONTAINER="sonora-qemu-boot-$$"
  W=/work
  I=/iso
  KVM_DEV=()
  if docker run --rm --privileged "$QEMU_IMAGE" test -c /dev/kvm; then
    KVM_DEV=(--device /dev/kvm)
    echo "accel: kvm (in the Docker VM)"
  else
    echo "accel: tcg (no /dev/kvm in the Docker VM; expect a slow boot)"
  fi
  RUNNER=(docker run --rm --name "$CONTAINER" --label org.sonora=1 "${KVM_DEV[@]}"
          -v "$(host_path "$WORK"):/work" -v "$(host_path "$ROOT/iso"):/iso:ro"
          "$QEMU_IMAGE" qemu-system-x86_64)
else
  W="$(host_path "$WORK")"
  I="$(host_path "$ROOT/iso")"
  RUNNER=("$QEMU")
fi
"${RUNNER[@]}" \
  -m "$MEMORY" \
  -smp 2 \
  "${ACCEL[@]}" \
  -kernel "$W/vmlinuz" \
  -initrd "$W/initrd.img" \
  -append "boot=live components quiet splash persistence username=sonora hostname=sonora threadirqs console=ttyS0,115200 sonora.smoketest${QEMU_APPEND_EXTRA:+ $QEMU_APPEND_EXTRA}" \
  -cdrom "$I/sonora.iso" \
  -device intel-hda -device hda-duplex \
  -serial "file:$W/serial.log" \
  -display none \
  -monitor none \
  -no-reboot \
  < /dev/null > "$WORK/qemu.log" 2>&1 &
QEMU_PID=$!
started=$SECONDS

# --- wait for a verdict, or for the clock to run out ---
waited=0
verdict=""
while [[ "$waited" -lt "$TIMEOUT" ]]; do
  if ! kill -0 "$QEMU_PID" 2>/dev/null; then break; fi
  if grep -q "SMOKETEST RESULT" "$LOG" 2>/dev/null; then break; fi
  sleep 2
  waited=$((waited + 2))
done

# The verdict is printed before the unit's SuccessAction=/FailureAction= powers
# the guest off. Killing QEMU at once left that path untested, and boot 3 of
# tools/test-write-usb.sh depends on the orderly poweroff to get a clean /home.
# On a pass the guest must also power itself off, within POWEROFF_TIMEOUT.
POWEROFF_TIMEOUT="${QEMU_POWEROFF_TIMEOUT:-60}"
powered_off=""
if grep -q "SMOKETEST RESULT" "$LOG" 2>/dev/null; then
  verdict_at=$SECONDS
  while kill -0 "$QEMU_PID" 2>/dev/null && (( SECONDS - verdict_at < POWEROFF_TIMEOUT )); do
    sleep 1
  done
  if kill -0 "$QEMU_PID" 2>/dev/null; then
    powered_off=no
  else
    powered_off=yes
  fi
  poweroff_seconds=$((SECONDS - verdict_at))
fi

if kill -0 "$QEMU_PID" 2>/dev/null; then
  kill "$QEMU_PID" 2>/dev/null || true
  sleep 1
  kill -9 "$QEMU_PID" 2>/dev/null || true
fi
[[ -z "$CONTAINER" ]] || docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
echo "boot took $((SECONDS - started))s"

echo
echo "=== smoke test output ==="
grep "SMOKETEST" "$LOG" 2>/dev/null || {
  echo "no smoke test output at all." >&2
  echo "--- last 40 lines of the serial log ---" >&2
  tail -40 "$LOG" >&2
  exit 1
}

verdict=$(grep "SMOKETEST RESULT" "$LOG" 2>/dev/null | tail -1 || true)
echo
case "$powered_off" in
  yes) echo "guest powered itself off ${poweroff_seconds}s after the verdict" ;;
  no)  echo "guest still running ${poweroff_seconds}s after the verdict; killed" ;;
esac
if [[ "$verdict" == *pass* && "$powered_off" == no ]]; then
  echo "BOOT TEST FAIL: the smoke test passed but the guest did not power off" >&2
  echo "within ${POWEROFF_TIMEOUT}s (sonora-smoketest.service SuccessAction=)." >&2
  echo "Full log: $LOG" >&2
  exit 1
fi
if [[ "$verdict" == *pass* ]]; then
  echo "BOOT TEST PASS"
  exit 0
fi
if [[ -z "$verdict" ]]; then
  echo "BOOT TEST INCONCLUSIVE: no verdict after ${waited}s." >&2
  echo "Full log: $LOG" >&2
  exit 1
fi
echo "BOOT TEST FAIL" >&2
echo "Full log: $LOG" >&2
exit 1
