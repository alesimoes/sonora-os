#!/usr/bin/env bash
# Run the engine against a real audio server, in a container, with no hardware
# and no booting. This is the fast loop: seconds, not minutes.
#
# What this proves: the engine connects to PipeWire through the JACK API,
# registers and connects its ports, captures a known signal, and writes
# well-formed BWF files of the right length with the timecode origin set, with
# no xruns and no overruns. Also the capture device policy, with a fake
# physical device standing in for a laptop's onboard audio: left alone when
# the inputs are patched by hand within the wait; recorded from after the
# wait when it is the only device, with the notice; refused, both listed,
# beside a second non-USB device; and, killed mid-take, DeviceGone closes the
# take and names it. Before PipeWire starts, `sonora-engine --backend fake`
# records 18 tracks with no server, checked by the same verifier. Last, the
# backend conformance suite (ADR-0007 §7) runs against JackBackend on this
# PipeWire, the suite ctest runs against drivers/fake, with the quantum,
# rate, stall, unplug and server-loss faults caused from outside.
#
# Defaults to 18 channels, matching the Behringer UMC1820 this is built for.
# The count matters: verify-recording.py checks that track N carries tone N,
# so a lexicographic port sort that crosses channels fails here. A stereo test
# could not distinguish a crossed pair from a correct one.
#
# The quantum is not a parameter. It comes from audio-stack/pipewire-sonora.conf,
# copied into the image and into this container, because the point is to test
# the configuration we ship rather than one chosen by the test.
#
# What this cannot prove: anything about the RT kernel, the bootloader, real
# interface latency, or systemd bringing the session up. A container shares the
# host kernel -- `uname -r` here is the host's. Use QEMU for those.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=sonora-runtime:trixie

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

command -v docker >/dev/null || { echo "error: docker is required." >&2; exit 1; }
docker info >/dev/null 2>&1 || {
  echo "error: the Docker daemon is not reachable. Start Docker Desktop." >&2
  exit 1
}

echo "=== building the engine ==="
"$ROOT/tools/build-engine.sh" >/dev/null
ENGINE=""
for candidate in "$ROOT/build/sonora-engine" "$ROOT/build-container/sonora-engine"; do
  [[ -f "$candidate" ]] && ENGINE="$candidate"
done
[[ -n "$ENGINE" ]] || { echo "error: no engine binary to test." >&2; exit 1; }

echo "=== building the runtime image ==="
# Run by image ID, not by tag: the tag is shared by every worktree on this
# Docker host, and a rebuild from another checkout in between would put its
# audio-stack/ under our name.
IMAGE_ID="$(docker build -q -t "$IMAGE" --label org.sonora=1 \
  -f "$(host_path "$ROOT/tools")/Dockerfile.runtime" \
  "$(host_path "$ROOT")")"

echo "=== running the engine against PipeWire ==="
# The binary is mounted rather than baked in, so a rebuild does not rebuild the
# image. --mode 0755 cannot be expressed on a bind mount, hence the copy in the
# container before running it.
# Unlimited memlock as sonora-session.service grants it: the engine refuses
# to start without it.
docker run --rm --ulimit memlock=-1:-1   -e RECORD_SECONDS="${RECORD_SECONDS:-3}"   -e TRACKS="${TRACKS:-18}"   -e RATE="${RATE:-48000}"   -e QUANTUM="${QUANTUM:-128}"   -v "$(host_path "$(dirname "$ENGINE")"):/engine-build:ro"   "$IMAGE_ID"
