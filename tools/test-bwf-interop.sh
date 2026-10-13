#!/usr/bin/env bash
# Do the files the engine writes open, elsewhere, at the right timecode?
#
# Phase 1's exit criterion says they do. This checks it with readers we did
# not write: ffprobe, soxi, sndfile-info (libsndfile, which Ardour reads
# through), mediainfo, and Ardour 8 itself, headless.
#
#   1. The engine records two takes in the runtime image (the audio stack we
#      ship), driven through its key loop on a pseudo-terminal. Take 2 starts
#      where take 1 stopped, so its TimeReference is non-zero and has a known
#      right answer.
#   2. A second image holding only Debian trixie's readers judges the files:
#      RIFF/WAVE, 24-bit, 48 kHz, length, bext TimeReference, Originator,
#      OriginationDate/Time, iXML if present, and the position Ardour gives
#      each file on its timeline.
#
# Output: verdict lines on stdout, everything else in build-interop/.
# Exit 0 only if every check passed; known defects print "xfail" and do not
# fail the run. See brain/research/bwf-interop.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build-interop"
RUNTIME_IMAGE=sonora-runtime:trixie
READER_IMAGE=sonora-bwf-readers:trixie
TRACKS="${TRACKS:-18}"
RATE="${RATE:-48000}"

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

# The quantum is pinned by the configuration we ship, and the timeline check
# allows one period of slack, so it is read from there rather than assumed.
QUANTUM=$(sed -n 's/^[[:space:]]*default\.clock\.quantum[[:space:]]*=[[:space:]]*\([0-9]*\).*/\1/p' \
  "$ROOT/audio-stack/pipewire-sonora.conf" | head -1)
[[ -n "$QUANTUM" ]] || { echo "error: no quantum in pipewire-sonora.conf." >&2; exit 1; }

rm -rf "$OUT/session" "$OUT/ardour"
mkdir -p "$OUT/ardour"
# The containers run as uid 1000; on a Linux host the directory may belong to
# someone else.
chmod 0777 "$OUT" "$OUT/ardour"

echo "=== building the engine ==="
"$ROOT/tools/build-engine.sh" >"$OUT/build-engine.log" 2>&1 || {
  echo "FAIL: engine build, see $OUT/build-engine.log" >&2
  exit 1
}
ENGINE=""
for candidate in "$ROOT/build/sonora-engine" "$ROOT/build-container/sonora-engine"; do
  [[ -f "$candidate" ]] && ENGINE="$candidate"
done
[[ -n "$ENGINE" ]] || { echo "FAIL: no engine binary." >&2; exit 1; }

echo "=== building the images ==="
docker build -q -t "$RUNTIME_IMAGE" --label org.sonora=1 \
  -f "$(host_path "$ROOT/tools")/Dockerfile.runtime" \
  "$(host_path "$ROOT")" >"$OUT/build-images.log" 2>&1
docker build -q -t "$READER_IMAGE" --label org.sonora=1 \
  -f "$(host_path "$ROOT/tools")/Dockerfile.bwfreaders" \
  "$(host_path "$ROOT/tools")" >>"$OUT/build-images.log" 2>&1

echo "=== recording two takes ==="
if ! docker run --rm \
    -e TRACKS="$TRACKS" -e RATE="$RATE" \
    -v "$(host_path "$(dirname "$ENGINE")"):/engine-build:ro" \
    -v "$(host_path "$ROOT/tools"):/interop-tools:ro" \
    -v "$(host_path "$OUT"):/out" \
    --entrypoint /bin/bash "$RUNTIME_IMAGE" \
    /interop-tools/test-bwf-interop-record.sh >"$OUT/record.log" 2>&1; then
  grep -E "FAIL|patched|exited" "$OUT/record.log" >&2 || true
  echo "TEST FAIL: recording, see $OUT/record.log" >&2
  exit 1
fi
grep -E "^recorded:" "$OUT/record.log"

echo "=== judging the files with third-party readers ==="
# The script is single-quoted on purpose: it expands inside the container,
# from the -e variables.
# shellcheck disable=SC2016
docker run --rm -e TRACKS="$TRACKS" -e RATE="$RATE" -e QUANTUM="$QUANTUM" \
  -v "$(host_path "$OUT"):/out" -v "$(host_path "$ROOT/tools"):/interop-tools:ro" \
  "$READER_IMAGE" bash -c '
    { ffprobe -version | head -1; soxi 2>&1 | head -1
      dpkg-query -W -f "\${Package} \${Version}\n" libsndfile1 ardour
      mediainfo --version | tail -1; } | sed "s/^/reader: /"
    python3 /interop-tools/bwf-interop.py check --dir /out/session \
      --tracks "$TRACKS" --rate "$RATE" --quantum "$QUANTUM" \
      --workdir /out/ardour' >"$OUT/check.log" 2>&1 && RC=0 || RC=$?

grep -E "^(reader:|FAIL|xfail|RESULT|TEST )" "$OUT/check.log"
echo "full log: $OUT/check.log"
exit "$RC"
