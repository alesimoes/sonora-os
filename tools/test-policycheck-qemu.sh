#!/usr/bin/env bash
# Policy check of the running image in QEMU: the mounts, RT kernel, limits,
# sysctls, PipeWire clock, gettys and enabled units that the static tests
# (test-ro-root.sh, test-image-config.sh) could only claim.
#
# Same boot as tools/test-boot-qemu.sh with the kernel parameter
# sonora.policycheck, which makes sonora-smoketest source
# /usr/local/bin/sonora-policycheck. That script prints one
# "POLICY CHECK PASS|FAIL <item>" line per assertion; this one requires every
# item to be present and PASS. An image built before the check existed has no
# such lines, so it cannot pass.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG="$ROOT/build-qemu/serial.log"

# Keep in step with the pc lines in sonora-policycheck (test-image-config does
# not parse this; the loop below fails loudly on a missing item instead).
ITEMS=(
  "root is overlay over read-only squashfs"
  "persistence requested on the kernel command line"
  "/home is the persistence volume, or the overlay when none is attached"
  "operator can write the Sessions directory"
  "tmp.mount active, nosuid,nodev, size 512M"
  "journald volatile, runtime cap 64M"
  "running kernel is the RT one (uname -r)"
  "running kernel is PREEMPT_RT (/sys/kernel/realtime)"
  "threadirqs on the kernel command line"
  "IRQ handlers run as threads (irq/ kernel threads exist)"
  "pipewire process up"
  "wireplumber process up"
  "pipewire answers the operator's session"
  "pipewire clock.quantum/rate equal audio-stack/pipewire-sonora.conf"
  "RT limits (rtprio, memlock, nice) equal audio-stack/limits-audio.conf"
  "installed limits file is the audio-stack one"
  "sysctls equal audio-stack/sysctl-sonora.conf"
  "no getty on any VT or serial line, no display manager"
  "enabled units are all on the allowlist"
)

# --static: no boot; every item above must be asserted by the script in the
# image, and every assertion there must be listed (a renamed pc line would
# otherwise only show up as a failure after a 20 minute ISO build).
if [[ "${1:-}" == "--static" ]]; then
  PC="$ROOT/iso/config/includes.chroot/usr/local/bin/sonora-policycheck"
  [[ -f "$PC" ]] || { echo "FAIL: missing $PC" >&2; exit 1; }
  fail=0
  for item in "${ITEMS[@]}"; do
    grep -qF "\"$item\"" "$PC" || { echo "FAIL: not asserted in sonora-policycheck: $item" >&2; fail=1; }
  done
  n="$(grep -c '^pc "' "$PC")"
  [[ "$n" == "${#ITEMS[@]}" ]] || { echo "FAIL: $n pc lines, ${#ITEMS[@]} items listed" >&2; fail=1; }
  grep -q 'sonora.policycheck) POLICYCHECK=1' "$ROOT/iso/config/includes.chroot/usr/local/bin/sonora-smoketest" ||
    { echo "FAIL: sonora-smoketest does not read sonora.policycheck" >&2; fail=1; }
  [[ "$fail" == 0 ]] && echo "ok: ${#ITEMS[@]} policy items match sonora-policycheck"
  exit "$fail"
fi

export QEMU_APPEND_EXTRA="sonora.policycheck"
rc=0
"$ROOT/tools/test-boot-qemu.sh" || rc=$?

echo
echo "=== policy check ==="
grep "POLICY CHECK" "$LOG" 2>/dev/null || true

bad=0
for item in "${ITEMS[@]}"; do
  if grep -qF "POLICY CHECK PASS $item" "$LOG" 2>/dev/null; then
    continue
  fi
  bad=1
  if grep -qF "POLICY CHECK FAIL $item" "$LOG" 2>/dev/null; then
    echo "POLICY CHECK TEST FAIL: \"$item\" failed on this image." >&2
  else
    echo "POLICY CHECK TEST FAIL: no line for \"$item\" in $LOG." >&2
    echo "The image probably predates sonora-policycheck; rebuild it." >&2
  fi
done
if grep -q "POLICY CHECK FAIL" "$LOG" 2>/dev/null; then bad=1; fi

if [[ "$bad" != 0 || "$rc" != 0 ]]; then
  echo "POLICY CHECK TEST FAIL (boot test exit $rc)" >&2
  exit 1
fi
echo "POLICY CHECK TEST PASS"
