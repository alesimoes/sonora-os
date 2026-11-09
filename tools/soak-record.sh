#!/usr/bin/env bash
# The 24-hour soak of ADR-0108: one continuous 18-track take, watched every
# 60 s, failed at once on the first xrun or overrun, judged at the end.
#
#   tools/soak-record.sh --buffer N [--seconds S] [--out DIR] [--sessions DIR]
#                        [--device NAME] [--backend alsa|jack|fake] [--rate R]
#                        [--interval S] [--tracks N]
#   tools/soak-record.sh --self-test
#
# Run it on the installed system with memlock on, never on the live stick.
# --buffer is the quantum: alsa gets --alsa-period N, jack a forced quantum
# through pw-metadata. Until drivers.umc1820.latency reports, the operator
# picks it and the report records it.
#
# Exit status: 0 PASS, 1 FAIL, 2 setup refused (nothing was started).
# --self-test: ~25 s on the fake backend, proves a PASS and an injected-xrun
# FAIL. It skips the hardware preflight (governor, limits, card channels) and
# says so; it is the only part CI may run.
#
# Test hooks: SONORA_ENGINE (engine binary), SONORA_VERIFY (verify script),
# SONORA_XRUN_CMD (prints the xrun count of the control state; none is read
# when unset and the report says so), SONORA_JOURNAL_CMD (prints journal
# error lines since the start), SONORA_SOAK_FREE_KB (free space override),
# SONORA_CAPTURE_CHANNELS (channel count override).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENGINE="${SONORA_ENGINE:-sonora-engine}"
VERIFY_CMD=(python3 -I "$ROOT/tools/verify-recording.py")
[ -z "${SONORA_VERIFY:-}" ] || VERIFY_CMD=("$SONORA_VERIFY")
SECONDS_TOTAL=86400
BUFFER=""
OUT_DIR="$ROOT/brain/research"
SESSIONS="${TMPDIR:-/tmp}/soak-sessions"
DEVICE=""
BACKEND="alsa"
RATE=48000
TRACKS=18
INTERVAL=60
BYTES=3
SELF_TEST=0
PREFLIGHT_HW=1

usage() { sed -n '2,22p' "$0"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --buffer) BUFFER="$2"; shift 2 ;;
    --seconds) SECONDS_TOTAL="$2"; shift 2 ;;
    --out) OUT_DIR="$2"; shift 2 ;;
    --sessions) SESSIONS="$2"; shift 2 ;;
    --device) DEVICE="$2"; shift 2 ;;
    --backend) BACKEND="$2"; shift 2 ;;
    --rate) RATE="$2"; shift 2 ;;
    --tracks) TRACKS="$2"; shift 2 ;;
    --interval) INTERVAL="$2"; shift 2 ;;
    --self-test) SELF_TEST=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

refuse() { echo "REFUSED: $*" >&2; exit 2; }

# ---- self-test ------------------------------------------------------------
if [ "$SELF_TEST" = 1 ]; then
  T="$(mktemp -d)"
  trap 'rm -rf "$T"' EXIT
  fails=0
  common=(--backend fake --buffer 128 --seconds 10 --interval 2 --out "$T/out" --sessions "$T/s")
  echo "self-test 1/2: clean run must PASS"
  SONORA_SOAK_NO_PREFLIGHT=1 "$0" "${common[@]}" >"$T/pass.log" 2>&1
  rc=$?
  if [ "$rc" != 0 ]; then echo "FAIL: clean run exited $rc, want 0" >&2; tail -15 "$T/pass.log" >&2; fails=1; fi
  echo "self-test 2/2: an injected xrun must FAIL at once"
  printf '#!/bin/sh\necho 1\n' >"$T/xr"; chmod +x "$T/xr"
  SONORA_SOAK_NO_PREFLIGHT=1 SONORA_XRUN_CMD="$T/xr" "$0" "${common[@]}" >"$T/fail.log" 2>&1
  rc=$?
  if [ "$rc" != 1 ]; then echo "FAIL: injected xrun exited $rc, want 1" >&2; tail -15 "$T/fail.log" >&2; fails=1; fi
  grep -q 'first xrun' "$T/fail.log" || { echo "FAIL: no early-abort line in the FAIL run" >&2; fails=1; }
  if [ "$fails" = 0 ]; then echo "self-test: ok"; else echo "self-test: FAILED" >&2; fi
  exit "$fails"
fi

[ -n "$BUFFER" ] || refuse "--buffer N is required (the quantum is chosen explicitly, never negotiated)"
case "$BUFFER$SECONDS_TOTAL$INTERVAL" in *[!0-9]*) refuse "--buffer, --seconds, --interval must be whole numbers" ;; esac
command -v "$ENGINE" >/dev/null || [ -x "$ENGINE" ] || refuse "engine '$ENGINE' not found (set SONORA_ENGINE)"
[ -n "${SONORA_VERIFY:-}" ] || command -v python3 >/dev/null || refuse "python3 not found"
[ -n "${SONORA_SOAK_NO_PREFLIGHT:-}" ] && PREFLIGHT_HW=0

# ---- 4. disk need, computed -------------------------------------------------
NEED_BYTES=$((TRACKS * RATE * BYTES * SECONDS_TOTAL))
NEED_WITH_MARGIN=$((NEED_BYTES + NEED_BYTES / 20))
gb() { awk -v b="$1" 'BEGIN {printf "%.1f", b/1e9}'; }
echo "disk need: $TRACKS ch x $RATE Hz x $BYTES B x ${SECONDS_TOTAL} s = $(gb "$NEED_BYTES") GB ($(gb "$NEED_WITH_MARGIN") GB with 5 % margin; 32-bit float would be $(gb $((NEED_BYTES * 4 / 3))) GB)"

# ---- 3. preflight: refuse before starting ------------------------------------
mkdir -p "$SESSIONS" "$OUT_DIR" || refuse "cannot create $SESSIONS or $OUT_DIR"
if [ -n "${SONORA_SOAK_FREE_KB:-}" ]; then FREE_KB="$SONORA_SOAK_FREE_KB"
else FREE_KB="$(df -Pk "$SESSIONS" | awk 'NR==2 {print $4}')"; fi
[ "${FREE_KB:-0}" -ge $((NEED_WITH_MARGIN / 1024)) ] || refuse "free space on the session volume is $((FREE_KB / 1024 / 1024)) GiB, need $(gb "$NEED_WITH_MARGIN") GB"

GOVERNOR="not checked"
LIMITS="not checked"
CHANNELS="not checked"
if [ "$PREFLIGHT_HW" = 1 ]; then
  GOVS="$(cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null | sort -u)"
  [ "$GOVS" = performance ] || refuse "CPU governor is '${GOVS:-unavailable}', want performance on every CPU"
  GOVERNOR=performance
  [ "$(ulimit -l)" = unlimited ] || refuse "memlock limit is $(ulimit -l), want unlimited (audio-stack limits)"
  RTP="$(ulimit -r)"
  { [ "$RTP" = unlimited ] || [ "$RTP" -ge 50 ]; } || refuse "RT priority limit is $RTP, want >= 50"
  LIMITS="memlock unlimited, rtprio $RTP"
  if [ -n "${SONORA_CAPTURE_CHANNELS:-}" ]; then CH="$SONORA_CAPTURE_CHANNELS"
  else
    CARD_NO="$(arecord -l 2>/dev/null | grep -i -E "${DEVICE:-UMC|behringer}" | sed -n 's/^card \([0-9]*\):.*/\1/p' | head -1)"
    [ -n "$CARD_NO" ] || refuse "no capture card found in arecord -l"
    CH="$(arecord -D "hw:$CARD_NO,0" --dump-hw-params -d 1 /dev/null 2>&1 </dev/null | grep -E '^CHANNELS:' | grep -o '[0-9]\+' | sort -n | tail -1)"
  fi
  [ "${CH:-0}" -ge "$TRACKS" ] || refuse "card reports ${CH:-0} capture channels, want $TRACKS"
  CHANNELS="$CH"
fi

# ---- 1. one continuous take ----------------------------------------------------
RUN_ID="$(date -u +%Y%m%d-%H%M%S)"
SDIR="$SESSIONS/soak-$RUN_ID"
CSV="$OUT_DIR/soak-$RUN_ID.csv"
REPORT="$OUT_DIR/soak-$RUN_ID.md"
ENGINE_OUT="$SDIR.engine.out"
ENGINE_ERR="$SDIR.engine.err"
echo "elapsed_s,engine_alive,free_kb,journal_errors,xruns" >"$CSV"

ARGS=(--backend "$BACKEND" --dir "$SDIR" --tracks "$TRACKS" --rate "$RATE" --arm-all --record "$SECONDS_TOTAL")
[ -n "$DEVICE" ] && [ "$BACKEND" != fake ] && ARGS+=(--device "$DEVICE")
[ "$BACKEND" = alsa ] && ARGS+=(--alsa-period "$BUFFER")
[ "$BACKEND" = fake ] && ARGS+=(--no-memlock)
[ "$BACKEND" = jack ] && pw-metadata -n settings 0 clock.force-quantum "$BUFFER" >/dev/null 2>&1
START_ISO="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
START_EPOCH="$(date +%s)"

"$ENGINE" "${ARGS[@]}" >"$ENGINE_OUT" 2>"$ENGINE_ERR" &
EPID=$!

trap 'kill "$EPID" 2>/dev/null' EXIT
echo "engine pid $EPID, ${SECONDS_TOTAL} s, buffer $BUFFER, sampling every ${INTERVAL} s -> $CSV"

# ---- 3. watch ------------------------------------------------------------------
ABORT=""
JOURNAL_SEEN=0
LAST_XR="not sampled"
sample() {
  local alive=0 free j xr
  kill -0 "$EPID" 2>/dev/null && alive=1
  free="$(df -Pk "$SESSIONS" | awk 'NR==2 {print $4}')"
  if [ -n "${SONORA_JOURNAL_CMD:-}" ]; then j="$($SONORA_JOURNAL_CMD 2>/dev/null | grep -c .)"
  elif command -v journalctl >/dev/null; then
    j="$(journalctl -k --since "@$START_EPOCH" --no-pager -q 2>/dev/null | grep -i -E 'xhci|snd_usb' | grep -ci -E 'error|fail|reset|disconnect|xrun')"
  else j=0; fi
  xr=""
  [ -n "${SONORA_XRUN_CMD:-}" ] && xr="$($SONORA_XRUN_CMD 2>/dev/null | head -1)"
  [ -n "$xr" ] && LAST_XR="$xr"
  echo "$(($(date +%s) - START_EPOCH)),$alive,$free,${j:-0},${xr:-na}" >>"$CSV"
  JOURNAL_SEEN=$((JOURNAL_SEEN + ${j:-0}))
  case "${xr:-0}" in 0|"") ;; *[!0-9]*) ;; *) ABORT="first xrun/overrun seen in the control state: xruns=$xr"; return ;; esac
  [ "$alive" = 1 ] || return 0
}
while kill -0 "$EPID" 2>/dev/null; do
  # Sleep in short steps so the engine's own exit is noticed promptly.
  waited=0
  while [ "$waited" -lt "$INTERVAL" ] && kill -0 "$EPID" 2>/dev/null; do sleep 1; waited=$((waited + 1)); done
  sample
  if [ -n "$ABORT" ]; then
    echo "FAIL: $ABORT; stopping the engine, files kept in $SDIR"
    kill "$EPID" 2>/dev/null
    break
  fi
done
wait "$EPID" 2>/dev/null
ENGINE_RC=$?
trap - EXIT
[ -n "$ABORT" ] && ENGINE_RC=1

# ---- 2. judge ---------------------------------------------------------------------
RESULT="$(grep '^RESULT' "$ENGINE_OUT" | tail -1)"
field() { sed -n "s/.* $1=\\([0-9.]*\\).*/\\1/p" <<<"$RESULT"; }
XR="$(field xruns)"; OV="$(field overruns)"; DR="$(field dropped)"; MS="$(field misses)"
FR="$(field frames)"; STEPS="$(field clock_steps)"; BUF_SEEN="$(field buffer)"
CRITERIA=()
FAILED=0
crit() { # name ok detail
  if [ "$2" = 1 ]; then CRITERIA+=("- PASS: $1 ($3)"); else CRITERIA+=("- FAIL: $1 ($3)"); FAILED=1; fi
}
[ -z "$ABORT" ] || crit "no xrun or overrun during the run" 0 "$ABORT"
crit "RESULT line printed" "$([ -n "$RESULT" ] && echo 1 || echo 0)" "${RESULT:-none; the engine died or was stopped}"
crit "xruns=0" "$([ "${XR:-x}" = 0 ] && echo 1 || echo 0)" "xruns=${XR:-?}"
crit "overruns=0" "$([ "${OV:-x}" = 0 ] && echo 1 || echo 0)" "overruns=${OV:-?}"
crit "dropped=0" "$([ "${DR:-x}" = 0 ] && echo 1 || echo 0)" "dropped=${DR:-?}"
crit "misses=0" "$([ "${MS:-x}" = 0 ] && echo 1 || echo 0)" "misses=${MS:-?}"
crit "engine exit status 0, one PID, no restart" "$([ "$ENGINE_RC" = 0 ] && echo 1 || echo 0)" "pid $EPID, exit $ENGINE_RC"
crit "no xhci/snd_usb error in the journal" "$([ "$JOURNAL_SEEN" = 0 ] && echo 1 || echo 0)" "$JOURNAL_SEEN line(s)"

# All 18 files, not only the non-empty ones the engine compares.
mapfile -t WAVS < <(find "$SDIR" -name '*.wav' 2>/dev/null | sort)
AUDIO_DIR=""
[ "${#WAVS[@]}" -gt 0 ] && AUDIO_DIR="$(dirname "${WAVS[0]}")"
SIZES="$(for f in "${WAVS[@]}"; do stat -c %s "$f"; done 2>/dev/null | sort -u | wc -l)"
crit "$TRACKS files on disk" "$([ "${#WAVS[@]}" = "$TRACKS" ] && echo 1 || echo 0)" "${#WAVS[@]} found"
crit "all files the same size" "$([ "$SIZES" = 1 ] && echo 1 || echo 0)" "$SIZES distinct size(s)"
if [ -n "$AUDIO_DIR" ] && [ -n "$FR" ] && [ "$FR" -gt 0 ]; then
  VSEC="$(awk -v f="$FR" -v r="$RATE" 'BEGIN {printf "%.6f", f/r}')"
  VOUT="$("${VERIFY_CMD[@]}" --check "$AUDIO_DIR" --tracks "$TRACKS" --rate "$RATE" --seconds "$VSEC" 2>&1)"
  VRC=$?
  crit "verify-recording.py passes (frames=$FR)" "$([ "$VRC" = 0 ] && echo 1 || echo 0)" "exit $VRC: $(tail -2 <<<"$VOUT" | tr '\n' ' ')"
else
  crit "verify-recording.py passes" 0 "no files or no frames to check"
fi

# ---- 5. report -------------------------------------------------------------------------
if [ "$FAILED" = 0 ]; then VERDICT=PASS; RC=0; else VERDICT=FAIL; RC=1; fi
{
  echo "# Soak $RUN_ID: $VERDICT"
  echo
  echo "- host: $(hostname 2>/dev/null || echo unknown), kernel \`$(uname -r)\`, CPU: $(sed -n 's/^model name[^:]*: //p' /proc/cpuinfo 2>/dev/null | head -1)"
  echo "- start: $START_ISO, planned ${SECONDS_TOTAL} s, ran $(($(date +%s) - START_EPOCH)) s"
  echo "- backend: $BACKEND${DEVICE:+, device $DEVICE}, $TRACKS tracks, $RATE Hz, 24-bit"
  echo "- buffer requested: $BUFFER, seen by the engine: ${BUF_SEEN:-unknown}"
  echo "- disk need: $(gb "$NEED_BYTES") GB (free before: $((FREE_KB / 1024 / 1024)) GiB)"
  echo "- preflight: governor $GOVERNOR; limits $LIMITS; capture channels $CHANNELS"
  echo "- clock_steps: ${STEPS:-unknown} (reported, not failed)"
  echo "- samples CSV: \`$CSV\`"
  echo "- session: \`$SDIR\`"
  echo
  echo "## Criteria"
  echo
  printf '%s\n' "${CRITERIA[@]}"
  echo
  echo "RESULT: \`${RESULT:-none}\`"
  echo
  echo "## Not measured"
  echo
  [ "$PREFLIGHT_HW" = 1 ] || echo "- the hardware preflight was skipped (self-test)"
  [ -n "${SONORA_XRUN_CMD:-}" ] || echo "- the xrun count of the control state was not sampled (no SONORA_XRUN_CMD); xruns come from the engine's RESULT line at the end, so an early abort on an xrun needs the hook"
  echo "- last control-state xrun sample: $LAST_XR"
  [ "$BACKEND" != fake ] || echo "- a fake backend is not the UMC1820: no USB, no card clock; this is not hardware evidence"
  echo "- RF64 beyond 4 GB per track was not exercised unless the run lasted past about 6 h"
  echo "- the UMC1820 itself, unless the host line above names it and the backend is alsa or jack"
} >"$REPORT"
echo "$VERDICT; report: $REPORT"
exit "$RC"
