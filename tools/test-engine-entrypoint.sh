#!/bin/bash
# Runs inside the runtime container: brings up PipeWire with no hardware,
# patches a known signal into the engine, records, and verifies what landed on
# disk.
set -euo pipefail

SECONDS_TO_RECORD="${RECORD_SECONDS:-3}"
TRACKS="${TRACKS:-18}"
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

echo "--- sonora-engine --backend fake, before any audio server exists ---"
# drivers/fake (ADR-0007 M3): the whole engine with no server, shaped as the
# UMC1820. Its tones are verify-recording.py's, so the same check proves 18
# files, full length, each carrying its own channel.
FAKE_DIR=/home/sonora/Sessions/fake-backend
rm -rf "$FAKE_DIR"
mkdir -p /home/sonora/Sessions
"$ENGINE_BIN" --backend fake --dir "$FAKE_DIR" --tracks auto --arm-all --record 2 \
  >/tmp/fake-backend.out 2>/tmp/fake-backend.err || {
  echo "FAIL: sonora-engine --backend fake exited $?" >&2
  cat /tmp/fake-backend.out /tmp/fake-backend.err >&2
  exit 1
}
grep "^RESULT" /tmp/fake-backend.out
grep -q "^RESULT files=18 connected=18 " /tmp/fake-backend.out || {
  echo "FAIL: --backend fake did not record 18 connected channels" >&2
  exit 1
}
python3 /usr/local/bin/verify-recording.py --check "$FAKE_DIR/Audio Files" \
  --tracks 18 --rate 48000 --seconds 2

echo "--- the engine as a process, an interface on its socket (ADR-0007 M7) ---"
# sonora-engine serves its control socket and the memfd state block;
# sonora-ctl, a separate process, arms, records, stops and plays the take
# back from 0 through the socket alone, checking the state block at each
# step: playback must roll, and a refusal of any kind fails the step.
# Then the files are judged by the same verifier, so the take an interface
# drove from outside is proven channel for channel.
if [[ -f /engine-build/sonora-ctl ]]; then
  cp /engine-build/sonora-ctl /tmp/sonora-ctl
  chmod 0755 /tmp/sonora-ctl
  E2E_DIR=/home/sonora/Sessions/e2e
  E2E_SOCK=/tmp/sonora-e2e/engine.sock
  rm -rf "$E2E_DIR"
  "$ENGINE_BIN" --backend fake --dir "$E2E_DIR" --tracks auto --headless \
    --control-socket "$E2E_SOCK" >/tmp/e2e-engine.out 2>/tmp/e2e-engine.err &
  E2E_PID=$!
  /tmp/sonora-ctl --control-socket "$E2E_SOCK" take 2.5 >/tmp/e2e-ctl.out 2>&1 || {
    echo "FAIL: the socket client's take failed" >&2
    cat /tmp/e2e-ctl.out /tmp/e2e-engine.out /tmp/e2e-engine.err >&2
    kill "$E2E_PID" 2>/dev/null || true
    exit 1
  }
  cat /tmp/e2e-ctl.out
  grep -q "^RESULT checks_failed=0 .* play=ok" /tmp/e2e-ctl.out || {
    echo "FAIL: the engine did not roll the take back over the socket" >&2
    exit 1
  }
  /tmp/sonora-ctl --control-socket "$E2E_SOCK" state
  kill -TERM "$E2E_PID"
  set +e
  wait "$E2E_PID"
  E2E_RC=$?
  set -e
  [[ "$E2E_RC" == "0" ]] || {
    echo "FAIL: the engine exited $E2E_RC after SIGTERM" >&2
    cat /tmp/e2e-engine.err >&2
    exit 1
  }
  [[ ! -e "$E2E_SOCK" ]] || { echo "FAIL: the engine left its socket behind" >&2; exit 1; }
  python3 /usr/local/bin/verify-recording.py --check "$E2E_DIR/Audio Files" \
    --tracks 18 --rate 48000 --seconds 2
  echo "PROCESS SPLIT PASS"
else
  echo "FAIL: no sonora-ctl beside the engine" >&2
  exit 1
fi

export XDG_RUNTIME_DIR=/tmp/runtime
mkdir -p "$XDG_RUNTIME_DIR"
chmod 0700 "$XDG_RUNTIME_DIR"

# wireplumber talks to PipeWire over a session bus.
eval "$(dbus-launch --sh-syntax)"

cleanup() {
  for pid in "${PLAY_PID:-}" "${ENGINE_PID:-}" "${ONBOARD_PID:-}" "${HDMI_PID:-}" "${WIREPLUMBER_PID:-}" "${PIPEWIRE_PID:-}"; do
    if [[ -n "$pid" ]]; then
      kill "$pid" 2>/dev/null || true
    fi
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

echo "--- adding a physical capture device that is not USB ---"
# A laptop has onboard audio. Without --device the engine records from it
# only once the startup wait is over with nothing connected, so here, where
# the inputs are patched by hand within the wait, it must be left alone. A
# JACK client with physical ports and an onboard card's object.path stands in
# for it; no /proc/asound/PCH/usbid exists here, so it reads as not USB, as
# onboard audio does.
ONBOARD_NAME="Built-in Audio Pro"
# What pw-link calls it: the node.name, where JACK clients see the description.
ONBOARD_NODE=fake-capture
if [[ -f /engine-build/sonora-fake-capture ]]; then
  cp /engine-build/sonora-fake-capture /tmp/sonora-fake-capture
  chmod 0755 /tmp/sonora-fake-capture
  PIPEWIRE_PROPS="{ object.path = \"alsa:acp:PCH:0:capture\" node.description = \"$ONBOARD_NAME\" }" \
    /tmp/sonora-fake-capture 2 >/tmp/onboard.log 2>&1 &
  ONBOARD_PID=$!
  for _ in $(seq 1 25); do
    pw-link -o 2>/dev/null | grep -q "^$ONBOARD_NODE:capture_AUX0" && break
    sleep 0.2
  done
  pw-link -o 2>/dev/null | grep -q "^$ONBOARD_NODE:capture_AUX0" || {
    echo "FAIL: the stand-in onboard device never exposed its ports." >&2
    exit 1
  }
else
  echo "note: no sonora-fake-capture beside the engine; onboard check skipped" >&2
fi

echo "--- starting the engine ---"
# The engine auto-connects to a USB interface during the wait; a container
# has none, so the patching happens below. --wait-for-inputs is what makes
# that possible, and it is the same flag the appliance uses for a USB
# interface that is enumerated after boot.
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

if [[ -n "${ONBOARD_PID:-}" ]]; then
  # By now the engine has had more than one of its half-second retries.
  sleep 1
  if pw-link -l 2>/dev/null | grep -A2 "^$ONBOARD_NODE:capture_" | grep -q "sonora:in_"; then
    echo "FAIL: the engine connected the onboard device over a hand patch." >&2
    pw-link -l >&2 2>/dev/null
    exit 1
  fi
  echo "onboard device left unconnected"
fi

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

echo "--- playback: a take played through the monitor bus, looped back, recorded ---"
# The engine plays track 1's take from above on its stereo monitor bus while
# track 2 records sonora:out_1, patched back into sonora:in_2. The second
# take must be the first at the pan law's centre gain, from timeline frame 0,
# late only by whole periods of loop latency: the playback path end to end,
# through the disk reader's residency gate, against the real server. Armed
# tracks monitor nothing here, or the loop would feed back.
#
# The loop is inside one client, and PipeWire's JACK layer gives the
# self-linked input the output's own buffer, one of two in turn: that is the
# two periods of loop latency. A period the dummy driver skips, woken late
# on a loaded host, breaks the turn, and in_2 reads two blocks swapped,
# though the engine's out_1 is continuous (measured 2026-10-13: every one of
# the swaps came with a skipped period; out_1 as it left the callback had
# none). The engine counts the skip as a clock step; the verifier then lets
# a few blocks null a period early or late, two per step and nothing else.
# A take that still does not null is recorded again, up to three times, only
# if steps were counted; with none it fails at once.
PB_DIR=/home/sonora/Sessions/playback
for attempt in 1 2 3; do
  rm -rf "$PB_DIR"
  mkdir -p "$PB_DIR/Audio Files"
  cp "$SESSION_DIR/Audio Files/Audio 1_01.wav" "$PB_DIR/Audio Files/"
  "$ENGINE_BIN" --dir "$PB_DIR" --tracks 2 --rate "$RATE" \
    --place "Audio Files/Audio 1_01.wav" --arm 2 --no-input-monitoring \
    --wait-for-inputs 20 --record "$SECONDS_TO_RECORD" >/tmp/playback.out 2>/tmp/playback.err &
  ENGINE_PID=$!
  for _ in $(seq 1 50); do
    pw-link -i 2>/dev/null | grep -q "sonora:in_2" &&
      pw-link -o 2>/dev/null | grep -q "sonora:out_1" && break
    sleep 0.2
  done
  pw-link "sonora:out_1" "sonora:in_2" >/dev/null 2>&1 || {
    echo "FAIL: could not loop sonora:out_1 back into sonora:in_2." >&2
    exit 1
  }
  set +e
  wait "$ENGINE_PID"
  ENGINE_RC=$?
  set -e
  ENGINE_PID=""
  if [[ "$ENGINE_RC" != "0" ]]; then
    echo "FAIL: the playback run exited $ENGINE_RC." >&2
    cat /tmp/playback.out /tmp/playback.err >&2
    exit 1
  fi
  grep "^RESULT" /tmp/playback.out
  BUFFER=$(sed -n 's/^RESULT .* buffer=\([0-9]*\) .*/\1/p' /tmp/playback.out)
  STEPS=$(sed -n 's/^RESULT .* clock_steps=\([0-9]*\) .*/\1/p' /tmp/playback.out)
  set +e
  python3 /usr/local/bin/verify-recording.py --check-playback \
    "$PB_DIR/Audio Files/Audio 1_01.wav" "$PB_DIR/Audio Files/Audio 2_02.wav" \
    --buffer "$BUFFER" --clock-steps "${STEPS:-0}"
  NULL_RC=$?
  set -e
  [[ "$NULL_RC" == "0" ]] && break
  if [[ "${STEPS:-0}" == "0" || "$attempt" == "3" ]]; then
    echo "FAIL: the loopback take does not null (clock steps: ${STEPS:-?}, attempt $attempt)." >&2
    exit 1
  fi
  echo "note: the server skipped a period during the take (clock_steps=$STEPS);" \
    "recording it again"
done

# The device policy, with the fake onboard device as the laptop's own audio
# (revised by the owner 2026-10-13; ADR-0007 M2 note, item 8). Each engine
# run below is short; its stderr is kept and shown when a check fails.
if [[ -z "${ONBOARD_PID:-}" ]]; then
  echo "note: device policy checks skipped: no sonora-fake-capture" >&2
  exit 0
fi
kill "$PLAY_PID" 2>/dev/null || true
PLAY_PID=""

# Runs the engine to completion: ENGINE_RC, and its output in /tmp/$1.out
# and /tmp/$1.err.
run_engine() {
  local tag=$1
  shift
  rm -rf "/home/sonora/Sessions/$tag"
  set +e
  "$ENGINE_BIN" --dir "/home/sonora/Sessions/$tag" --rate "$RATE" --arm-all "$@" \
    >"/tmp/$tag.out" 2>"/tmp/$tag.err"
  ENGINE_RC=$?
  set -e
}

fail_with() {
  echo "FAIL: $1" >&2
  echo "--- stdout ---" >&2; cat "/tmp/$2.out" >&2
  echo "--- stderr ---" >&2; cat "/tmp/$2.err" >&2
  exit 1
}

wait_for_node_ports() {
  for _ in $(seq 1 25); do
    pw-link -o 2>/dev/null | grep -q "^$1:capture_AUX0" && return 0
    sleep 0.2
  done
  echo "FAIL: $1 never exposed its ports." >&2
  exit 1
}

NOTICE="recording from $ONBOARD_NAME (onboard): no USB interface found"

echo "--- only onboard audio, --tracks auto: recorded from after the wait ---"
run_engine onboard-auto --tracks auto --wait-for-inputs 2 --record 2
[[ "$ENGINE_RC" == "0" ]] || fail_with "the engine exited $ENGINE_RC on onboard audio" onboard-auto
grep -qF "$NOTICE" /tmp/onboard-auto.err || fail_with "no onboard notice" onboard-auto
grep -q "^RESULT files=2 connected=2 " /tmp/onboard-auto.out ||
  fail_with "--tracks auto did not size to the 2 onboard channels" onboard-auto
grep "^RESULT" /tmp/onboard-auto.out
echo "onboard notice: $(grep -F "$NOTICE" /tmp/onboard-auto.err)"

echo "--- only onboard audio, --tracks 4: the same, after the session exists ---"
run_engine onboard-fixed --tracks 4 --wait-for-inputs 2 --record 2
[[ "$ENGINE_RC" == "0" ]] || fail_with "the engine exited $ENGINE_RC on onboard audio" onboard-fixed
grep -qF "$NOTICE" /tmp/onboard-fixed.err || fail_with "no onboard notice" onboard-fixed
grep -q "^RESULT .*connected=2 " /tmp/onboard-fixed.out ||
  fail_with "the onboard channels were not connected" onboard-fixed
grep "^RESULT" /tmp/onboard-fixed.out

echo "--- the patched onboard device killed mid-take: DeviceGone ends it ---"
GONE_DIR=/home/sonora/Sessions/device-gone
rm -rf "$GONE_DIR"
"$ENGINE_BIN" --dir "$GONE_DIR" --rate "$RATE" --arm-all --device "$ONBOARD_NAME" \
  --tracks auto --wait-for-inputs 5 --record 30 >/tmp/device-gone.out 2>/tmp/device-gone.err &
ENGINE_PID=$!
for _ in $(seq 1 50); do
  pw-link -l 2>/dev/null | grep -A2 "^$ONBOARD_NODE:capture_AUX0" | grep -q "sonora:in_1" && break
  sleep 0.2
done
pw-link -l 2>/dev/null | grep -A2 "^$ONBOARD_NODE:capture_AUX0" | grep -q "sonora:in_1" ||
  fail_with "the named onboard device was never connected" device-gone
# Into the take, so there is audio on disk for the close to keep.
sleep 2
kill "$ONBOARD_PID"
wait "$ONBOARD_PID" 2>/dev/null || true
ONBOARD_PID=""
set +e
wait "$ENGINE_PID"
ENGINE_RC=$?
set -e
ENGINE_PID=""
[[ "$ENGINE_RC" == "1" ]] || fail_with "the engine exited $ENGINE_RC, not 1, on DeviceGone" device-gone
grep -q "capture device $ONBOARD_NAME went away" /tmp/device-gone.err ||
  fail_with "the reason does not name the device that went away" device-gone
python3 - "$GONE_DIR/Audio Files" <<'PY' || fail_with "the take was not closed with audio in it" device-gone
import pathlib, sys, wave
files = sorted(pathlib.Path(sys.argv[1]).glob("*.wav"))
assert len(files) == 2, files
for f in files:
    with wave.open(str(f)) as w:
        assert w.getnframes() > 48000, (f.name, w.getnframes())
print(f"closed take: {len(files)} files, {wave.open(str(files[0])).getnframes()} frames each")
PY
echo "reason: $(grep "went away" /tmp/device-gone.err)"

echo "--- two non-USB devices and none named: refused, both listed ---"
start_fake() {
  PIPEWIRE_PROPS="{ object.path = \"$3\" node.description = \"$2\" }" \
    /tmp/sonora-fake-capture 2 "$1" >"/tmp/$1.log" 2>&1 &
}
start_fake "$ONBOARD_NODE" "$ONBOARD_NAME" "alsa:acp:PCH:0:capture"
ONBOARD_PID=$!
start_fake fake-hdmi "HDMI Audio Pro" "alsa:acp:HDMI:3:capture"
HDMI_PID=$!
wait_for_node_ports "$ONBOARD_NODE"
wait_for_node_ports fake-hdmi
for tracks in auto 2; do
  run_engine "two-onboard-$tracks" --tracks "$tracks" --wait-for-inputs 2 --record 2
  [[ "$ENGINE_RC" == "1" ]] ||
    fail_with "the engine exited $ENGINE_RC with two non-USB devices" "two-onboard-$tracks"
  for listed in "\"$ONBOARD_NAME\"" '"HDMI Audio Pro"'; do
    grep -qF "$listed" "/tmp/two-onboard-$tracks.err" ||
      fail_with "the refusal does not list $listed" "two-onboard-$tracks"
  done
  echo "refused (--tracks $tracks): $(tail -1 "/tmp/two-onboard-$tracks.err")"
done
kill "$HDMI_PID" 2>/dev/null || true
HDMI_PID=""

echo "DEVICE POLICY PASS"

# The backend conformance suite (ADR-0007 §7) against JackBackend on this
# PipeWire, the same suite ctest runs against drivers/fake. The one physical
# capture device left is the stand-in onboard card. Faults are caused from
# here: the quantum and rate through PipeWire's settings metadata, a stall
# by stopping the server, the unplug by killing the card's client, and the
# server loss, last, by killing PipeWire. Xruns and clock steps cannot be
# caused on demand here and are reported as skipped.
#
# The shipped config pins the quantum and rate (audio-stack/
# pipewire-sonora.conf, settings.check-quantum/-rate), and this container
# runs it, so the forced change is refused: measured 2026-10-13, the
# settings metadata stores clock.force-quantum 256 while the running driver
# stays at quantum 128. Whether it took is asked of the running driver
# (pw-top), not of the metadata; held, the check is skipped with the reason
# and the backend must carry on unharmed. The pin is never loosened here.
if [[ ! -f /engine-build/sonora-conformance-jack ]]; then
  echo "note: no sonora-conformance-jack beside the engine; conformance skipped" >&2
  exit 0
fi
cp /engine-build/sonora-conformance-jack /tmp/sonora-conformance-jack
chmod 0755 /tmp/sonora-conformance-jack
# The running driver's quantum and rate, as "QUANT RATE".
cat >/tmp/sonora-graph-clock <<'SH'
#!/bin/sh
pw-top -b -n 2 | awk '$1 == "R" && $NF == "Dummy-Driver" { q = $3; r = $4 } END { print q, r }'
SH
chmod 0755 /tmp/sonora-graph-clock
PINNED="the server pins its quantum and rate (audio-stack/pipewire-sonora.conf); the refusal path is covered by the drivers/support PeriodGuard tests and the fake rigs"
echo "--- backend conformance suite against JackBackend ---"
echo "graph clock before: $(/tmp/sonora-graph-clock)"
set +e
SONORA_CONFORMANCE_RATE="$RATE" \
SONORA_INJECT_BUFFER_CHANGE="pw-metadata -n settings 0 clock.force-quantum 256 >/dev/null" \
SONORA_TOOK_BUFFER_CHANGE="sleep 1; /tmp/sonora-graph-clock | grep -q '^256 '" \
SONORA_HELD_WHY_BUFFER_CHANGE="$PINNED" \
SONORA_RESTORE_BUFFER_CHANGE="pw-metadata -n settings 0 clock.force-quantum 0 >/dev/null; sleep 0.5" \
SONORA_INJECT_RATE_CHANGE="pw-metadata -n settings 0 clock.force-rate 96000 >/dev/null" \
SONORA_TOOK_RATE_CHANGE="sleep 1; /tmp/sonora-graph-clock | grep -q ' 96000$'" \
SONORA_HELD_WHY_RATE_CHANGE="$PINNED" \
SONORA_RESTORE_RATE_CHANGE="pw-metadata -n settings 0 clock.force-rate 0 >/dev/null; sleep 0.5" \
SONORA_INJECT_STALL="kill -STOP $PIPEWIRE_PID" \
SONORA_RESTORE_STALL="kill -CONT $PIPEWIRE_PID; sleep 0.5" \
SONORA_UNPLUG_DEVICE="$ONBOARD_NAME" \
SONORA_INJECT_UNPLUG="kill $ONBOARD_PID" \
SONORA_INJECT_SERVER_LOSS="kill $PIPEWIRE_PID" \
  /tmp/sonora-conformance-jack
CONFORMANCE_RC=$?
set -e
ONBOARD_PID=""
PIPEWIRE_PID=""
[[ "$CONFORMANCE_RC" == "0" ]] || { echo "FAIL: backend conformance against JACK" >&2; exit 1; }
echo "CONFORMANCE PASS"
