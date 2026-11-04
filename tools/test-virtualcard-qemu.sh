#!/usr/bin/env bash
# Virtual multichannel card in QEMU: snd-dummy model=rme9652 (26 capture
# channels) through ALSA -> PipeWire -> JACK -> sonora-engine, in the image's
# own RT kernel. Same boot as tools/test-boot-qemu.sh, with the kernel
# parameter sonora.virtualcard that makes sonora-smoketest run
# /usr/local/bin/sonora-virtualcard-test; the verdict is that script's.
#
# Needs an ISO built from a tree that contains sonora-virtualcard-test
# (tools/build-iso.sh); an older image has no such mode and the boot test would
# pass without testing anything, so this refuses it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG="$ROOT/build-qemu/serial.log"

export QEMU_APPEND_EXTRA="sonora.virtualcard sonora.expect_capture=18"
"$ROOT/tools/test-boot-qemu.sh"

# The boot test's pass could come from an image that ignores the parameter.
for needle in "sonora.virtualcard: snd-dummy" "ok    virtual card exposes" \
              "ok    engine recorded from the virtual card" "ok    18 track files written"; do
  if ! grep -q "SMOKETEST.*$needle" "$LOG"; then
    echo "VIRTUAL CARD TEST FAIL: no \"$needle\" in $LOG." >&2
    echo "The image probably predates sonora-virtualcard-test; rebuild it." >&2
    exit 1
  fi
done
echo "VIRTUAL CARD TEST PASS"
