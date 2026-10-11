#!/usr/bin/env bash
# Run the engine against a real audio server, in a container, with no hardware
# and no booting. This is the fast loop: seconds, not minutes.
#
# What this proves: the engine connects to PipeWire through the JACK API,
# registers and connects its ports, captures a known signal, and writes
# well-formed BWF files of the right length with the timecode origin set, with
# no xruns and no overruns.
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
docker build -q -t "$IMAGE" \
  -f "$(host_path "$ROOT/tools")/Dockerfile.runtime" \
  "$(host_path "$ROOT")" >/dev/null

echo "=== running the engine against PipeWire ==="
# The binary is mounted rather than baked in, so a rebuild does not rebuild the
# image. --mode 0755 cannot be expressed on a bind mount, hence the copy in the
# container before running it.
docker run --rm   -e RECORD_SECONDS="${RECORD_SECONDS:-3}"   -e TRACKS="${TRACKS:-18}"   -e RATE="${RATE:-48000}"   -e QUANTUM="${QUANTUM:-128}"   -v "$(host_path "$(dirname "$ENGINE")"):/engine-build:ro"   "$IMAGE"
