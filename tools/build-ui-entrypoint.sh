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

# ui/qt skips itself when Qt or Xvfb is missing, which is right for
# tools/build-engine.sh and wrong here: this image exists to run the UI tests,
# and a green run that registered none would say the interface was tested.
listed=$(ctest --test-dir "$BUILD" -N)
for t in ui_meter_model ui_keys ui_keymaps ui_edit ui_clipboard ui_inserts ui_shell ui_render; do
  grep -q " $t\$" <<<"$listed" || { echo "error: $t is not registered; is Qt too old?" >&2; exit 1; }
done

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
  # Three shots of the demo session: the Edit window, the Mix window, and
  # the Edit window after the engine stops answering. NAME.png is the Edit
  # window; the Mix window is NAME-mix.png, with a trailing "-edit" in NAME
  # dropped, so sonora-ui-edit.png gives sonora-ui-mix.png.
  shoot() {  # file, then sonora-ui arguments
    local file="$1"
    shift
    # shellcheck disable=SC2016  # expanded by the inner shell, inside cage
    in_cage sh -c '
      out="$1"; shift
      "$@" & ui=$!
      sleep 5
      grim "/out/$out"
      kill "$ui"; wait "$ui" || true
    ' sh "$file" "$UI" --kiosk --demo --fake-engine --keymap keymaps/protools.json "$@"
  }
  base="${SHOT%.png}"
  mix="${base%-edit}-mix.png"
  # The Transport window open over the Edit window, on the demo session
  # with a tempo map: sonora-ui-transport.png beside sonora-ui-edit.png.
  shoot "${base%-edit}-transport.png" --window edit --transport-view
  echo "saved /out/${base%-edit}-transport.png"
  shoot "$SHOT" --window edit
  shoot "$mix" --window mix
  shoot "$base-no-engine.png" --window edit --engine-dies-after 2
  # The Edit window zoomed in on the snare's crossfade and the fades near it.
  shoot "$base-fades.png" --window edit --edit-view fades
  # Copy and Paste: the snare's crossfaded edit pasted on the next track,
  # with the Edit menu open on "Undo Paste".
  shoot "$base-clipboard.png" --window edit --edit-view clipboard
  # The Mix window with the plugin chooser open, and with a parameter panel.
  shoot "${mix%.png}-browser.png" --window mix --insert-view browser
  shoot "${mix%.png}-panel.png" --window mix --insert-view panel
  # Lead Vox soloed, Room and Talkback solo safe: recessed solo buttons with
  # a padlock, and no implicit-mute outline on either.
  shoot "${mix%.png}-solo-safe.png" --window mix --mix-view solo-safe
  # The Keys menu open over the Edit window: the profiles, and the a-z button.
  shoot "$base-keys.png" --window edit --keys-view profiles --keymap-dir keymaps
  # A version 3 file whose tempo map is outside the bounds: it opens
  # modified, with one held notice per value the loader repaired.
  shoot "$base-repairs.png" --window edit \
    --open common/model/tests/fixtures/v3-tempo-out-of-bounds.sonora

  # The version refusal on a real sonora-engine that announces the next
  # protocol version (--announce-versions, as the conformance suite runs it):
  # at startup the refusal page ($base-refusal.png); on reconnect, an engine
  # of this build replaced by the newer one, the Edit window's banner
  # ($base-refusal-reconnect.png).
  protocol=$(sed -n 's/.*kProtocolVersion = \([0-9]*\);.*/\1/p' contracts/api/include/sonora/api/messages.h)
  layout=$(sed -n 's/.*kStateLayoutVersion = \([0-9]*\);.*/\1/p' contracts/api/include/sonora/api/state_block.h)
  engine_dir="$(mktemp -d)"
  sock="$engine_dir/sock/engine.sock"
  engine() {  # sonora-engine arguments after the common ones; its pid in $pid
    "$BUILD/sonora-engine" --backend fake --tracks 18 --dir "$engine_dir/session" \
      --control-socket "$sock" --headless --no-memlock "$@" \
      >>"$engine_dir/engine.log" 2>&1 &
    pid=$!
    for _ in $(seq 100); do [[ -S "$sock" ]] && return 0; sleep 0.1; done
    echo "error: sonora-engine did not listen on $sock" >&2
    return 1
  }
  shoot_on_engine() {  # file, seconds, then sonora-ui arguments
    # shellcheck disable=SC2016  # expanded by the inner shell, inside cage
    in_cage sh -c '
      out="$1"; wait_s="$2"; shift 2
      "$@" & ui=$!
      sleep "$wait_s"
      grim "/out/$out"
      kill "$ui"; wait "$ui" || true
    ' sh "$@"
  }
  engine --announce-versions "$((protocol + 1))" "$layout"
  shoot_on_engine "$base-refusal.png" 4 "$UI" --kiosk --engine-socket "$sock"
  kill "$pid" 2>/dev/null || true; wait "$pid" || true
  # The interface attaches to this build's engine; at 3 s that engine is
  # killed and the newer one takes the socket; at 8 s the screen is read.
  engine
  shoot_on_engine "$base-refusal-reconnect.png" 8 "$UI" --kiosk --demo --window edit \
    --keymap keymaps/protools.json --engine-socket "$sock" &
  cage_job=$!
  sleep 3
  kill -9 "$pid" 2>/dev/null || true; wait "$pid" || true
  rm -f "$sock"
  engine --announce-versions "$((protocol + 1))" "$layout"
  wait "$cage_job" || true
  kill "$pid" 2>/dev/null || true; wait "$pid" || true
  rm -rf "$engine_dir"
  echo "saved /out/$base-refusal.png and /out/$base-refusal-reconnect.png"
  echo "saved /out/$SHOT, /out/$mix, /out/$base-no-engine.png, /out/$base-fades.png, the Mix window's chooser and panel, $base-clipboard.png, $base-keys.png and $base-repairs.png"
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

run "xcb on Xvfb, 18 meters" env DISPLAY=:99 QT_QPA_PLATFORM=xcb "$UI" --demo --fake-engine
run "xcb on Xvfb, meters hidden (baseline)" env DISPLAY=:99 QT_QPA_PLATFORM=xcb "$UI" --demo --fake-engine --no-meters
run "wayland under cage, 18 meters" in_cage "$UI" --kiosk --demo --fake-engine
run "wayland under cage, meters hidden (baseline)" in_cage "$UI" --kiosk --demo --fake-engine --no-meters
