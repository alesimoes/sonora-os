#!/bin/bash
# Runs inside the runtime container: brings up PipeWire with no hardware,
# patches a known signal into the engine, records, and verifies what landed on
# disk.
set -euo pipefail

SECONDS_TO_RECORD="${RECORD_SECONDS:-3}"
TRACKS="${TRACKS:-4}"
RATE="${RATE:-48000}"
SESSION_DIR=/home/sonora/Sessions/test

# The engine is bind-mounted from the host build directory, where the exec bit
# does not survive. Copy it somewhere writable and set the mode here.
if [[ -f /engine-build/sonora-engine ]]; then
  cp /engine-build/sonora-engine /tmp/sonora-engine
  chmod 0755 /tmp/sonora-engine
  ENGINE_BIN=/tmp/sonora-engine
else
  ENGINE_BIN=/usr/local/bin/sonora-engine
fi
[[ -x "$ENGINE_BIN" ]] || { echo "FAIL: no engine binary at $ENGINE_BIN" >&2; exit 1; }

export XDG_RUNTIME_DIR=/tmp/runtime
mkdir -p "$XDG_RUNTIME_DIR"
chmod 0700 "$XDG_RUNTIME_DIR"

# wireplumber talks to PipeWire over a session bus.
eval "$(dbus-launch --sh-syntax)"

cleanup() {
  for pid in "${PLAY_PID:-}" "${ENGINE_PID:-}" "${WIREPLUMBER_PID:-}" "${PIPEWIRE_PID:-}"; do
    [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

echo "--- starting PipeWire (no hardware, dummy driver) ---"
pipewire >/tmp/pipewire.log 2>&1 &
PIPEWIRE_PID=$!
wireplumber >/tmp/wireplumber.log 2>&1 &
WIREPLUMBER_PID=$!

# Wait for the server rather than sleeping a guessed amount.
for _ in $(seq 1 50); do
  pw-cli info 0 >/dev/null 2>&1 && break
  sleep 0.2
done
pw-cli info 0 >/dev/null 2>&1 || {
  echo "FAIL: PipeWire never became reachable." >&2
  tail -20 /tmp/pipewire.log >&2
  exit 1
}
echo "PipeWire is up."

echo "--- creating a virtual source with $TRACKS channels ---"
# A null sink gives us real ports with no hardware behind them. Its monitor
# ports carry whatever is played into the sink, which is how a known signal
# reaches the engine inputs.
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
pw-link -o 2>/dev/null | grep "test-source" || {
  echo "FAIL: the virtual source never exposed monitor ports." >&2
  pw-link -o >&2 2>/dev/null
  exit 1
}

echo "--- generating the test signal ---"
# A different frequency per track, so a channel written to the wrong file is
# detectable rather than merely suspicious.
python3 /usr/local/bin/verify-recording.py --generate /tmp/test-tone.wav \
  --tracks "$TRACKS" --rate "$RATE" --seconds "$((SECONDS_TO_RECORD + 4))"

rm -rf "$SESSION_DIR"
mkdir -p "$SESSION_DIR"

echo "--- starting playback into the virtual source ---"
# Without an explicit channel map pw-play downmixes to stereo, and every
# track above the second records silence -- which looks exactly like an
# engine bug.
CHANNEL_MAP=$(python3 -c "print(','.join('AUX%d' % i for i in range($TRACKS)))")
pw-play --target test-source --channel-map "$CHANNEL_MAP" /tmp/test-tone.wav >/tmp/pw-play.log 2>&1 &
PLAY_PID=$!

echo "--- starting the engine ---"
# The engine auto-connects to physical capture ports; a container has none, so
# the patching happens below. --wait-for-inputs is what makes that possible,
# and it is the same flag the appliance uses for a USB interface that is
# enumerated after boot.
"$ENGINE_BIN" \
  --dir "$SESSION_DIR" \
  --tracks "$TRACKS" \
  --rate "$RATE" \
  --arm-all \
  --wait-for-inputs 20 \
  --record "$SECONDS_TO_RECORD" &
ENGINE_PID=$!

for _ in $(seq 1 50); do
  pw-link -i 2>/dev/null | grep -q "sonora:in_1" && break
  sleep 0.2
done

echo "--- patching $TRACKS inputs ---"
CONNECTED=0
for i in $(seq 1 "$TRACKS"); do
  SRC="test-source:monitor_AUX$((i - 1))"
  if pw-link "$SRC" "sonora:in_$i" >/dev/null 2>&1; then
    CONNECTED=$((CONNECTED + 1))
  else
    echo "note: could not connect $SRC -> sonora:in_$i" >&2
  fi
done
echo "patched $CONNECTED of $TRACKS inputs"

if [[ "$CONNECTED" == "0" ]]; then
  echo "FAIL: nothing could be patched into the engine." >&2
  echo "outputs:" >&2; pw-link -o >&2 2>/dev/null
  echo "inputs:" >&2;  pw-link -i >&2 2>/dev/null
  exit 1
fi

set +e
wait "$ENGINE_PID"
ENGINE_RC=$?
set -e
ENGINE_PID=""

if [[ "$ENGINE_RC" != "0" ]]; then
  echo "FAIL: the engine exited $ENGINE_RC." >&2
  exit "$ENGINE_RC"
fi

echo "--- verifying the files ---"
python3 /usr/local/bin/verify-recording.py --check "$SESSION_DIR/Audio Files" \
  --tracks "$TRACKS" --rate "$RATE" --seconds "$SECONDS_TO_RECORD"
