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

# A verdict about yesterday's ISO is worse than no verdict. This project has
# already shipped one script that reported success for a failed build.
for newer in "$ROOT/iso/config" "$ROOT/iso/auto/config" "$ROOT/engine"; do
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
[[ -n "$QEMU" ]] || { echo "error: qemu-system-x86_64 not found." >&2; exit 1; }
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
if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
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
"$QEMU" \
  -m "$MEMORY" \
  -smp 2 \
  "${ACCEL[@]}" \
  -kernel "$(host_path "$WORK")/vmlinuz" \
  -initrd "$(host_path "$WORK")/initrd.img" \
  -append "boot=live components quiet splash persistence username=sonora hostname=sonora threadirqs console=ttyS0,115200 sonora.smoketest" \
  -cdrom "$(host_path "$ROOT/iso")/sonora.iso" \
  -device intel-hda -device hda-duplex \
  -serial "file:$(host_path "$WORK")/serial.log" \
  -display none \
  -monitor none \
  -no-reboot \
  < /dev/null > "$WORK/qemu.log" 2>&1 &
QEMU_PID=$!

# --- wait for a verdict, or for the clock to run out ---
waited=0
verdict=""
while [[ "$waited" -lt "$TIMEOUT" ]]; do
  if ! kill -0 "$QEMU_PID" 2>/dev/null; then break; fi
  if grep -q "SMOKETEST RESULT" "$LOG" 2>/dev/null; then break; fi
  sleep 2
  waited=$((waited + 2))
done

if kill -0 "$QEMU_PID" 2>/dev/null; then
  kill "$QEMU_PID" 2>/dev/null || true
  sleep 1
  kill -9 "$QEMU_PID" 2>/dev/null || true
fi

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
