#!/usr/bin/env bash
# Runs inside the sonora-ui container; tools/build-ui.sh is the way in.
#
#   $1  test | screenshot | measure
#   $2  screenshot file name, written to /out
#   $3  seconds per measurement run
set -euo pipefail

MODE="${1:-test}"
SHOT="${2:-}"
SECONDS_EACH="${3:-20}"
BUILD=build-ui-container
UI="$BUILD/sonora-ui"

cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD" -j"$(nproc)"
QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD" --output-on-failure

[[ "$MODE" == test ]] && exit 0

export XDG_RUNTIME_DIR=/tmp/xdg-runtime
mkdir -p "$XDG_RUNTIME_DIR" && chmod 0700 "$XDG_RUNTIME_DIR"

# cage on wlroots' headless backend: a 1280x720 output, no GPU, no input
# devices. pixman composites; the client renders with llvmpipe through
# Mesa's Wayland EGL.
in_cage() {
  WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    QT_QPA_PLATFORM=wayland cage -- "$@"
}

if [[ "$MODE" == screenshot ]]; then
  # Captured by the compositor (grim, over wlr-screencopy), not by the
  # application, so the picture is what the kiosk actually put on the output.
  # The second shot is the same shell after the engine stops answering.
  # shellcheck disable=SC2016  # expanded by the inner shell, inside cage
  in_cage sh -c '
    "$1" --kiosk --demo & ui=$!
    sleep 5
    grim "/out/$2"
    kill "$ui"; wait "$ui" || true
  ' sh "$UI" "$SHOT"
  base="${SHOT%.png}"
  # shellcheck disable=SC2016
  in_cage sh -c '
    "$1" --kiosk --demo --engine-dies-after 2 & ui=$!
    sleep 5
    grim "/out/$2"
    kill "$ui"; wait "$ui" || true
  ' sh "$UI" "$base-no-engine.png"
  echo "saved /out/$SHOT and /out/$base-no-engine.png"
  exit 0
fi

# --- measure ---------------------------------------------------------------
echo
echo "=== measurement: llvmpipe (CPU rasteriser), $(nproc) CPUs visible ==="
echo "    These figures are the cost of drawing on a CPU. They are not a"
echo "    frame-rate measurement of the target, which needs its GPU."
echo

Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/dev/null 2>&1 &
xvfb=$!
trap 'kill "$xvfb" 2>/dev/null || true' EXIT
sleep 1

run() {  # label, then sonora-ui arguments
  local label="$1"
  shift
  "$@" --meter-hz 60 --measure "$SECONDS_EACH" --measure-label "$label" 2>/dev/null
  echo
}

run "xcb on Xvfb, 18 meters" env DISPLAY=:99 QT_QPA_PLATFORM=xcb "$UI" --demo
run "xcb on Xvfb, meters hidden (baseline)" env DISPLAY=:99 QT_QPA_PLATFORM=xcb "$UI" --demo --no-meters
run "wayland under cage, 18 meters" in_cage "$UI" --kiosk --demo
run "wayland under cage, meters hidden (baseline)" in_cage "$UI" --kiosk --demo --no-meters
