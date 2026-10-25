#!/usr/bin/env bash
# The real yabridge path (ADR-0017 Y5): Windows builds of the host's test
# plugin, installed in a fresh Wine prefix, bridged by `yabridgectl sync`
# into ~/.vst3/yabridge, then scanned, probed and run by sonora-plugin-host
# exactly as a native VST3 is. Exit 77 (skipped) when Wine, yabridge or the
# Windows builds are missing, which is CI's plugins image today;
# tools/test-yabridge.sh runs it in an image that has all three.
#
#   yabridge_wine.sh HOST DRIVER [NATIVE_BUNDLE]
#   SONORA_YABRIDGE_TEST_VST3_DIR  holds sonora-test-state.vst3 and
#                                  sonora-test-liar.vst3, Windows builds
#                                  (CMake makes them when mingw-w64 is found)
#
# Every figure it prints is a container figure (tools/build-plugins.sh).
set -euo pipefail

HOST="$1"
DRIVER="$2"
NATIVE="${3:-}"  # the Linux sonora-test.vst3, for the native round trip

skip() {
  echo "SKIP: $*"
  exit 77
}
fail() {
  echo "FAIL: $*"
  exit 1
}

command -v wine >/dev/null || skip "wine is not installed"
command -v wineserver >/dev/null || skip "wineserver is not installed"
command -v yabridgectl >/dev/null || skip "yabridgectl is not installed"
command -v yabridge-host.exe >/dev/null || skip "yabridge-host.exe is not on PATH"
WIN_DIR="${SONORA_YABRIDGE_TEST_VST3_DIR:-}"
[[ -n "$WIN_DIR" && -d "$WIN_DIR/sonora-test-state.vst3" && -d "$WIN_DIR/sonora-test-liar.vst3" ]] ||
  skip "no Windows builds of the test plugin (mingw-w64 not found)"
YABRIDGE_DIR="$(dirname "$(command -v yabridgectl)")"

WORK="$(mktemp -d)"
cleanup() {
  WINEPREFIX="$WORK/home/.wine" wineserver -k >/dev/null 2>&1 || true
  if [[ -n "${XVFB_PID:-}" ]]; then kill "$XVFB_PID" >/dev/null 2>&1 || true; fi
  rm -rf "$WORK"
}
trap cleanup EXIT

# A home of its own: ~/.vst3/yabridge, yabridgectl's config and the Wine
# prefix all start empty, and nothing of the caller's is scanned.
export HOME="$WORK/home"
export XDG_CONFIG_HOME="$HOME/.config" XDG_DATA_HOME="$HOME/.local/share"
export WINEPREFIX="$HOME/.wine" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
mkdir -p "$HOME"

# yabridge's Wine host opens an X11 connection even with no editor.
if [[ -z "${DISPLAY:-}" ]] && command -v Xvfb >/dev/null; then
  Xvfb :97 -nolisten tcp >/dev/null 2>&1 &
  XVFB_PID=$!
  export DISPLAY=:97
fi

ms() { echo $(( ($(date +%s%N) - $1) / 1000000 )); }

t=$(date +%s%N)
wineboot -i >"$WORK/wineboot.log" 2>&1 || fail "wineboot -i: $(tail -3 "$WORK/wineboot.log")"
wineserver -w
echo "wine: $(wine --version 2>/dev/null), prefix initialised in $(ms "$t") ms"

VST3_WIN="$WINEPREFIX/drive_c/Program Files/Common Files/VST3"
mkdir -p "$VST3_WIN"
cp -r "$WIN_DIR/sonora-test-state.vst3" "$WIN_DIR/sonora-test-liar.vst3" "$VST3_WIN/"

# Where yabridgectl looks by default. Not `yabridgectl set --path`: 5.1.1's
# `set` panics in its argument parser (clap, `path_auto`).
mkdir -p "$XDG_DATA_HOME"
ln -s "$YABRIDGE_DIR" "$XDG_DATA_HOME/yabridge"
yabridgectl add "$VST3_WIN" >"$WORK/yabridgectl.log" 2>&1 || fail "yabridgectl add: $(tail -5 "$WORK/yabridgectl.log")"
t=$(date +%s%N)
yabridgectl sync >>"$WORK/yabridgectl.log" 2>&1 || fail "yabridgectl sync: $(tail -5 "$WORK/yabridgectl.log")"
echo "yabridgectl sync: $(ms "$t") ms"
STATE="$HOME/.vst3/yabridge/sonora-test-state.vst3"
LIAR="$HOME/.vst3/yabridge/sonora-test-liar.vst3"
for b in "$STATE" "$LIAR"; do
  [[ -d "$b" ]] || fail "yabridgectl made no bundle at $b: $(tail -5 "$WORK/yabridgectl.log")"
done

# 1. Enumerate: the bridged bundles are found under ~/.vst3 and marked.
"$HOST" --list --json --bundles-only --format vst3 >"$WORK/bundles.json" 2>/dev/null
grep -q "\"bundle\":\"$STATE\",\"format\":\"vst3\",\"bridge\":\"yabridge\"" "$WORK/bundles.json" ||
  fail "--bundles-only did not mark $STATE as bridged: $(cat "$WORK/bundles.json")"
echo "ok 1 - --bundles-only marks the bundles bridged"

# 2. Describe: each bundle through Wine, in a scan child of its own. The
# Liar writes 8 channels into a 2-channel bus inside Wine, past what
# yabridge gave it, where this host's trap cannot see: whatever that does
# costs the Liar's own entry and nothing else.
t=$(date +%s%N)
"$HOST" --list --json --format vst3 --scan-timeout 30 >"$WORK/list.json" 2>"$WORK/list.err" ||
  fail "--list: $(tail -5 "$WORK/list.err")"
echo "--list through Wine: $(ms "$t") ms"
state_line="$(grep '"name":"Sonora Test State"' "$WORK/list.json" || true)"
[[ -n "$state_line" ]] || fail "--list did not describe Sonora Test State: $(head -c 3000 "$WORK/list.json")"
[[ "$state_line" == *"\"bundle\":\"$STATE\",\"bridge\":\"yabridge\""* ]] ||
  fail "the State class is not marked bridged: $state_line"
STATE_URI="$(sed -E 's/^\{"uri":"([^"]+)".*/\1/' <<<"$state_line")"
echo "ok 2 - --list describes $STATE_URI through yabridge"
echo "  the Liar's entry: $(grep -F "$LIAR" "$WORK/list.json" | head -1 | cut -c1-240)"

# 3. Probe: validated like any VST3, and found again through the bundle's
# moduleinfo.json.
t=$(date +%s%N)
"$HOST" --probe "$STATE_URI" --channels 1 >"$WORK/probe.json" 2>"$WORK/probe.err" || true
grep -q '"ok":true' "$WORK/probe.json" ||
  fail "--probe $STATE_URI: $(cat "$WORK/probe.json") $(tail -5 "$WORK/probe.err")"
echo "ok 3 - --probe validates it ($(ms "$t") ms)"

# 4. Run it on the plugin boundary at 128 frames / 48 kHz: the round trip
# now carries yabridge's own hop into Wine, measured against the deadline.
"$DRIVER" --host "$HOST" --bundle "$STATE" --scenario rtt --blocks "${SONORA_YABRIDGE_BLOCKS:-3000}" \
  --chain "$STATE_URI" >"$WORK/rtt.log" 2>&1 || fail "rtt: $(tail -15 "$WORK/rtt.log")"
grep -E '^rtt:|CONTAINER FIGURES|round trip us|dropped|driver:' "$WORK/rtt.log" | sed 's/^ */  /'
echo "ok 4 - runs through the plugin boundary"
# The same class built for Linux, the same run: the cost of the Wine hop.
if [[ -n "${NATIVE:-}" && -d "$NATIVE" ]]; then
  "$DRIVER" --host "$HOST" --bundle "$NATIVE" --scenario rtt --blocks "${SONORA_YABRIDGE_BLOCKS:-3000}" \
    --chain "$STATE_URI" >"$WORK/rtt-native.log" 2>&1 || fail "native rtt: $(tail -15 "$WORK/rtt-native.log")"
  echo "  native, for comparison:"
  grep -E 'round trip us|dropped' "$WORK/rtt-native.log" | sed 's/^ */  /'
fi

# 5. Validate before trusting: the Liar is never usable. Refused, or no
# answer within the engine's probe timeout (it then caches it Blocked);
# "ok":true is the failure.
LIAR_URI="vst3:534F4E4F524156535433544553540004"
t=$(date +%s%N)
rc=0
timeout 30 "$HOST" --probe "$LIAR_URI" --channels 2 >"$WORK/liar.json" 2>"$WORK/liar.err" || rc=$?
if grep -q '"ok":true' "$WORK/liar.json"; then
  fail "the Liar passed validation through yabridge: $(cat "$WORK/liar.json")"
fi
if grep -q '"ok":false' "$WORK/liar.json"; then
  echo "ok 5 - the Liar is refused: $(grep -o '"reason":"[^"]*"' "$WORK/liar.json") ($(ms "$t") ms)"
else
  echo "ok 5 - the Liar never answers its probe (exit $rc after $(ms "$t") ms): Blocked"
fi

# 6. Isolation: a Windows plugin that crashes in its factory costs its own
# --list entry; the scan goes on.
SONORA_TEST_VST3_CRASH_ON_SCAN=1 "$HOST" --list --json --format vst3 --scan-timeout 30 \
  >"$WORK/crash.json" 2>/dev/null || fail "--list exited non-zero after a crashing Windows plugin"
crash_line="$(grep -F "\"bundle\":\"$STATE\",\"format\":\"vst3\",\"usable\":false" "$WORK/crash.json" || true)"
[[ -n "$crash_line" ]] ||
  fail "a crashing Windows plugin was not reported unusable: $(grep -F "$STATE" "$WORK/crash.json" | cut -c1-300)"
echo "ok 6 - a crashing Windows plugin costs its own entry: $(grep -o '"reason":"[^"]*"' <<<"$crash_line")"

echo "PASS: yabridge"
