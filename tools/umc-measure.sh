#!/usr/bin/env bash
# One command, the evidence for drivers.umc1820.channels / .latency / .ipc.
# Runs inside Debian WSL2 (card passed through with usbipd-win) or on the ISO.
#
#   tools/umc-measure.sh [--seconds N] [--device NAME] [--out DIR] [--sessions DIR]
#
# 1. channels: the ALSA card reports 18 capture / 20 playback channels at 44.1
#    and 48 kHz, JACK (pw-jack) lists 18 distinct capture ports on one node, and
#    the engine records 18 tracks into 18 files none of them short.
# 2. latency: quantum 1024..64, the engine records the same time at each; the
#    lowest quantum with zero xruns and zero overruns is the answer.
# 3. ipc: a 64-byte SOCK_SEQPACKET round trip (tools/umc-ipc-probe.py) while the
#    engine records 18 channels at quantum 128 (ADR-0007 open question 1).
#    The shared-memory plugin round trip (question 2) needs the plugin host and
#    is NOT measured here.
#
# Under WSL the card sits behind USB/IP: latency and IPC are NOT bare-metal
# evidence and the report says so. Only the channel result stands.
# Exit status: 0 if the channel checks pass, 1 if not, 2 on setup errors.
#
# Test hooks: SONORA_ENGINE (engine binary), SONORA_UMC_FORCE_WSL=1.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SECONDS_PER_RUN=10
DEVICE=""
OUT_DIR="$ROOT/brain/research"
SESSIONS="${TMPDIR:-/tmp}/umc-measure-sessions"
ENGINE="${SONORA_ENGINE:-sonora-engine}"
WANT=18
WANT_PLAY=20
RATE=48000
QUANTUMS=(1024 512 256 128 64)

while [ $# -gt 0 ]; do
  case "$1" in
    --seconds) SECONDS_PER_RUN="$2"; shift 2 ;;
    --device) DEVICE="$2"; shift 2 ;;
    --out) OUT_DIR="$2"; shift 2 ;;
    --sessions) SESSIONS="$2"; shift 2 ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

for c in arecord aplay pw-jack pw-metadata python3; do
  command -v "$c" >/dev/null || { echo "error: $c not found" >&2; exit 2; }
done
command -v "$ENGINE" >/dev/null || [ -x "$ENGINE" ] || { echo "error: engine '$ENGINE' not found (set SONORA_ENGINE)" >&2; exit 2; }

HOST_KERNEL="$(uname -r)"
HOST_NAME="$(hostname 2>/dev/null || echo unknown)"
HOST_DATE="$(date -u +%Y-%m-%dT%H:%MZ)"
UNDER_WSL=0
if [ -n "${SONORA_UMC_FORCE_WSL:-}" ]; then
  UNDER_WSL="$SONORA_UMC_FORCE_WSL"
elif grep -qi microsoft /proc/version 2>/dev/null; then
  UNDER_WSL=1
fi
USBIP=0
if compgen -G '/sys/devices/platform/vhci_hcd*' >/dev/null 2>&1 || { [ -r /proc/modules ] && grep -q '^vhci_hcd' /proc/modules; }; then USBIP=1; fi
[ "$UNDER_WSL" = 1 ] && USBIP=1
CPU="$(sed -n 's/^model name[^:]*: //p' /proc/cpuinfo 2>/dev/null | head -1)"

mkdir -p "$OUT_DIR" "$SESSIONS"
REPORT="$OUT_DIR/umc1820-measurements-$(date -u +%Y%m%d-%H%M).md"
FAILED=0
LINES=()
say() { echo "$*"; LINES+=("$*"); }
bad() { FAILED=1; say "- FAIL: $*"; }
ok() { say "- ok: $*"; }

BANNER=""
if [ "$UNDER_WSL" = 1 ]; then
  BANNER="!!! RUNNING UNDER WSL2 / USB-IP: LATENCY AND IPC FIGURES BELOW ARE NOT VALID BARE-METAL EVIDENCE (USB/IP adds latency). ONLY THE CHANNELS RESULT COUNTS. !!!"
  echo "$BANNER" >&2
fi

restore_quantum() {
  pw-metadata -n settings 0 clock.force-quantum 0 >/dev/null 2>&1
  pw-metadata -n settings 0 clock.force-rate 0 >/dev/null 2>&1
}
trap restore_quantum EXIT

say "# UMC1820 measurements $HOST_DATE"
say ""
if [ -n "$BANNER" ]; then
  say "> **$BANNER**"
  say ""
fi
say "## Host"
say ""
say "- model: Behringer UMC1820 (as reported by ALSA below)"
say "- kernel: \`$HOST_KERNEL\` ($(uname -v))"
say "- host: $HOST_NAME, CPU: ${CPU:-unknown}"
say "- WSL: $([ "$UNDER_WSL" = 1 ] && echo yes || echo no); USB/IP: $([ "$USBIP" = 1 ] && echo yes || echo no); bare metal: $([ "$USBIP" = 1 ] && echo no || echo yes)"
say "- seconds per run: $SECONDS_PER_RUN, rate: $RATE"
say ""

# ---- 1. channels ---------------------------------------------------------
say "## 1. Channels (drivers.umc1820.channels)"
say ""
CARD_LINE="$(arecord -l 2>/dev/null | grep -i -E 'UMC|behringer' | head -1)"
if [ -z "$CARD_LINE" ]; then
  bad "no UMC/Behringer capture card in \`arecord -l\`; is the card attached (tools/umc-wsl.ps1) and the usb-audio driver loaded?"
else
  CARD_NO="$(sed -n 's/^card \([0-9]*\):.*/\1/p' <<<"$CARD_LINE")"
  ok "ALSA card: $CARD_LINE"
  [ -n "$DEVICE" ] || DEVICE="$(cat "/proc/asound/card${CARD_NO}/id" 2>/dev/null || echo UMC1820)"
  say "- engine --device: \`$DEVICE\`"

  # Highest number on the CHANNELS: line of the hw params dump.
  max_channels() {
    local out
    out="$("$1" -D "hw:$CARD_NO,0" --dump-hw-params -d 1 /dev/null 2>&1 </dev/null)"
    grep -E '^CHANNELS:' <<<"$out" | grep -o '[0-9]\+' | sort -n | tail -1
  }
  rate_ok() {
    "$1" -D "hw:$CARD_NO,0" -c "$3" -r "$2" -f S32_LE -d 1 /dev/null >/dev/null 2>&1 </dev/null
  }
  CAP="$(max_channels arecord)"
  PLAY="$(max_channels aplay)"
  if [ "${CAP:-0}" -ge "$WANT" ]; then ok "ALSA capture channels: $CAP"; else bad "ALSA capture channels: ${CAP:-unknown}, want $WANT"; fi
  if [ "${PLAY:-0}" -ge "$WANT_PLAY" ]; then ok "ALSA playback channels: $PLAY"; else bad "ALSA playback channels: ${PLAY:-unknown}, want $WANT_PLAY"; fi
  for r in 44100 48000; do
    if rate_ok arecord "$r" "$WANT"; then ok "capture $WANT ch at $r Hz opens"; else bad "capture $WANT ch at $r Hz did not open (a device held by PipeWire also lands here; see fuser -v /dev/snd/*)"; fi
    if rate_ok aplay "$r" "$WANT_PLAY"; then ok "playback $WANT_PLAY ch at $r Hz opens"; else bad "playback $WANT_PLAY ch at $r Hz did not open"; fi
  done

  PORTS="$(pw-jack jack_lsp 2>/dev/null | grep ':capture_AUX')"
  # shellcheck disable=SC2001
  NODE="$(sed 's/:capture_AUX[0-9]*$//' <<<"$PORTS" | grep -i -E "$DEVICE|UMC" | sort -u | head -1)"
  if [ -z "$NODE" ]; then
    bad "JACK lists no capture_AUX ports for the card; the Pro Audio profile is probably not active (the default profile is a stereo pair)"
  else
    ok "JACK node: $NODE"
    NPORT="$(grep -c -F "$NODE:capture_AUX" <<<"$PORTS")"
    UNIQ="$(grep -F "$NODE:capture_AUX" <<<"$PORTS" | sort -u | wc -l)"
    MISSING=""
    for ((i = 0; i < WANT; i++)); do
      grep -q -x -F "$NODE:capture_AUX$i" <<<"$PORTS" || MISSING="$MISSING AUX$i"
    done
    if [ "$UNIQ" -ge "$WANT" ] && [ -z "$MISSING" ]; then
      ok "JACK: $NPORT ports, $UNIQ distinct, AUX0..AUX17 all present on one node"
    else
      bad "JACK: $NPORT ports, $UNIQ distinct, missing:${MISSING:- none}"
    fi
  fi

  pw-metadata -n settings 0 clock.force-rate "$RATE" >/dev/null 2>&1
  CH_DIR="$SESSIONS/channels"
  rm -rf "$CH_DIR"
  CH_OUT="$(pw-jack "$ENGINE" --dir "$CH_DIR" --tracks "$WANT" --rate "$RATE" --arm-all --device "$DEVICE" --wait-for-inputs 10 --record "$SECONDS_PER_RUN" 2>"$SESSIONS/channels.err")"
  CH_RC=$?
  FILES="$(find "$CH_DIR" -name '*.wav' 2>/dev/null | wc -l)"
  # 24-bit mono: half the take's bytes is the floor for "not short".
  MIN=$((RATE * SECONDS_PER_RUN * 3 / 2))
  SHORT="$(find "$CH_DIR" -name '*.wav' -size -"${MIN}"c 2>/dev/null | wc -l)"
  if [ "$CH_RC" = 0 ]; then ok "engine recorded and exited 0"; else bad "engine exit status $CH_RC: $(tail -3 "$SESSIONS/channels.err" | tr '\n' ' ')"; fi
  if [ "$FILES" = "$WANT" ]; then ok "$FILES track files"; else bad "$FILES track files, want $WANT"; fi
  if [ "$SHORT" = 0 ] && [ "$FILES" -gt 0 ]; then ok "no track file shorter than half the take (>= $MIN bytes)"; else bad "$SHORT of $FILES files shorter than $MIN bytes"; fi
  say "- engine: \`$(grep '^RESULT' <<<"$CH_OUT" | tail -1)\`"
fi
say ""

# ---- 2. latency ----------------------------------------------------------
say "## 2. Latency: lowest zero-xrun quantum (drivers.umc1820.latency)"
say ""
if [ -n "$BANNER" ]; then
  say "> NOT bare-metal evidence (WSL2 / USB-IP)."
  say ""
fi
LOWEST=""
if [ -n "$CARD_LINE" ]; then
  say "| quantum | buffer seen | xruns | overruns | result |"
  say "|---|---|---|---|---|"
  for q in "${QUANTUMS[@]}"; do
    pw-metadata -n settings 0 clock.force-quantum "$q" >/dev/null 2>&1
    D="$SESSIONS/q$q"
    rm -rf "$D"
    OUT="$(pw-jack "$ENGINE" --dir "$D" --tracks "$WANT" --rate "$RATE" --arm-all --device "$DEVICE" --wait-for-inputs 10 --record "$SECONDS_PER_RUN" 2>/dev/null)"
    R="$(grep '^RESULT' <<<"$OUT" | tail -1)"
    BUF="$(sed -n 's/.* buffer=\([0-9]*\).*/\1/p' <<<"$R")"
    XR="$(sed -n 's/.* xruns=\([0-9]*\).*/\1/p' <<<"$R")"
    OV="$(sed -n 's/.* overruns=\([0-9]*\).*/\1/p' <<<"$R")"
    if [ -z "$R" ]; then VERDICT="no result"
    elif [ "$BUF" != "$q" ]; then VERDICT="quantum not honoured"
    elif [ "$XR" = 0 ] && [ "$OV" = 0 ]; then VERDICT="clean"; LOWEST="$q"
    else VERDICT="xruns"; fi
    say "| $q | ${BUF:--} | ${XR:--} | ${OV:--} | $VERDICT |"
  done
  restore_quantum
  say ""
  if [ -n "$LOWEST" ]; then
    say "Lowest clean quantum: **$LOWEST** frames ($(python3 -c "print(round($LOWEST/$RATE*1000,2))") ms per period at $RATE Hz), $SECONDS_PER_RUN s per run, 18 channels."
    say "This is the quantum only; the round-trip latency also includes the USB transport and is not derived here."
  else
    say "No quantum ran clean."
  fi
else
  say "Skipped: no card."
fi
say ""

# ---- 3. IPC --------------------------------------------------------------
say "## 3. IPC under load (drivers.umc1820.ipc, ADR-0007 question 1)"
say ""
if [ -n "$BANNER" ]; then
  say "> NOT bare-metal evidence (WSL2 / USB-IP)."
  say ""
fi
if [ -n "$CARD_LINE" ]; then
  pw-metadata -n settings 0 clock.force-quantum 128 >/dev/null 2>&1
  D="$SESSIONS/ipc"
  rm -rf "$D"
  pw-jack "$ENGINE" --dir "$D" --tracks "$WANT" --rate "$RATE" --arm-all --device "$DEVICE" --wait-for-inputs 10 --record "$SECONDS_PER_RUN" >"$SESSIONS/ipc.out" 2>/dev/null &
  ENGINE_PID=$!
  # Start after the engine has connected, stop before it finishes.
  sleep 3
  PROBE_S=$((SECONDS_PER_RUN > 6 ? SECONDS_PER_RUN - 5 : 1))
  PROBE="$(python3 "$ROOT/tools/umc-ipc-probe.py" --seconds "$PROBE_S" 2>&1)"
  wait "$ENGINE_PID"
  restore_quantum
  R="$(grep '^RESULT' "$SESSIONS/ipc.out" | tail -1)"
  XR="$(sed -n 's/.* xruns=\([0-9]*\).*/\1/p' <<<"$R")"
  say "- quantum 128, 18 channels recording, $SECONDS_PER_RUN s"
  say "- SOCK_SEQPACKET 64-byte round trip: \`$PROBE\`"
  say "- engine xruns during the run: ${XR:-unknown} (\`$R\`)"
  say "- ADR-0007 container reference: p50 60 us, p99 158 us, max 484 us"
  say "- Not measured here: shared-memory plugin round trip (question 2), StateView read cost under load."
else
  say "Skipped: no card."
fi
say ""
say "## Verdict"
say ""
if [ "$FAILED" = 0 ]; then say "Channels: PASS."; else say "Channels: FAIL (see the FAIL lines)."; fi

printf '%s\n' "${LINES[@]}" >"$REPORT"
echo "report: $REPORT"
exit "$FAILED"
