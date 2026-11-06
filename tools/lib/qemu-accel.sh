#!/usr/bin/env bash
# Shared acceleration and timeout policy for the QEMU tests (sourced, not run).
#
# A host without /dev/kvm (Docker Desktop on Windows, many CI runners) boots
# the image under TCG, which is several times slower. The guest is the same;
# only the clocks change. So:
#   qemu_pick_accel MODE KVM_OK   sets ACCEL, SLOW, ACCEL_NAME and logs them
#   qemu_scale SECONDS            SECONDS * SLOW, for every host timeout
#   qemu_append_slow              sonora.slow=N for the guest (sonora-smoketest)
# Exit codes shared by the tests: 0 pass, 1 fail, 3 inconclusive (the slow-mode
# clock ran out with no verdict and no failure), 77 skip (no way to run QEMU
# here at all). A skip or an inconclusive run is never a pass.
#
#   SONORA_QEMU_SLOW=N     force the multiplier (default 1 KVM, 2 WHPX, 8 TCG)
#   SONORA_QEMU_ACCEL=tcg  force TCG even when KVM exists (to test this path)
# shellcheck disable=SC2034 # used by the sourcing scripts
QEMU_EXIT_SKIP=77
# shellcheck disable=SC2034
QEMU_EXIT_INCONCLUSIVE=3
# shellcheck disable=SC2034
ACCEL=()
SONORA_TCG_SLOW_DEFAULT=8

# MODE is container|msys|host; KVM_OK is 1 or 0 and only matters for container
# (the Docker VM's /dev/kvm, probed by the caller).
qemu_pick_accel() {
  local mode="$1" kvm_ok="$2" kind=tcg
  if [[ "${SONORA_QEMU_ACCEL:-}" != tcg ]]; then
    case "$mode" in
      container) if [[ "$kvm_ok" == 1 ]]; then kind=kvm; fi ;;
      msys) kind=whpx ;;
      *) if [[ -w /dev/kvm ]]; then kind=kvm; fi ;;
    esac
  fi
  case "$kind" in
    kvm)  ACCEL=(-accel kvm); SLOW=1 ;;
    # WHPX can fall back to TCG silently, so it keeps a TCG fallback and a
    # modest multiplier instead of 1.
    whpx) ACCEL=(-accel whpx -accel "tcg,thread=multi" -cpu max); SLOW=2 ;;
    *)    ACCEL=(-accel "tcg,thread=multi" -cpu max); SLOW="$SONORA_TCG_SLOW_DEFAULT" ;;
  esac
  ACCEL_NAME="$kind"
  if [[ -n "${SONORA_QEMU_SLOW:-}" ]]; then
    case "$SONORA_QEMU_SLOW" in
      ''|*[!0-9]*|0) echo "error: SONORA_QEMU_SLOW must be a positive integer" >&2; return 1 ;;
    esac
    SLOW="$SONORA_QEMU_SLOW"
  fi
  echo "accel: $ACCEL_NAME; slow-mode multiplier x$SLOW on every timeout"
}

qemu_scale() { echo $(( $1 * SLOW )); }

qemu_append_slow() { if [[ "$SLOW" -gt 1 ]]; then printf 'sonora.slow=%s' "$SLOW"; fi; }
