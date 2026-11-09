#!/usr/bin/env bash
# Appliance sign-off for bridged Windows plugins (ADR-0017 Y6). Run on the
# installed appliance (UMC1820) with a real Windows VST3:
#
#   signoff-yabridge.sh --host HOST --driver DRIVER --plugin PATH.vst3 \
#       [--blocks N] [--report FILE]
#   SONORA_YABRIDGE_TEST_VST3_DIR  holds sonora-test-crash.vst3 (Windows build)
#
# Scans and probes the plugin, runs plugin_driver's rtt scenario at the real
# period, samples Wine's thread priorities, runs crash_bridged, and writes a
# report. Exit 0 only when every criterion holds, 1 when one fails, 77 when
# Wine, yabridge or the plugin is absent. It uses the caller's real
# ~/.vst3/yabridge: this is the appliance, not a test prefix.
set -euo pipefail

here="$(cd "${BASH_SOURCE[0]%/*}" && pwd)"
# shellcheck source=tools/lib/yabridge-signoff.sh
source "$here/lib/yabridge-signoff.sh"

skip() {
  echo "SKIP: $*"
  exit 77
}
fail() {
  echo "FAIL: $*"
  exit 1
}

HOST="" DRIVER="" PLUGIN="" BLOCKS=20000 REPORT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --host) HOST="$2"; shift 2 ;;
    --driver) DRIVER="$2"; shift 2 ;;
    --plugin) PLUGIN="$2"; shift 2 ;;
    --blocks) BLOCKS="$2"; shift 2 ;;
    --report) REPORT="$2"; shift 2 ;;
    *)
      echo "usage: $0 --host HOST --driver DRIVER --plugin PATH.vst3 [--blocks N] [--report FILE]"
      exit 2
      ;;
  esac
done

missing=()
for c in wine wineserver yabridgectl yabridge-host.exe; do
  command -v "$c" >/dev/null || missing+=("$c")
done
((${#missing[@]} == 0)) || skip "not installed or not on PATH: ${missing[*]}"
[[ -n "$PLUGIN" && -e "$PLUGIN" ]] || skip "no Windows plugin at '${PLUGIN}' (--plugin PATH.vst3)"
[[ -x "$HOST" && -x "$DRIVER" ]] || skip "sonora-plugin-host or plugin_driver not given or not executable"
CRASH_SRC="${SONORA_YABRIDGE_TEST_VST3_DIR:-}/sonora-test-crash.vst3"
[[ -d "$CRASH_SRC" ]] || skip "no Windows build of the crash test plugin (SONORA_YABRIDGE_TEST_VST3_DIR)"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
REPORT="${REPORT:-$WORK/report.txt}"

# The crash plugin goes beside the real one so yabridgectl bridges both.
PLUGIN_DIR="$(cd "$(dirname "$PLUGIN")" && pwd)"
[[ -d "$PLUGIN_DIR/$(basename "$CRASH_SRC")" ]] || cp -r "$CRASH_SRC" "$PLUGIN_DIR/"
yabridgectl add "$PLUGIN_DIR" >"$WORK/yabridgectl.log" 2>&1 || true
yabridgectl sync >>"$WORK/yabridgectl.log" 2>&1 || fail "yabridgectl sync: $(tail -5 "$WORK/yabridgectl.log")"
BRIDGED="$HOME/.vst3/yabridge/$(basename "$PLUGIN")"
CRASH="$HOME/.vst3/yabridge/$(basename "$CRASH_SRC")"
[[ -d "$BRIDGED" && -d "$CRASH" ]] || fail "yabridgectl made no bundle at $BRIDGED or $CRASH"

"$HOST" --list --json --format vst3 --scan-timeout 30 >"$WORK/list.json" 2>"$WORK/list.err" ||
  fail "--list: $(tail -5 "$WORK/list.err")"
line="$(grep -F "\"bundle\":\"$BRIDGED\"" "$WORK/list.json" | head -1 || true)"
[[ -n "$line" ]] || fail "--list did not describe $BRIDGED"
URI="$(sed -E 's/^\{"uri":"([^"]+)".*/\1/' <<<"$line")"
"$HOST" --probe "$URI" --channels 2 >"$WORK/probe.json" 2>"$WORK/probe.err" || true
grep -q '"ok":true' "$WORK/probe.json" || fail "--probe $URI: $(head -c 600 "$WORK/probe.json")"
CRASH_URI="vst3:534F4E4F524156535433544553540001"

# The run is backgrounded here so Wine's threads can be sampled while they
# exist; they are gone when it ends.
: >"$WORK/ps.txt"
"$DRIVER" --host "$HOST" --bundle "$BRIDGED" --scenario rtt --blocks "$BLOCKS" --channels 2 \
  --chain "$URI" >"$WORK/rtt.log" 2>&1 &
drv=$!
rtt_rc=0
while kill -0 "$drv" 2>/dev/null; do
  ps -eLo cls,rtprio,comm >>"$WORK/ps.txt" 2>/dev/null || true
  sleep 1
done
wait "$drv" || rtt_rc=$?
[[ "$rtt_rc" == 0 ]] || fail "rtt: $(tail -15 "$WORK/rtt.log")"
read -r P50 P99 P999 MAX DROPPED < <(yb_parse_rtt "$WORK/rtt.log") ||
  fail "could not read the rtt figures: $(tail -8 "$WORK/rtt.log")"

crash_rc=0
"$DRIVER" --host "$HOST" --bundle "$BRIDGED" --bundle "$CRASH" --scenario crash_bridged \
  --chain "$URI,$CRASH_URI" >"$WORK/crash.log" 2>&1 || crash_rc=$?

rc=0
{
  echo "plugin: $PLUGIN ($URI), $BLOCKS blocks, kernel $(uname -r)"
  yb_evaluate "$P50" "$P99" "$P999" "$MAX" "$DROPPED" "$WORK/ps.txt" "$WORK/crash.log" "$crash_rc"
} >"$REPORT" || rc=$?
cat "$REPORT"
exit "$rc"
