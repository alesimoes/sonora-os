#!/usr/bin/env bash
# Do the delivery files open in other people's readers?
#
# test_audio_export writes every container at every depth (SONORA_EXPORT_KEEP
# keeps them); a stock Debian trixie container then judges them with the
# reference tools: flac -t (the frame CRCs and the STREAMINFO MD5), sndfile-info
# (libsndfile, which most DAWs read through), sox --i, and ffprobe for the
# bext. Exit 0 only if every reader accepted every file.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_IMAGE=sonora-build:trixie
OUT="${EXPORT_INTEROP_OUT:-$ROOT/build-export-interop}"

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
docker info >/dev/null 2>&1 || { echo "error: the Docker daemon is not reachable." >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/files"
chmod 0777 "$OUT" "$OUT/files"

echo "=== writing the files ==="
docker run --rm -v "$(host_path "$ROOT"):/src:ro" -v "$(host_path "$OUT"):/out" \
  -v sonora-export-interop-build:/build "$BUILD_IMAGE" bash -lc '
    set -euo pipefail
    cmake -S /src -B /build -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build /build -j"$(nproc)" --target test_audio_export >/dev/null
    cd /build && SONORA_EXPORT_KEEP=/out/files ./test_audio_export
  '

echo "=== judging them with the reference tools ==="
docker run --rm -v "$(host_path "$OUT"):/out:ro" debian:trixie bash -lc '
  set -uo pipefail
  apt-get update -qq >/dev/null && apt-get install -y -qq --no-install-recommends \
    flac sox sndfile-programs ffmpeg >/dev/null 2>&1
  fail=0
  for f in /out/files/*; do
    case "$f" in
      *.flac)
        # A warning (a frame number that does not follow, say) is a failure.
        out=$(flac -t -s "$f" 2>&1)
        if [ -z "$out" ]; then echo "ok   flac -t   $(basename "$f")"
        else echo "FAIL flac -t   $(basename "$f")"; fail=1; fi ;;
    esac
    if sndfile-info "$f" >/dev/null 2>&1; then echo "ok   sndfile    $(basename "$f")"
    else echo "FAIL sndfile    $(basename "$f")"; fail=1; fi
    if sox --i "$f" >/dev/null 2>&1; then echo "ok   sox        $(basename "$f")"
    else echo "FAIL sox        $(basename "$f")"; fail=1; fi
    # Decoding everything is what finds a bad frame in a container ffprobe
    # would only open.
    if ffmpeg -v error -i "$f" -f null - 2>/dev/null; then echo "ok   ffmpeg     $(basename "$f")"
    else echo "FAIL ffmpeg     $(basename "$f")"; fail=1; fi
  done
  for f in /out/files/export_0_16.wav; do
    t=$(ffprobe -v error -show_entries format_tags=time_reference,encoded_by,description -of default=nw=1 "$f" 2>&1)
    echo "$t"
    echo "$t" | grep -q "time_reference=123456" || { echo "FAIL bext time_reference"; fail=1; }
    echo "$t" | grep -q "encoded_by=Sonora Engine" || { echo "FAIL bext originator (ffprobe: encoded_by)"; fail=1; }
  done
  exit $fail
'
