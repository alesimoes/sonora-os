#!/usr/bin/env bash
# Boot the image in a QEMU window so a human can watch it and use it.
#
# Different purpose from tools/test-boot-qemu.sh: that one boots headless,
# asserts, and powers off. This one opens a window, boots the image exactly
# as a real machine would -- through the bootloader, with the shipped kernel
# command line -- and leaves you looking at the engine.
#
# What you should see: a few seconds of boot, then the engine's track list
# with meters, on tty1. No login prompt, no desktop.
#
# Close the window to shut it down. Ctrl-Alt-G releases the mouse if QEMU
# grabs it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ISO="$ROOT/iso/sonora.iso"
MEMORY="${QEMU_MEMORY:-4096}"
CPUS="${QEMU_CPUS:-2}"

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

QEMU=""
for candidate in \
  qemu-system-x86_64 \
  "/c/Program Files/qemu/qemu-system-x86_64.exe" \
  "/usr/bin/qemu-system-x86_64"; do
  if command -v "$candidate" >/dev/null 2>&1; then QEMU="$candidate"; break; fi
  if [[ -x "$candidate" ]]; then QEMU="$candidate"; break; fi
done
[[ -n "$QEMU" ]] || { echo "error: qemu-system-x86_64 not found." >&2; exit 1; }

if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
  export MSYS2_ARG_CONV_EXCL='*'
  ACCEL=(-accel whpx -accel tcg)
elif [[ -w /dev/kvm ]]; then
  ACCEL=(-accel kvm)
else
  ACCEL=(-accel tcg)
fi

MODE="${1:-bios}"
FIRMWARE=()
if [[ "$MODE" == "uefi" ]]; then
  # The two boot paths are built from different configs in the ISO, so a
  # machine that boots one can fail on the other. Worth being able to try both.
  OVMF=""
  for f in \
    "/c/Program Files/qemu/share/edk2-x86_64-code.fd" \
    "/usr/share/OVMF/OVMF_CODE.fd" \
    "/usr/share/ovmf/OVMF.fd"; do
    [[ -f "$f" ]] && OVMF="$f" && break
  done
  if [[ -z "$OVMF" ]]; then
    echo "error: no OVMF firmware found; cannot test the UEFI path." >&2
    exit 1
  fi
  FIRMWARE=(-bios "$(host_path "$(dirname "$OVMF")")/$(basename "$OVMF")")
  echo "firmware: UEFI ($OVMF)"
else
  echo "firmware: legacy BIOS (pass 'uefi' as the first argument for the other path)"
fi

cat <<'NOTE'

  Booting SonoraOS. Expect:
    - a boot menu, then about 10 seconds of kernel messages
    - the engine's track list with level meters, on tty1
    - no login prompt and no desktop: the engine IS the session

  Keys inside the engine:
    space   start and stop recording
    1-8     arm or unarm a track
    a       arm every track
    q       quit (systemd restarts it, by design)

  Sessions are written to /home/sonora/Sessions inside the VM. Nothing is
  written to your machine.

NOTE

# -cdrom with no -kernel, so the real bootloader runs. intel-hda gives
# PipeWire a card to find, which is what makes the meters do anything.
exec "$QEMU" \
  -m "$MEMORY" \
  -smp "$CPUS" \
  "${ACCEL[@]}" \
  "${FIRMWARE[@]}" \
  -cdrom "$(host_path "$ROOT/iso")/sonora.iso" \
  -boot d \
  -device intel-hda -device hda-duplex \
  -vga std \
  -no-reboot
