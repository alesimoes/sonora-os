#!/usr/bin/env bash
# ADR-0010 §1 and Unverified 2: the shipped rc.xml against labwc 0.8.3 itself.
#
# Headless labwc (pixman, no input devices) is started through the image's own
# /usr/local/bin/sonora-compositor and /usr/share/sonora/labwc. `wev` is the
# focused client; `wtype` (virtual keyboard) and `wlrctl` (virtual pointer)
# play the operator. Two runs:
#   shipped   Alt-Tab, Super-Return and Super-a reach the client; a left and a
#             right press on the empty desktop open no root menu (a key typed
#             afterwards still reaches the client); a left press on the client
#             reaches it.
#   defaults  the same probe with an rc.xml that declares no binding, so labwc
#             loads its defaults. Here the chords must be swallowed and the
#             desktop presses must open the root menu. If this run passes the
#             shipped checks, the probe cannot tell a kiosk from a desktop.
#
# What this cannot show: Ctrl+Alt+Fn under srvrkeys:none. wtype uploads its own
# keymap, so its keys never go through labwc's XKB options. That, and real
# hardware input, stay with the QEMU check. Needs Docker; no privileges.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=sonora-labwc-test:trixie
export MSYS_NO_PATHCONV=1
src="$ROOT"
command -v cygpath >/dev/null 2>&1 && src="$(cygpath -w "$ROOT")"

# labwc and wlroots are the versions ADR-0010 was written against; a newer
# labwc may change what an rc.xml without <default/> loads.
docker build -q --label org.sonora=1 -t "$IMAGE" - >/dev/null <<'EOF'
FROM debian:trixie
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      labwc=0.8.3-1 libwlroots-0.18=0.18.2-3 wev=1.0.0-3 wtype=0.4-3 wlrctl=0.2.2-2 \
 && rm -rf /var/lib/apt/lists/*
EOF

# Runs inside the container. $1 is the run: shipped or defaults.
# shellcheck disable=SC2016 # expanded by the container's bash, not here
probe='
set -eu
run="$1"
I=/src/iso/config/includes.chroot
mkdir -p /usr/share/sonora/labwc
cp "$I"/usr/share/sonora/labwc/rc.xml "$I"/usr/share/sonora/labwc/menu.xml /usr/share/sonora/labwc/
[ "$run" = defaults ] && echo "<labwc_config/>" > /usr/share/sonora/labwc/rc.xml
# Bind-mounted from Windows, the wrapper may lack its mode bits.
cp "$I"/usr/local/bin/sonora-compositor /usr/local/bin/ && chmod 755 /usr/local/bin/sonora-compositor

export XDG_RUNTIME_DIR=/tmp/xdg WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1
mkdir -p -m 700 "$XDG_RUNTIME_DIR"
sonora-compositor > /tmp/labwc.log 2>&1 &
for _ in $(seq 100); do [ -S "$XDG_RUNTIME_DIR/wayland-0" ] && break; sleep 0.1; done
[ -S "$XDG_RUNTIME_DIR/wayland-0" ] || { echo "labwc did not start"; cat /tmp/labwc.log; exit 2; }
export WAYLAND_DISPLAY=wayland-0
# wev buffers stdout when it is a file; the log is read while it runs.
stdbuf -oL wev -f wl_keyboard:key -f wl_pointer > /tmp/wev.log 2>&1 &
for _ in $(seq 100); do grep -q "identifier=wev" /tmp/labwc.log && break; sleep 0.1; done
sleep 0.3

seen() { grep -c "sym: $1 " /tmp/wev.log || true; }
fail=0
check() { # name expected actual
  if [ "$2" = "$3" ]; then echo "  ok    $1"; else echo "  FAIL  $1 (expected $2, got $3)"; fail=1; fi
}
settle() { sleep 0.3; }
expect_kiosk() { [ "$run" = shipped ] && echo yes || echo no; }

wtype -M alt -k Tab -m alt; settle
check "Alt-Tab reaches the client" "$(expect_kiosk)" "$([ "$(seen Tab)" -gt 0 ] && echo yes || echo no)"
wtype -M logo -k Return -m logo; settle
check "Super-Return reaches the client" "$(expect_kiosk)" "$([ "$(seen Return)" -gt 0 ] && echo yes || echo no)"

# The virtual pointer moves relatively: pin it to the top-left corner of the
# 1280x720 output, which wev (a 640x480 centred window) does not cover.
# An open root menu takes the keyboard, so a plain key typed after the click
# never arrives. Escape closes a menu before the next button; a second click
# while one is open would only close it.
wlrctl pointer move -5000 -5000; settle
wlrctl pointer click left; settle
wtype -k x; settle
check "a left press on the desktop opens no menu" "$(expect_kiosk)" \
  "$([ "$(seen x)" -gt 0 ] && echo yes || echo no)"
wtype -k Escape; settle
wlrctl pointer click right; settle
wtype -k y; settle
check "a right press on the desktop opens no menu" "$(expect_kiosk)" \
  "$([ "$(seen y)" -gt 0 ] && echo yes || echo no)"
wtype -k Escape; settle
# After the desktop checks: in the defaults run this maximizes wev, which would
# then cover the corner the desktop presses aim at.
wtype -M logo -k a -m logo; settle
check "Super-a reaches the client" "$(expect_kiosk)" "$([ "$(seen a)" -gt 0 ] && echo yes || echo no)"

# Onto wev, in the middle of the output: the Client binding must not eat it.
wlrctl pointer move 640 360; settle
wlrctl pointer click left; settle
check "a press on the client reaches it" yes \
  "$(grep -q "button: .*state: 1" /tmp/wev.log && echo yes || echo no)"
if [ "$fail" != 0 ]; then
  echo "--- wev"; tail -40 /tmp/wev.log; echo "--- labwc"; tail -40 /tmp/labwc.log
fi
exit "$fail"
'

status=0
for run in shipped defaults; do
  echo "labwc kiosk: $run"
  docker run --rm --label org.sonora=1 -v "$src:/src:ro" "$IMAGE" bash -c "$probe" probe "$run" \
    || status=1
done
[ "$status" = 0 ] && echo "labwc kiosk: PASS" || echo "labwc kiosk: FAIL"
exit "$status"
