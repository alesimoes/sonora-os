#!/bin/sh
# Runs a command against a private Xvfb, then stops it.
#
# -noreset: an X server regenerates when its last client disconnects, and a
# client connecting during that is refused. In CI's fresh containers the
# render test failed about half its runs with "could not connect to display"
# while Xvfb was up and logged nothing; with -noreset it passed 6 of 6, the
# same build in the same job. Something connects and leaves before Qt does;
# with nothing to regenerate, it no longer matters what.
#
# Not xvfb-run: as PID 1 of a container it waited forever in its SIGUSR1
# readiness handshake. -displayfd is the server's own: it writes the display
# number once it accepts connections, and picks a free one.
#
#   with-xvfb.sh WIDTHxHEIGHTxDEPTH command [args...]
set -u

screen="$1"
shift

dir="$(mktemp -d)"
Xvfb -displayfd 3 -screen 0 "$screen" -nolisten tcp -noreset 3>"$dir/display" 2>"$dir/xvfb.log" &
xvfb=$!
trap 'kill "$xvfb" 2>/dev/null; wait "$xvfb" 2>/dev/null; rm -rf "$dir"' EXIT

tries=0
until [ -s "$dir/display" ]; do
  if ! kill -0 "$xvfb" 2>/dev/null || [ "$tries" -ge 300 ]; then
    echo "with-xvfb.sh: Xvfb did not start:" >&2
    cat "$dir/xvfb.log" >&2
    exit 1
  fi
  tries=$((tries + 1))
  sleep 0.1
done

DISPLAY=":$(cat "$dir/display")" "$@"
status=$?
# A failure to reach the display looks the same from the client side whether
# the server died or refused; its own log tells them apart.
if [ "$status" -ne 0 ]; then
  if kill -0 "$xvfb" 2>/dev/null; then state=running; else state=gone; fi
  echo "with-xvfb.sh: exit $status, Xvfb $state, display :$(cat "$dir/display"); its log:" >&2
  cat "$dir/xvfb.log" >&2
fi
exit "$status"
