#!/bin/bash
# Runs inside the runtime container for tools/test-quantum-pin.sh.
#
# Brings up PipeWire with the shipped drop-in, keeps one JACK client (the
# observer, standing in for the engine) running, and has a second client try
# every way we know of to move the graph to a 64-frame quantum or to 96 kHz.
# The observer records the size of every block it is handed. One block of any
# size other than 128, or any rate other than 48000, fails the test: under
# ADR-0007 M2 that is the event that closes a take.
set -euo pipefail

PROBE=/usr/local/bin/jack-quantum-probe.py
PINNED_QUANTUM=128
PINNED_RATE=48000

export XDG_RUNTIME_DIR=/tmp/runtime
mkdir -p "$XDG_RUNTIME_DIR"
chmod 0700 "$XDG_RUNTIME_DIR"
eval "$(dbus-launch --sh-syntax)"

cleanup() {
  for pid in "${WIREPLUMBER_PID:-}" "${PIPEWIRE_PID:-}"; do
    if [[ -n "$pid" ]]; then kill "$pid" 2>/dev/null || true; fi
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
pw-cli info 0 >/dev/null 2>&1 || { echo "FAIL: PipeWire never became reachable." >&2; tail -20 /tmp/pipewire.log >&2; exit 1; }

echo "PipeWire $(pipewire --version | sed -n 's/.*libpipewire //p'), settings as the server reports them:"
pw-metadata -n settings 0 2>/dev/null | grep -o "key:'clock[^']*' value:'[^']*'" | sed 's/^/  /'

FAILED=0
GAP=0
ok()  { if [[ "$GAP" == 1 ]]; then echo "QUANTUM gap   closed?: $1"; else echo "QUANTUM ok    $1"; fi; }
bad() { if [[ "$GAP" == 1 ]]; then echo "QUANTUM gap   $1"; else echo "QUANTUM FAIL  $1"; FAILED=1; fi; }

# What the observer must have seen: every block PINNED_QUANTUM frames, the rate
# PINNED_RATE throughout, and no buffer-size or rate callback announcing
# anything else.
judge() {
  python3 - "$1" "$PINNED_QUANTUM" "$PINNED_RATE" <<'PY'
import json, sys
r = json.loads(open(sys.argv[1]).read())
q, rate = int(sys.argv[2]), int(sys.argv[3])
problems = []
if "error" in r:
    problems.append(r["error"])
else:
    if r["process_calls"] == 0:
        problems.append("never scheduled")
    if set(r["nframes_seen"]) != {str(q)}:
        problems.append("blocks of %s frames" % ",".join(r["nframes_seen"]))
    if any(b != q for b in r["bufsize_callbacks"]):
        problems.append("buffer-size callbacks %s" % r["bufsize_callbacks"])
    if r["buffer_size_at_start"] != q or r["buffer_size_at_end"] != q:
        problems.append("buffer size %d -> %d" % (r["buffer_size_at_start"], r["buffer_size_at_end"]))
    if r["rate_at_start"] != rate or r["rate_at_end"] != rate or any(x != rate for x in r["rate_callbacks"]):
        problems.append("rate %d -> %d, callbacks %s" % (r["rate_at_start"], r["rate_at_end"], r["rate_callbacks"]))
print("; ".join(problems))
sys.exit(1 if problems else 0)
PY
}

# The graph's own view, as an operator would look at it. Secondary to the
# observer's blocks, which are what the engine would be handed.
driver_quantum() {
  pw-top -b -n 2 2>/dev/null | awk '$1 == "R" && $NF == "Dummy-Driver" {q = $3 "/" $4} END {print q}'
}

# scenario LABEL SETUP TEARDOWN [ENV=VALUE ...] [-- PROBE_ARGS...]
# SETUP and TEARDOWN are shell snippets run around the misbehaving client.
scenario() {
  local label="$1" setup="$2" teardown="$3"
  shift 3
  local envs=() probe_args=()
  while [[ $# -gt 0 && "$1" != -- ]]; do envs+=("$1"); shift; done
  [[ "${1:-}" == -- ]] && shift
  probe_args=("$@")

  python3 "$PROBE" --name observer --seconds 5 > /tmp/observer.json &
  local obs=$!
  sleep 1
  eval "$setup"
  env "${envs[@]}" python3 "$PROBE" --name intruder --seconds 2 "${probe_args[@]}" > /tmp/intruder.json || true
  local during
  during="$(driver_quantum)"
  eval "$teardown"
  wait "$obs" || true

  local why
  if why="$(judge /tmp/observer.json)"; then
    ok "$label: observer saw only ${PINNED_QUANTUM}-frame blocks at ${PINNED_RATE} Hz"
  else
    bad "$label: $why"
  fi
  echo "QUANTUM info    observer: $(python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); print({k: r.get(k) for k in ("process_calls","nframes_seen","bufsize_callbacks","rate_at_end")})' /tmp/observer.json)"
  echo "QUANTUM info    intruder: $(python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); print({k: r.get(k) for k in ("nframes_seen","buffer_size_at_end","rate_at_end","set_buffer_size_rc","error")})' /tmp/intruder.json 2>/dev/null || echo none)"
  echo "QUANTUM info    driver quantum per pw-top while it ran: ${during:-unknown}"
}

RESET_FORCE="pw-metadata -n settings 0 clock.force-quantum 0 >/dev/null; pw-metadata -n settings 0 clock.force-rate 0 >/dev/null"

scenario "nothing asks (baseline)" : :
scenario "a JACK client asking for 64 frames (PIPEWIRE_LATENCY=64/48000)" : : \
  PIPEWIRE_LATENCY=64/48000
scenario "a JACK client calling jack_set_buffer_size(64)" : "$RESET_FORCE" \
  -- --set-buffer-size 64
scenario "a JACK client calling jack_set_buffer_size(256)" : "$RESET_FORCE" \
  -- --set-buffer-size 256
scenario "a global-buffer-size JACK client calling jack_set_buffer_size(64)" : "$RESET_FORCE" \
  'PIPEWIRE_PROPS={ jack.global-buffer-size = true }' -- --set-buffer-size 64
scenario "a JACK client forcing 64 frames (PIPEWIRE_QUANTUM=64/48000)" : : \
  PIPEWIRE_QUANTUM=64/48000
scenario "a JACK client forcing 256 frames (PIPEWIRE_QUANTUM=256/48000)" : : \
  PIPEWIRE_QUANTUM=256/48000
scenario "pw-metadata clock.force-quantum 64" \
  "pw-metadata -n settings 0 clock.force-quantum 64 >/dev/null" "$RESET_FORCE"
scenario "pw-metadata clock.force-quantum 256" \
  "pw-metadata -n settings 0 clock.force-quantum 256 >/dev/null" "$RESET_FORCE"
scenario "pw-metadata clock.min-quantum 64 and clock.quantum 64" \
  "pw-metadata -n settings 0 clock.min-quantum 64 >/dev/null; pw-metadata -n settings 0 clock.quantum 64 >/dev/null" \
  "pw-metadata -n settings 0 clock.min-quantum 0 >/dev/null; pw-metadata -n settings 0 clock.quantum 0 >/dev/null" \
  PIPEWIRE_LATENCY=64/48000
scenario "pw-metadata clock.max-quantum 64" \
  "pw-metadata -n settings 0 clock.max-quantum 64 >/dev/null" \
  "pw-metadata -n settings 0 clock.max-quantum 0 >/dev/null" \
  PIPEWIRE_LATENCY=64/48000
scenario "a JACK client asking for 96 kHz (PIPEWIRE_RATE=1/96000)" : : \
  PIPEWIRE_RATE=1/96000
scenario "a JACK client forcing 96 kHz (PIPEWIRE_QUANTUM=128/96000)" : : \
  PIPEWIRE_QUANTUM=128/96000
scenario "a JACK client forcing its own node.rate (node.force-rate=0, PIPEWIRE_RATE=1/96000)" : : \
  'PIPEWIRE_PROPS={ node.force-rate = 0 }' PIPEWIRE_RATE=1/96000
scenario "a JACK client calling jack_set_sample_rate(96000)" : "$RESET_FORCE" \
  -- --set-sample-rate 96000
scenario "pw-metadata clock.force-rate 96000" \
  "pw-metadata -n settings 0 clock.force-rate 96000 >/dev/null" "$RESET_FORCE"
scenario "pw-metadata clock.rate 96000" \
  "pw-metadata -n settings 0 clock.rate 96000 >/dev/null" \
  "pw-metadata -n settings 0 clock.rate 0 >/dev/null" \
  PIPEWIRE_RATE=1/96000

# Known gaps, measured and reported but not failing the run. The settings
# metadata keys that define the limits (clock.max-quantum, clock.allowed-rates)
# are not checked by PipeWire 1.4.2, so a client that first widens a limit and
# then forces a value inside it gets through. No JACK API call does this; it is
# a deliberate two-step `pw-metadata` edit. A pass here would mean the gap has
# closed and the note in brain/research/quantum-pin.md is out of date.
GAP=1
scenario "pw-metadata clock.max-quantum 256, then clock.force-quantum 256" \
  "pw-metadata -n settings 0 clock.max-quantum 256 >/dev/null; pw-metadata -n settings 0 clock.force-quantum 256 >/dev/null" \
  "$RESET_FORCE; pw-metadata -n settings 0 clock.max-quantum 0 >/dev/null"
scenario "pw-metadata clock.allowed-rates [48000 96000], then clock.force-rate 96000" \
  "pw-metadata -n settings 0 clock.allowed-rates '[ 48000 96000 ]' >/dev/null; pw-metadata -n settings 0 clock.force-rate 96000 >/dev/null" \
  "$RESET_FORCE; pw-metadata -n settings 0 clock.allowed-rates '' >/dev/null"
GAP=0

echo
if [[ "$FAILED" == 0 ]]; then
  echo "QUANTUM RESULT pass"
else
  echo "QUANTUM RESULT fail"
  exit 1
fi
