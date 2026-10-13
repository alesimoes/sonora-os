#!/bin/bash
# Runs inside the runtime image for tools/test-bwf-interop.sh: brings up
# PipeWire with no hardware, records two takes through the engine's key loop,
# and copies the session to /out for the reader container.
#
# The PipeWire bring-up is the one in test-engine-entrypoint.sh. It is
# repeated rather than shared because that script is baked into the runtime
# image and this one is mounted, so a change here never rebuilds the image.
set -euo pipefail

TRACKS="${TRACKS:-18}"
RATE="${RATE:-48000}"
TAKE_SECONDS="${TAKE_SECONDS:-2}"
SESSION_DIR=/home/sonora/Sessions/interop
TOOLS=/interop-tools

cp /engine-build/sonora-engine /tmp/sonora-engine
chmod 0755 /tmp/sonora-engine
# Since M7 the key loop is sonora-tui, which the engine starts from beside
# itself on its terminal.
cp /engine-build/sonora-tui /tmp/sonora-tui
chmod 0755 /tmp/sonora-tui

export XDG_RUNTIME_DIR=/tmp/runtime
mkdir -p "$XDG_RUNTIME_DIR"
chmod 0700 "$XDG_RUNTIME_DIR"
eval "$(dbus-launch --sh-syntax)"

cleanup() {
  for pid in "${PLAY_PID:-}" "${WIREPLUMBER_PID:-}" "${PIPEWIRE_PID:-}"; do
    if [[ -n "$pid" ]]; then
      kill "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

pipewire >/tmp/pipewire.log 2>&1 &
PIPEWIRE_PID=$!
wireplumber >/tmp/wireplumber.log 2>&1 &
WIREPLUMBER_PID=$!
for _ in $(seq 1 50); do
  pw-cli info 0 >/dev/null 2>&1 && break
  sleep 0.2
done
pw-cli info 0 >/dev/null 2>&1 || { echo "FAIL: PipeWire never came up." >&2; exit 1; }

POSITIONS=$(python3 -c "print('[' + ','.join('AUX%d' % i for i in range($TRACKS)) + ']')")
pw-cli create-node adapter \
  "{ factory.name=support.null-audio-sink
     node.name=test-source
     media.class=Audio/Sink
     audio.position=$POSITIONS
     object.linger=true }" >/dev/null 2>&1 || {
  echo "FAIL: could not create the virtual source." >&2
  exit 1
}
for _ in $(seq 1 25); do
  pw-link -o 2>/dev/null | grep -q "test-source:monitor" && break
  sleep 0.2
done

# Signal is not what this test is about, but a silent file would hide a
# reader that only parses headers; the tone keeps the data chunk honest.
python3 /usr/local/bin/verify-recording.py --generate /tmp/tone.wav \
  --tracks "$TRACKS" --rate "$RATE" --seconds 30 >/dev/null
CHANNEL_MAP=$(python3 -c "print(','.join('AUX%d' % i for i in range($TRACKS)))")
pw-play --target test-source --channel-map "$CHANNEL_MAP" /tmp/tone.wav \
  >/tmp/pw-play.log 2>&1 &
PLAY_PID=$!

rm -rf "$SESSION_DIR"
mkdir -p "$SESSION_DIR"

python3 "$TOOLS/bwf-interop.py" drive --engine /tmp/sonora-engine \
  --dir "$SESSION_DIR" --tracks "$TRACKS" --rate "$RATE" \
  --seconds "$TAKE_SECONDS" --log /out/engine-screen.log

# /out is a host directory; on a Linux host it belongs to another uid, so it
# is made world-writable by the caller and filled with a copy, not a move.
cp -r "$SESSION_DIR" /out/session
echo "recorded: $(find /out/session -name '*.wav' | wc -l) files"
