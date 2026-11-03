#!/usr/bin/env bash
# Regenerates plugins/native/ampcab/tests/data/: the test networks and input
# (make_models.py) and the NAM reference implementation's output on each
# (oracle.cpp), which test_nam.cpp holds our kernel to (ADR-0011 §3.2).
#
# The reference is a test oracle only (ADR-0011 §4.3): downloaded here at a
# pinned commit and checked by SHA-256, built in a scratch directory, never
# vendored or linked into a Sonora target. Eigen (MPL-2.0) comes from
# Debian's libeigen3-dev the same way.
#
# Runs in the build image, from the repository root:
#   docker run --rm -v "$PWD:/src" -w /src sonora-build:trixie \
#       plugins/native/ampcab/tests/oracle/generate.sh [--bench [SECONDS]]
# A changed output is a deliberate commit that says why (ADR-0011 §3.4).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA="$HERE/../data"
NAM_COMMIT=0b3d3c97b0859a3a8c92a8628c4dd89a25eb5842  # NeuralAmpModelerCore 0.6.0, 2026-09
NAM_SHA256=ca8bd36a66c0861c4359660440210455f436b48ba28529c7134a09a08eaaf616
BLOCK=64

if ! command -v curl >/dev/null || [[ ! -d /usr/include/eigen3 ]]; then
  apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
    ca-certificates curl libeigen3-dev >/dev/null
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
curl -sSfL -o "$WORK/nam.tar.gz" \
  "https://codeload.github.com/sdatkinson/NeuralAmpModelerCore/tar.gz/$NAM_COMMIT"
echo "$NAM_SHA256  $WORK/nam.tar.gz" | sha256sum -c --quiet
tar -xzf "$WORK/nam.tar.gz" -C "$WORK"
NAM="$(echo "$WORK"/NeuralAmpModelerCore-*)"

# The reference's own sources, without the A2 fast path (not defined, so
# the plain Eigen path every .nam of format 0.5 takes).
mapfile -t SRC < <(ls "$NAM"/NAM/*.cpp "$NAM"/NAM/wavenet/*.cpp)
g++ -std=c++20 -O2 -DNDEBUG -I "$NAM" -I /usr/include/eigen3 -I "$NAM/Dependencies/nlohmann" \
  -o "$WORK/oracle" "$HERE/oracle.cpp" "${SRC[@]}"

# --bench: time the reference at the trainer's four sizes instead, for the
# comparison beside ampcab-budget (ADR-0011 Unverified 9). Writes nothing.
if [[ "${1:-}" == --bench ]]; then
  python3 -I "$HERE/make_models.py" --presets "$WORK/presets"
  for name in nano feather lite standard; do
    "$WORK/oracle" --bench "$WORK/presets/$name.nam" "${2:-20}"
  done
  exit 0
fi

python3 -I "$HERE/make_models.py" "$DATA"
for model in "$DATA"/*.nam; do
  name="$(basename "$model" .nam)"
  "$WORK/oracle" "$model" "$DATA/input.f32" "$BLOCK" "$DATA/$name.ref.f32"
  echo "oracle: $name"
done
