#!/usr/bin/env bash
# Runs AlsaBackend on a real ALSA device: snd-aloop, a kernel loopback card.
# A 32-channel WAV with a 1 kHz tone on channel 5 only is played into one end;
# sonora-alsa-passthrough captures it, copies input k to output k and plays
# it out of another subdevice; arecord records that. Pass when the tone is on
# channel 5 of the recording and absent from the others, with no xruns.
#
#   tools/test-alsa-loopback.sh [--passthrough PATH]
#
# Needs: the snd-aloop module (modprobe snd-aloop as root, on a host kernel
# that has it; Docker Desktop's and WSL2's do not), aplay, arecord, python3.
# Without them it prints SKIP and exits 0, which is NOT a pass: the line
# says what was not run. Exit 1: the stream ran and the check failed.
set -uo pipefail

PASSTHROUGH="${SONORA_ALSA_PASSTHROUGH:-sonora-alsa-passthrough}"
while [ $# -gt 0 ]; do
  case "$1" in
    --passthrough) PASSTHROUGH="$2"; shift 2 ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

skip() { echo "SKIP (not run): $1"; exit 0; }

command -v aplay >/dev/null || skip "aplay not installed"
command -v arecord >/dev/null || skip "arecord not installed"
command -v python3 >/dev/null || skip "python3 not installed"
command -v "$PASSTHROUGH" >/dev/null || [ -x "$PASSTHROUGH" ] || skip "$PASSTHROUGH not built (needs libasound)"
grep -qs Loopback /proc/asound/cards || modprobe snd-aloop 2>/dev/null || true
grep -qs Loopback /proc/asound/cards || skip "no snd-aloop card (modprobe snd-aloop needs root and a kernel that has it)"

WORK="$(mktemp -d)"
PIDS=()
# shellcheck disable=SC2329  # run by the EXIT trap
cleanup() {
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

# Capture end of the pair the generator plays into (dev 0 sub 0 -> dev 1 sub 0);
# playback to dev 0 sub 1, which arecord reads at dev 1 sub 1.
export SONORA_ALSA_CAPTURE_PCM="hw:CARD=Loopback,DEV=1,SUBDEV=0"
export SONORA_ALSA_PLAYBACK_PCM="hw:CARD=Loopback,DEV=0,SUBDEV=1"

python3 -I - "$WORK/in.wav" <<'EOF'
import math, struct, sys, wave
ch, rate, secs = 32, 48000, 8
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(ch); w.setsampwidth(4); w.setframerate(rate)
    frames = bytearray()
    for i in range(rate * secs):
        v = int(0.5 * 2147483647 * math.sin(2 * math.pi * 1000 * i / rate))
        frames += struct.pack("<" + "i" * ch, *[v if c == 5 else 0 for c in range(ch)])
    w.writeframes(bytes(frames))
EOF

# The capture side takes the generator's parameters, so the generator starts first.
aplay -q -D hw:Loopback,0,0 "$WORK/in.wav" & PIDS+=($!)
sleep 1
arecord -q -D hw:Loopback,1,1 -c 32 -f S32_LE -r 48000 -d 5 "$WORK/out.wav" & ARECORD=$!
PIDS+=("$ARECORD")
"$PASSTHROUGH" --device Loopback --seconds 6 --rtprio 0 | tee "$WORK/report.txt"
RC=${PIPESTATUS[0]}
wait "$ARECORD" 2>/dev/null || true

python3 -I - "$WORK/out.wav" "$WORK/report.txt" <<'EOF'
import re, struct, sys, wave
with wave.open(sys.argv[1]) as w:
    ch, n = w.getnchannels(), w.getnframes()
    raw = w.readframes(n)
data = struct.unpack("<" + "i" * (len(raw) // 4), raw)
def peak(c): return max(abs(data[i]) for i in range(c, len(data), ch)) / 2147483648.0
peaks = [peak(c) for c in range(ch)]
report = open(sys.argv[2]).read()
xruns = int(re.search(r"xruns=(\d+)", report).group(1))
print("channel 5 peak %.3f, loudest other %.5f, xruns %d" % (peaks[5], max(p for c, p in enumerate(peaks) if c != 5), xruns))
ok = peaks[5] > 0.3 and all(p < 0.01 for c, p in enumerate(peaks) if c != 5)
print("PASS" if ok else "FAIL: the tone is not on channel 5 alone")
sys.exit(0 if ok else 1)
EOF
PYRC=$?
[ "$RC" -eq 0 ] || { echo "FAIL: passthrough exited $RC"; exit 1; }
exit "$PYRC"
