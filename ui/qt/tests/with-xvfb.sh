#!/bin/sh
# Runs a command against a private Xvfb, then stops it.
#
# Not xvfb-run: its readiness handshake is a SIGUSR1 from Xvfb, caught by a
# trap and a `wait` in the script, and that races. In CI the test started
# before the server listened ("could not connect to display :99"), and as
# PID 1 of a container xvfb-run waited forever for a signal already handled.
# -displayfd is the server's own answer: it writes the display number once
# it accepts connections, and picks a free one.
#
#   with-xvfb.sh WIDTHxHEIGHTxDEPTH command [args...]
set -u

screen="$1"
shift

dir="$(mktemp -d)"
Xvfb -displayfd 3 -screen 0 "$screen" -nolisten tcp 3>"$dir/display" 2>"$dir/xvfb.log" &
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
