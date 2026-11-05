#!/usr/bin/env bash
# ADR-0010 §1 and §5 against labwc 0.8.3 itself: the two input paths that
# wtype and wlrctl cannot drive.
#
#   keys     Ctrl+Alt+F1, F2 and F12 reach the focused client as plain function
#            keys (srvrkeys:none, set by sonora-compositor), not as a VT switch.
#            A virtual keyboard carries its own keymap, so it would bypass the
#            compositor's XKB options. Instead the image's sonora-compositor
#            runs nested (WLR_BACKENDS=wayland) inside a headless labwc: to the
#            nested labwc the parent's keyboard is a real one, read as raw
#            evdev keycodes through the keymap labwc builds from XKB_DEFAULT_*.
#   pointer  An X11 window (xev under labwc's lazy Xwayland, as a plugin editor
#            would be) is moved by a title-bar drag and closed by its close
#            button, which must send WM_DELETE_WINDOW.
#
# Each runs twice. "shipped" uses the image's files. "control" starts labwc
# without srvrkeys:none, and with the Title and Close mouse contexts removed
# from rc.xml: there the chords must be swallowed and the window must neither
# move nor close. If the control run passed the shipped checks, the probe could
# not tell the configurations apart.
#
# The input comes from tools/labwc-vinput.c (built in the container), which
# sends evdev keycodes, modifiers, absolute motion and separate button press
# and release. Not covered: real input devices and a real VT (QEMU or
# hardware), and native Wayland editors: labwc 0.8.3 gives an xdg-shell window
# a title bar only if it asks for one through xdg-decoration. Needs Docker; no
# privileges.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=sonora-labwc-input:trixie
export MSYS_NO_PATHCONV=1
src="$ROOT"
command -v cygpath >/dev/null 2>&1 && src="$(cygpath -w "$ROOT")"

# labwc, wlroots and Xwayland are the versions ADR-0010 was written against.
# libc6-dev is left to match the base image's libc6.
# CI pulls the image (built on main by images.yml) and sets
# SONORA_LABWC_INPUT_IMAGE_BUILT=1; the Dockerfile is tools/Dockerfile.labwc-input.
if [[ "${SONORA_LABWC_INPUT_IMAGE_BUILT:-0}" != 1 ]]; then
  (cd "$ROOT/tools" && docker build -q --label org.sonora=1 -t "$IMAGE" \
    -f Dockerfile.labwc-input . >/dev/null)
fi

# Runs inside the container. $1 is the run: shipped or control.
# shellcheck disable=SC2016 # expanded by the container's bash, not here
probe='
set -eu
run="$1"
I=/src/iso/config/includes.chroot
mkdir -p /usr/share/sonora/labwc
cp "$I"/usr/share/sonora/labwc/rc.xml "$I"/usr/share/sonora/labwc/menu.xml /usr/share/sonora/labwc/
# Bind-mounted from Windows, the wrapper may lack its mode bits.
cp "$I"/usr/local/bin/sonora-compositor /usr/local/bin/ && chmod 755 /usr/local/bin/sonora-compositor
gcc -O1 -Wall -Wextra -Werror -o /usr/local/bin/labwc-vinput /src/tools/labwc-vinput.c \
  $(pkg-config --cflags --libs wayland-client xkbcommon)
if [ "$run" = control ]; then
  sed -i -e "/<context name=\"Title\">/,/<\/context>/d" \
         -e "/<context name=\"Close\">/,/<\/context>/d" /usr/share/sonora/labwc/rc.xml
fi

export XDG_RUNTIME_DIR=/tmp/xdg WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
mkdir -p -m 700 "$XDG_RUNTIME_DIR"
fail=0
check() { # question expected actual
  if [ "$2" = "$3" ]; then echo "  ok    $1 $3"; else echo "  FAIL  $1 $3 (expected $2)"; fail=1; fi
}
yes_no() { if "$@"; then echo yes; else echo no; fi; }
expect() { [ "$run" = shipped ] && echo yes || echo no; }
wait_for() { # file pattern
  for _ in $(seq 100); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done
  echo "  timed out waiting for \"$2\" in $1"; return 1
}

# --- keys: sonora-compositor nested in a headless parent ----------------------
wait_socket() { # name
  for _ in $(seq 100); do [ -S "$XDG_RUNTIME_DIR/$1" ] && return 0; sleep 0.1; done
  echo "no compositor on $1"; cat /tmp/*.log; exit 2
}
WLR_BACKENDS=headless labwc -C /usr/share/sonora/labwc > /tmp/parent.log 2>&1 &
PARENT=$!
wait_socket wayland-0
if [ "$run" = shipped ]; then
  WAYLAND_DISPLAY=wayland-0 WLR_BACKENDS=wayland sonora-compositor > /tmp/nested.log 2>&1 &
else
  WAYLAND_DISPLAY=wayland-0 WLR_BACKENDS=wayland labwc -C /usr/share/sonora/labwc > /tmp/nested.log 2>&1 &
fi
NESTED=$!
wait_socket wayland-1
# wev buffers stdout when it is a file; the log is read while it runs.
WAYLAND_DISPLAY=wayland-1 stdbuf -oL wev -f wl_keyboard -f xdg_toplevel > /tmp/wev.log 2>&1 &
wait_for /tmp/wev.log "configure: width: [1-9]"
# The nested labwc gains its keyboard only when the virtual one appears in the
# parent; the first wait lets it bind before any key is sent.
WAYLAND_DISPLAY=wayland-0 labwc-vinput 1280 720 <<KEYS
wait 500
key 29 1
key 56 1
key 59 1
key 59 0
key 60 1
key 60 0
key 88 1
key 88 0
key 56 0
key 29 0
wait 100
key 30 1
key 30 0
wait 300
KEYS
seen() { grep -q "sym: $1 " /tmp/wev.log; }
for f in F1 F2 F12; do
  check "Ctrl+Alt+$f reaches the client as $f:" "$(expect)" "$(yes_no seen "$f")"
done
check "the chord was held as Control and Mod1:" yes \
  "$(yes_no grep -q "depressed: 0000000C: Control Mod1" /tmp/wev.log)"
check "a plain key after the chords reaches the client:" yes "$(yes_no seen a)"
check "the nested labwc is still running:" yes "$(yes_no kill -0 "$NESTED")"
kill "$NESTED" "$PARENT" 2>/dev/null || true
wait "$NESTED" "$PARENT" 2>/dev/null || true
if [ "$fail" != 0 ]; then echo "--- wev"; tail -40 /tmp/wev.log; echo "--- nested labwc"; tail -20 /tmp/nested.log; fi

# --- pointer: title bar and close button of an X11 window --------------------
rm -f "$XDG_RUNTIME_DIR"/wayland-*
WLR_BACKENDS=headless sonora-compositor > /tmp/labwc.log 2>&1 &
wait_socket wayland-0
export WAYLAND_DISPLAY=wayland-0 DISPLAY=:0
stdbuf -oL xev -geometry 400x300 -event mouse -event structure > /tmp/xev.log 2>&1 &
XEV=$!
wait_for /tmp/xev.log MapNotify
sleep 0.5
# The window origin in layout pixels, from where xev places a pointer that sits
# at 640,360 in the 1280x720 output.
origin() {
  printf "abs 641 361\nwait 50\nabs 640 360\nwait 100\n" | labwc-vinput 1280 720
  sleep 0.2
  local at
  at=$(grep -o "([0-9]*,[0-9]*), root:(640,360)" /tmp/xev.log | tail -1 \
    | sed -E "s/^\(([0-9]+),([0-9]+)\).*/\1 \2/")
  [ -n "$at" ] || { echo "-1 -1"; return; }
  set -- $at
  echo "$((640 - $1)) $((360 - $2))"
}
read -r ox oy <<<"$(origin)"
echo "  window at $ox,$oy"
# labwc starts the move at the first motion after the press, so the window
# follows the pointer from there: -90,+50.
labwc-vinput 1280 720 <<DRAG
abs $((ox + 100)) $((oy - 10))
wait 100
btn 272 1
wait 100
abs $((ox + 90)) $((oy - 5))
wait 100
abs $((ox + 30)) $((oy + 30))
wait 100
abs $((ox)) $((oy + 45))
wait 100
btn 272 0
wait 100
DRAG
read -r nx ny <<<"$(origin)"
echo "  after the drag at $nx,$ny"
moved() { [ "$nx" = $((ox - 90)) ] && [ "$ny" = $((oy + 50)) ]; }
check "a title-bar drag moves the window:" "$(expect)" "$(yes_no moved)"
# The close button is the right-most title-bar button (labwc default layout
# "icon:iconify,max,close").
labwc-vinput 1280 720 <<CLOSE
abs $((nx + 400 - 10)) $((ny - 10))
wait 100
btn 272 1
wait 50
btn 272 0
wait 300
CLOSE
sleep 0.5
check "the close button sends WM_DELETE_WINDOW:" "$(expect)" "$(yes_no grep -q WM_DELETE_WINDOW /tmp/xev.log)"
gone() { ! kill -0 "$XEV" 2>/dev/null; }
check "the window is gone:" "$(expect)" "$(yes_no gone)"
if [ "$fail" != 0 ]; then echo "--- xev"; grep -E "Notify|Message" /tmp/xev.log | tail -20; echo "--- labwc"; tail -20 /tmp/labwc.log; fi
exit "$fail"
'

status=0
for run in shipped control; do
  echo "labwc input: $run"
  docker run --rm --label org.sonora=1 -v "$src:/src:ro" "$IMAGE" bash -c "$probe" probe "$run" \
    || status=1
done
[ "$status" = 0 ] && echo "labwc input: PASS" || echo "labwc input: FAIL"
exit "$status"
