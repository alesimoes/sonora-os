#!/usr/bin/env bash
# Logic test for tools/umc-measure.sh against fake arecord/aplay/pw-jack/
# pw-metadata/engine on PATH. It proves the script's decisions (what counts as
# a pass, which quantum is "lowest clean", the WSL banner), not any hardware:
# the real card has not been measured by this test.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin"
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

cat >"$T/bin/arecord" <<'EOF'
#!/usr/bin/env bash
if [ "$1" = -l ]; then
  [ "${FAKE_NOCARD:-0}" = 1 ] || echo "card 1: UMC1820 [UMC1820], device 0: USB Audio [USB Audio]"
  exit 0
fi
case "$*" in *--dump-hw-params*) echo "CHANNELS: [1 ${FAKE_CAP:-18}]"; exit 0 ;; esac
exit 0
EOF
cat >"$T/bin/aplay" <<'EOF'
#!/usr/bin/env bash
case "$*" in *--dump-hw-params*) echo "CHANNELS: [1 ${FAKE_PLAY:-20}]"; exit 0 ;; esac
exit 0
EOF
cat >"$T/bin/pw-metadata" <<'EOF'
#!/usr/bin/env bash
[ "$4" = clock.force-quantum ] && echo "$5" >"$FAKE_DIR/quantum"
exit 0
EOF
cat >"$T/bin/pw-jack" <<'EOF'
#!/usr/bin/env bash
if [ "$1" = jack_lsp ]; then
  for ((i = 0; i < ${FAKE_PORTS:-18}; i++)); do echo "alsa_input.usb-Behringer_UMC1820.pro-input-0:capture_AUX$i"; done
  exit 0
fi
exec "$@"
EOF
cat >"$T/bin/fake-engine" <<'EOF'
#!/usr/bin/env bash
while [ $# -gt 0 ]; do
  case "$1" in --dir) dir="$2"; shift ;; --tracks) n="$2"; shift ;; --record) sec="$2"; shift ;; esac
  shift
done
q="$(cat "$FAKE_DIR/quantum" 2>/dev/null || echo 128)"
mkdir -p "$dir"
for ((i = 0; i < n; i++)); do head -c $((48000 * sec * 3)) /dev/zero >"$dir/t$i.wav"; done
[ -n "${FAKE_SHORT:-}" ] && head -c 100 /dev/zero >"$dir/t0.wav"
x=0
[ "$q" -lt "${FAKE_XRUN_BELOW:-256}" ] && x=3
echo "RESULT files=$n connected=$n frames=1 requested=1 rate=48000 buffer=$q xruns=$x clock_steps=0 overruns=0"
EOF
chmod +x "$T"/bin/*

run() { # NAME ENV... ; prints report path in $T/out.NAME
  local name="$1"
  shift
  env PATH="$T/bin:$PATH" FAKE_DIR="$T" SONORA_ENGINE="$T/bin/fake-engine" "$@" \
    "$ROOT/tools/umc-measure.sh" --seconds 1 --out "$T/rep.$name" --sessions "$T/s.$name" >"$T/out.$name" 2>"$T/err.$name"
}

# Healthy card, bare metal (as far as the script can tell): passes, lowest clean 256.
rc=0; run ok SONORA_UMC_FORCE_WSL=0 || rc=$?
[ "$rc" = 0 ] || bad "healthy card: exit $rc"
grep -q 'Lowest clean quantum: \*\*256\*\*' "$T/out.ok" || bad "healthy card: lowest clean quantum is not 256"
grep -q 'Channels: PASS' "$T/out.ok" || bad "healthy card: no PASS"
grep -q '| 64 | 64 | 3 |' "$T/out.ok" || bad "healthy card: quantum 64 row should show xruns"
grep -q 'p50_us' "$T/out.ok" || bad "healthy card: no IPC figures"
ls "$T"/rep.ok/umc1820-measurements-*.md >/dev/null 2>&1 || bad "no report file written"
if grep -q 'RUNNING UNDER WSL' "$T/out.ok" "$T/err.ok"; then bad "banner shown outside WSL"; fi

# WSL: the banner is loud on stderr and in the report.
rc=0; run wsl SONORA_UMC_FORCE_WSL=1 || rc=$?
grep -q 'NOT VALID BARE-METAL EVIDENCE' "$T/err.wsl" || bad "WSL: banner missing on stderr"
grep -q 'NOT VALID BARE-METAL EVIDENCE' "$T"/rep.wsl/umc1820-measurements-*.md || bad "WSL: banner missing in report"

# Wrong channel counts, too few JACK ports, short file, no card: each fails.
rc=0; run cap SONORA_UMC_FORCE_WSL=0 FAKE_CAP=8 || rc=$?
if [ "$rc" != 1 ] || ! grep -q 'capture channels: 8' "$T/out.cap"; then bad "8 capture channels not rejected"; fi
rc=0; run ports SONORA_UMC_FORCE_WSL=0 FAKE_PORTS=2 || rc=$?
if [ "$rc" != 1 ] || ! grep -q 'FAIL: JACK' "$T/out.ports"; then bad "2 JACK ports not rejected"; fi
rc=0; run short SONORA_UMC_FORCE_WSL=0 FAKE_SHORT=1 || rc=$?
if [ "$rc" != 1 ] || ! grep -q 'shorter than' "$T/out.short"; then bad "short file not rejected"; fi
rc=0; run nocard SONORA_UMC_FORCE_WSL=0 FAKE_NOCARD=1 || rc=$?
if [ "$rc" != 1 ] || ! grep -q 'no UMC' "$T/out.nocard"; then bad "missing card not rejected"; fi

# A quantum the graph did not honour is not "clean".
rc=0; run xr SONORA_UMC_FORCE_WSL=0 FAKE_XRUN_BELOW=9999 || rc=$?
grep -q 'No quantum ran clean' "$T/out.xr" || bad "all-xrun sweep should report no clean quantum"

python3 -I "$ROOT/tools/umc-ipc-probe.py" --seconds 1 --hz 100 | grep -q p99_us || bad "ipc probe printed no figures"

[ "$fail" = 0 ] && echo "umc-measure logic: ok"
exit "$fail"
