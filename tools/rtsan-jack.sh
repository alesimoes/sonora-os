#!/usr/bin/env bash
# The JACK backend's callbacks under RealtimeSanitizer, against live servers
# (ADR-0007, first 2026-10-12 update). ctest has no server, so
# JackBackend::Impl::process_cb and xrun_cb, both SONORA_RT, and the libjack
# calls inside them, run nowhere else under RTSan. Run by
# tools/build-rtsan.sh --native after ctest, inside tools/Dockerfile.rtsan,
# as root in a throwaway container: it starts servers and writes to /etc.
#
#   tools/rtsan-jack.sh BUILD_DIR
#
# Two servers, each with a capture device that can be unplugged
# (sonora-fake-capture):
#   jackd2 on its dummy driver: the JACK API's reference implementation,
#     where xrun_cb runs on a notification thread.
#   PipeWire with pipewire-jack, through pw-jack: the JACK the ISO ships,
#     with the shipped pinned quantum and rate, where xrun_cb runs on the
#     audio thread.
# On each: sonora-rtsan-jack-canary, which must be reported (the check is
# live in process_cb on the server's thread); the backend conformance suite
# with the faults that server can be made to cause; and sonora-engine
# recording a take from the capture device, so Session::process runs from
# process_cb. halt_on_error: a report fails the run.
set -euo pipefail

BUILD="${1:?usage: rtsan-jack.sh BUILD_DIR}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export RTSAN_OPTIONS="halt_on_error=true:suppressions=$ROOT/tools/rtsan.supp"
export RTSAN_SYMBOLIZER_PATH="${RTSAN_SYMBOLIZER_PATH:-/usr/bin/llvm-symbolizer-22}"
RATE=48000
# jack_lsp and the clients must find a server, never start one.
export JACK_NO_START_SERVER=1
DEVICE_CLIENT=fake-capture

PIDS=()
cleanup() {
  for pid in "${PIDS[@]+"${PIDS[@]}"}"; do kill -CONT "$pid" 2>/dev/null || true; kill "$pid" 2>/dev/null || true; done
  PIDS=()
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

# $JACK_RUN is "" for jackd2 (the default libjack) or "pw-jack".
wait_for_ports() {
  local pattern=$1
  for _ in $(seq 1 50); do
    $JACK_RUN jack_lsp 2>/dev/null | grep -q "$pattern" && return 0
    sleep 0.2
  done
  return 1
}

# The canary's callback makes one malloc after process_cb's libjack calls.
# Reported, with process_cb in the stack, means RTSan's realtime context is
# entered on the server's thread and is still on after the libjack calls.
canary() {
  local out=/tmp/rtsan-canary-$1.log
  set +e
  $JACK_RUN "$BUILD/sonora-rtsan-jack-canary" >"$out" 2>&1
  local rc=$?
  set -e
  if [[ "$rc" == "0" ]] || ! grep -q "real-time unsafe function \`malloc\`" "$out" ||
     ! grep -q "process_cb" "$out"; then
    cat "$out" >&2
    fail "$1: the canary's malloc in process_cb was not reported (exit $rc)"
  fi
  echo "canary reported, as it must be: $(grep -m1 'real-time unsafe' "$out" | sed 's/^.*ERROR: //')"
}

conformance() {
  local out=/tmp/rtsan-conformance-$1.log
  set +e
  SONORA_CONFORMANCE_RATE="$RATE" $JACK_RUN "$BUILD/sonora-conformance-jack" >"$out" 2>&1
  local rc=$?
  set -e
  grep -E "^(ok|SKIP|FAIL|RESULT) " "$out"
  if [[ "$rc" != "0" ]] || grep -q "RealtimeSanitizer" "$out"; then
    cat "$out" >&2
    fail "$1: backend conformance under RTSan (exit $rc)"
  fi
}

# A take recorded from the capture device: Session::process, the disk
# thread's ring and the meters, all from process_cb. With a third argument,
# that command runs a second into the take, to cause an xrun, and the take
# must have counted one: xrun_cb ran, under RTSan, wherever the server
# calls it.
record() {
  local dir=/tmp/rtsan-take-$1
  rm -rf "$dir"
  $JACK_RUN "$BUILD/sonora-engine" --dir "$dir" --rate "$RATE" --tracks auto --arm-all \
    --device "$2" --wait-for-inputs 5 --record 3 >"$dir.out" 2>"$dir.err" &
  local engine=$!
  if [[ -n "${3:-}" ]]; then
    for _ in $(seq 1 50); do
      $JACK_RUN jack_lsp -c 2>/dev/null | grep -A2 "^$DEVICE_NAME:capture_AUX0$" |
        grep -q "sonora:in_1" && break
      sleep 0.2
    done
    sleep 1
    ENGINE_PID=$engine bash -c "$3"
  fi
  set +e
  wait "$engine"
  local rc=$?
  set -e
  # A take with an xrun in it has holes, and the engine says so with exit
  # 1. RTSan's halt exits 43, and its report on stderr is checked as well.
  local expected=0
  [[ -n "${3:-}" ]] && expected=1
  if [[ "$rc" != "$expected" ]] || grep -q "RealtimeSanitizer" "$dir.err"; then
    cat "$dir.out" "$dir.err" >&2
    fail "$1: sonora-engine recording under RTSan (exit $rc)"
  fi
  grep "^RESULT" "$dir.out"
  grep -q "^RESULT files=2 connected=2 " "$dir.out" || fail "$1: the take did not record both channels"
  if [[ -n "${3:-}" ]] && grep -q "^RESULT .* xruns=0 " "$dir.out"; then
    fail "$1: no xrun reached xrun_cb"
  fi
}

start_device() {
  $JACK_RUN "$BUILD/sonora-fake-capture" 2 "$DEVICE_CLIENT" >/tmp/fake-capture.log 2>&1 &
  DEVICE_PID=$!
  PIDS+=("$DEVICE_PID")
  wait_for_ports "^$DEVICE_NAME:capture_AUX1$" || fail "$JACK_NAME: the capture device never came up"
}

echo "=== RTSan: JackBackend against jackd2 (dummy driver) ==="
JACK_NAME=jackd2
JACK_RUN=""
# The device as JACK clients see it, which is what the backend names it by.
DEVICE_NAME=$DEVICE_CLIENT
# No playback or capture ports of its own: the one capture device is the
# fake, so the device policy picks it and the unplug can take it away.
# --no-realtime: the container has no RT limits; RTSan's realtime context is
# the function attribute, not the thread's scheduling class.
jackd --no-realtime -d dummy -r "$RATE" -p 128 -C 0 -P 0 >/tmp/jackd.log 2>&1 &
SERVER_PID=$!
PIDS+=("$SERVER_PID")
for _ in $(seq 1 50); do
  jack_lsp >/dev/null 2>&1 && break
  sleep 0.2
done
jack_lsp >/dev/null 2>&1 || { tail -20 /tmp/jackd.log >&2; fail "jackd never came up"; }
start_device
canary jackd2
# The xrun: the device's client stopped for a few periods, a cycle jackd2
# then reports unfinished to every client. Caused in the take, not through
# the conformance suite: its xrun check also wants a gap in frame_time,
# which jackd2's dummy driver, never itself late, does not leave.
record jackd2 "$DEVICE_NAME" "kill -STOP $DEVICE_PID; sleep 0.2; kill -CONT $DEVICE_PID"
SONORA_INJECT_BUFFER_CHANGE="jack_bufsize 256 >/dev/null" \
SONORA_RESTORE_BUFFER_CHANGE="jack_bufsize 128 >/dev/null; sleep 0.5" \
SONORA_INJECT_STALL="kill -STOP $SERVER_PID" \
SONORA_RESTORE_STALL="kill -CONT $SERVER_PID; sleep 0.5" \
SONORA_UNPLUG_DEVICE="$DEVICE_NAME" \
SONORA_INJECT_UNPLUG="kill $DEVICE_PID" \
SONORA_INJECT_SERVER_LOSS="kill $SERVER_PID" \
  conformance jackd2
cleanup
echo "JACKD2 RTSAN PASS"

echo "=== RTSan: JackBackend against PipeWire (pipewire-jack, the shipped pin) ==="
JACK_NAME=pipewire
JACK_RUN=pw-jack
install -D -m 0644 "$ROOT/audio-stack/pipewire-sonora.conf" /etc/pipewire/pipewire.conf.d/99-sonora.conf
export XDG_RUNTIME_DIR=/tmp/rtsan-runtime
mkdir -p "$XDG_RUNTIME_DIR"
chmod 0700 "$XDG_RUNTIME_DIR"
# wireplumber talks to PipeWire over a session bus.
DBUS_OUT="$(dbus-launch --sh-syntax)"
DBUS_SESSION_BUS_ADDRESS="$(sed -n "s/^DBUS_SESSION_BUS_ADDRESS='\(.*\)';$/\1/p" <<<"$DBUS_OUT")"
DBUS_PID="$(sed -n 's/^DBUS_SESSION_BUS_PID=\([0-9]*\);$/\1/p' <<<"$DBUS_OUT")"
export DBUS_SESSION_BUS_ADDRESS
PIDS+=("$DBUS_PID")
pipewire >/tmp/pipewire.log 2>&1 &
SERVER_PID=$!
PIDS+=("$SERVER_PID")
wireplumber >/tmp/wireplumber.log 2>&1 &
PIDS+=("$!")
for _ in $(seq 1 50); do
  pw-cli info 0 >/dev/null 2>&1 && break
  sleep 0.2
done
pw-cli info 0 >/dev/null 2>&1 || { tail -20 /tmp/pipewire.log >&2; fail "PipeWire never came up"; }
# The device as tools/test-engine-entrypoint.sh makes it: an onboard card's
# object.path, so the default policy sees one non-USB device. Its JACK
# name is the node description; pw-link's is the node name.
DEVICE_NAME="Built-in Audio Pro"
export PIPEWIRE_PROPS="{ object.path = \"alsa:acp:PCH:0:capture\" node.description = \"$DEVICE_NAME\" }"
start_device
unset PIPEWIRE_PROPS
canary pipewire
record pipewire "$DEVICE_NAME"
# The pin holds a forced quantum or rate (audio-stack/pipewire-sonora.conf),
# so those two are not asked for here: tools/test-engine-entrypoint.sh
# covers the held change. No xrun can be caused on demand: stopping the
# device's client or the engine itself for 0.2 s mid-take reached no
# xrun_cb (measured 2026-10-14, xruns=0 both ways), so on PipeWire, where
# it runs on the audio thread, xrun_cb is checked only by jackd2's run of
# the same function.
SONORA_INJECT_STALL="kill -STOP $SERVER_PID" \
SONORA_RESTORE_STALL="kill -CONT $SERVER_PID; sleep 0.5" \
SONORA_UNPLUG_DEVICE="$DEVICE_NAME" \
SONORA_INJECT_UNPLUG="kill $DEVICE_PID" \
SONORA_INJECT_SERVER_LOSS="kill $SERVER_PID" \
  conformance pipewire
cleanup
echo "PIPEWIRE RTSAN PASS"
