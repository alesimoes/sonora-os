#!/usr/bin/env bash
# Logic test for sonora-acceptance against a fake /proc, a fake /sys and stubs
# for the engine, pw-metadata, uname and systemctl. It proves what the kit
# decides (PASS, FAIL or SKIP, exit status, the power-cut state machine, the
# WAV header check, the report); it measures no hardware.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ACC="$ROOT/iso/config/includes.chroot/usr/local/bin/sonora-acceptance"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
mkdir -p "$T/bin"

# ---- stubs ------------------------------------------------------------------
cat >"$T/bin/uname" <<'EOF'
#!/usr/bin/env bash
echo "${FAKE_KERNEL:-6.12.43+deb13-rt-amd64}"
EOF
cat >"$T/bin/systemctl" <<'EOF'
#!/usr/bin/env bash
echo "$*" >>"$FAKE_DIR/systemctl.log"
EOF
cat >"$T/bin/pw-metadata" <<'EOF'
#!/usr/bin/env bash
q="${FAKE_QUANTUM:-128}"
for k in quantum min-quantum max-quantum; do
  echo "update: id:0 key:'clock.$k' value:'$q' type:''"
done
echo "update: id:0 key:'clock.rate' value:'48000' type:''"
EOF
cat >"$T/bin/pw-play" <<'EOF'
#!/usr/bin/env bash
[ -z "${FAKE_NOPLAY:-}" ]
EOF
cat >"$T/bin/fake-engine" <<'EOF'
#!/usr/bin/env bash
# Writes valid mono 24-bit WAVs; --headless repairs headers like the real one.
le() { printf '%s' "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $(($1 & 255)) $((($1 >> 8) & 255)) $((($1 >> 16) & 255)) $((($1 >> 24) & 255)))"; }
mkwav() { # FILE DATABYTES
  { printf 'RIFF'; printf "$(le $((36 + $2)))"; printf 'WAVEfmt '; printf "$(le 16)"; printf '\x01\x00\x01\x00'
    printf "$(le 48000)"; printf "$(le 144000)"; printf '\x03\x00\x18\x00'; printf 'data'; printf "$(le "$2")"
    head -c "$2" /dev/zero; } >"$1"
}
dir=""; n=18; sec=1; headless=0
while [ $# -gt 0 ]; do
  case "$1" in --dir) dir="$2"; shift ;; --tracks) n="$2"; shift ;; --record) sec="$2"; shift ;; --headless) headless=1 ;; esac
  shift
done
if [ "$headless" = 1 ]; then
  for f in "$dir"/*.wav; do
    size="$(wc -c <"$f")"
    if [ -z "${FAKE_NOREPAIR:-}" ]; then mkwav_tmp="$f.tmp"; mkwav "$mkwav_tmp" $((size - 44)); mv "$mkwav_tmp" "$f"; fi
    echo "recovered $(basename "$f")"
  done
  echo "engine listening on /tmp/fake.sock"
  trap 'exit 0' TERM
  sleep 100 & wait
  exit 0
fi
mkdir -p "$dir"
if [ "$sec" = 7200 ]; then
  for ((i = 0; i < n; i++)); do mkwav "$dir/t$i.wav" $((144000 * 25)); done
  sleep 2
  exit 1
fi
for ((i = 0; i < n; i++)); do mkwav "$dir/t$i.wav" $((144000 * sec)); done
if [ -n "${FAKE_SLOW:-}" ]; then trap 'echo "RESULT files=$n connected=$n frames=1 requested=1 rate=48000 buffer=128 xruns=0 clock_steps=0 overruns=0 misses=0 dropped=0"; exit 0' TERM; sleep "$FAKE_SLOW" & wait; fi
echo "xrun noise" >&2
echo "RESULT files=$n connected=$n frames=1 requested=1 rate=48000 buffer=${FAKE_BUFFER:-128} xruns=${FAKE_XRUNS:-0} clock_steps=0 overruns=0 misses=0 dropped=0"
EOF
chmod +x "$T"/bin/*

# ---- fake machine -------------------------------------------------------------
mkmachine() { # NAME
  local m="$T/$1"
  rm -rf "$m"
  mkdir -p "$m/proc/self" "$m/proc/sys/kernel/random" "$m/proc/asound/card1" "$m/sys/class/dmi/id"
  mkdir -p "$m/sys/bus/usb/devices/1-2" "$m/sys/devices/system/cpu/cpu0/cpufreq" "$m/sys/devices/system/cpu/cpu1/cpufreq"
  echo "Linux version 6.12.43+deb13-rt-amd64 #1 SMP PREEMPT_RT Debian" >"$m/proc/version"
  echo "BOOT_IMAGE=/live/vmlinuz boot=live persistence threadirqs quiet" >"$m/proc/cmdline"
  printf 'Max realtime priority     95                   95                   \nMax locked memory         unlimited            unlimited            bytes\n' >"$m/proc/self/limits"
  echo "overlay /home overlay rw 0 0" >"$m/proc/mounts"
  echo "b1" >"$m/proc/sys/kernel/random/boot_id"
  printf ' 1 [UMC1820        ]: USB-Audio - UMC 1820\n' >"$m/proc/asound/cards"
  echo UMC1820 >"$m/proc/asound/card1/id"
  printf 'Behringer UMC1820\n\nPlayback:\n  Status: Stop\n    Channels: 20\n\nCapture:\n  Status: Stop\n    Channels: 18\n' >"$m/proc/asound/card1/stream0"
  printf 'processor : 0\nmodel name : Fake Core i7\nprocessor : 1\n' >"$m/proc/cpuinfo"
  echo "MemTotal:       16000000 kB" >"$m/proc/meminfo"
  echo performance >"$m/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
  echo performance >"$m/sys/devices/system/cpu/cpu1/cpufreq/scaling_governor"
  echo 1397 >"$m/sys/bus/usb/devices/1-2/idVendor"
  echo 0503 >"$m/sys/bus/usb/devices/1-2/idProduct"
  echo "UMC 1820" >"$m/sys/bus/usb/devices/1-2/product"
  echo FakeCorp >"$m/sys/class/dmi/id/sys_vendor"
  echo Laptop9 >"$m/sys/class/dmi/id/product_name"
}

run() { # MACHINE OUTNAME ENV... -- ARGS...
  local m="$1" name="$2"
  shift 2
  local envs=()
  while [ "$1" != -- ]; do envs+=("$1"); shift; done
  shift
  env PATH="$T/bin:$PATH" FAKE_DIR="$T" SONORA_ACC_PROC="$T/$m/proc" SONORA_ACC_SYS="$T/$m/sys" \
    SONORA_ACC_DIR="$T/$m/acc" SONORA_ACC_USER="" SONORA_ENGINE="$T/bin/fake-engine" \
    SONORA_ACC_POLL=1 SONORA_ACC_TAKE=2 "${envs[@]}" "$ACC" "$@" >"$T/out.$name" 2>"$T/err.$name"
}
has() { grep -q -- "$2" "$T/out.$1" || bad "$1: missing '$2'"; }
hasnt() { ! grep -q -- "$2" "$T/out.$1" || bad "$1: unexpected '$2'"; }

# 1. a healthy machine passes everything
mkmachine m1
rc=0; run m1 quick1 X=1 -- quick || rc=$?
[ "$rc" = 0 ] || bad "healthy quick: exit $rc ($(cat "$T/err.quick1"))"
for c in rt_kernel cmdline_threadirqs limit_rtprio limit_memlock cpu_governor booted_live home_mount umc_usb umc_channels pipewire_pin take_files take_buffer take_xruns take_play; do
  has quick1 "^PASS $c"
done
hasnt quick1 '^FAIL'
has quick1 'card 1: 18 capture, 20 playback'
grep -q 'stop sonora-session.service' "$T/systemctl.log" || bad "service not stopped"
grep -q 'start sonora-session.service' "$T/systemctl.log" || bad "service not restarted"

# 2. a bad machine fails each check and the exit status says so
mkmachine m2
echo "BOOT_IMAGE=/vmlinuz quiet" >"$T/m2/proc/cmdline"
echo "Linux version 6.12-amd64 #1 SMP Debian" >"$T/m2/proc/version"
echo powersave >"$T/m2/sys/devices/system/cpu/cpu1/cpufreq/scaling_governor"
echo 0507 >"$T/m2/sys/bus/usb/devices/1-2/idProduct"
printf 'Max realtime priority     0                    0                    \nMax locked memory         65536                65536                bytes\n' >"$T/m2/proc/self/limits"
printf 'Playback:\n    Channels: 2\nCapture:\n    Channels: 2\n' >"$T/m2/proc/asound/card1/stream0"
rc=0; run m2 quick2 FAKE_KERNEL=6.12-amd64 FAKE_QUANTUM=1024 FAKE_XRUNS=4 FAKE_BUFFER=1024 FAKE_NOPLAY=1 -- quick || rc=$?
[ "$rc" = 1 ] || bad "bad quick: exit $rc, want 1"
for c in rt_kernel cmdline_threadirqs limit_rtprio limit_memlock cpu_governor umc_usb umc_channels pipewire_pin take_buffer take_xruns take_play; do
  has quick2 "^FAIL $c"
done
has quick2 'powersave'
has quick2 'xruns=4'
has quick2 '^SKIP booted_live'

# 3. no card at all, no cpufreq: FAIL for the card, SKIP for the governor
mkmachine m3
rm -rf "$T/m3/sys/bus/usb/devices/1-2" "$T/m3/proc/asound/card1" "$T/m3/sys/devices/system/cpu/cpu0" "$T/m3/sys/devices/system/cpu/cpu1"
: >"$T/m3/proc/asound/cards"
rc=0; run m3 quick3 X=1 -- quick || rc=$?
[ "$rc" = 1 ] || bad "no card: exit $rc, want 1"
has quick3 '^FAIL umc_usb'
has quick3 '^FAIL umc_alsa'
has quick3 '^SKIP cpu_governor'

# 4. soak: clean, then cut short by the disk guard
rc=0; run m1 soak1 FAKE_SLOW=3 -- soak 0.002 || rc=$?
[ "$rc" = 0 ] || bad "soak clean: exit $rc ($(cat "$T/err.soak1"))"
has soak1 '^PASS soak'
has soak1 'soak summary'
has soak1 'min=1 '
rc=0; run m1 soak2 FAKE_SLOW=5 SONORA_ACC_MIN_FREE_MB=999999999 -- soak 0.002 || rc=$?
[ "$rc" = 1 ] || bad "soak guard: exit $rc, want 1"
has soak2 '^FAIL soak: stopped early, disk-full guard'
rc=0; run m1 soak3 -- soak abc || rc=$?
[ "$rc" = 2 ] || bad "soak abc: exit $rc, want 2"

# 5. power cut then recover-check
rc=0; run m1 recover0 X=1 -- recover-check || rc=$?
has recover0 '^SKIP recover_check'
[ "$rc" = 0 ] || bad "recover without a test: exit $rc, want 0"
rc=0; run m1 pc1 X=1 -- powercut || true
has pc1 'PULL THE POWER NOW'
has pc1 '^PASS powercut_start'
grep -q '^min_bytes=[0-9]' "$T/m1/acc/powercut.state" || bad "state file has no min_bytes"
run m1 rc1 X=1 -- recover-check || true
has rc1 '^FAIL recover_boot: same boot'
echo b2 >"$T/m1/proc/sys/kernel/random/boot_id"
rc=0; run m1 rc2 X=1 -- recover-check || rc=$?
[ "$rc" = 0 ] || bad "recover-check after reboot: exit $rc ($(cat "$T/out.rc2"))"
for c in recover_boot recover_folder recover_session recover_wavs recover_loss; do has rc2 "^PASS $c"; done
[ ! -e "$T/m1/acc/powercut.state" ] || bad "state file kept after recover-check"

# 6. unrepairable power-cut files fail on the header check
rc=0; run m1 pc2 X=1 -- powercut || true
echo b3 >"$T/m1/proc/sys/kernel/random/boot_id"
f="$(find "$T/m1/acc" -path '*powercut-*' -name t0.wav | sort | tail -1)"
head -c $(($(wc -c <"$f") - 1000)) "$f" >"$f.cut"
mv "$f.cut" "$f"
rc=0; run m1 rc3 FAKE_NOREPAIR=1 -- recover-check || rc=$?
[ "$rc" = 1 ] || bad "damaged wav: exit $rc, want 1"
has rc3 '^FAIL recover_wav_t0.wav: RIFF size'

# 7. the report names the machine and counts the results
rc=0; run m1 rep X=1 -- report || rc=$?
[ "$rc" = 0 ] || bad "report: exit $rc"
rf="$(find "$T/m1/acc" -name 'report-*.md' | head -1)"
grep -q 'FakeCorp Laptop9' "$rf" || bad "report: no machine name"
grep -q 'Fake Core i7' "$rf" || bad "report: no CPU"
grep -q '| PASS | rt_kernel |' "$rf" || bad "report: no rt_kernel row"
grep -q 'Totals: [1-9][0-9]* PASS, [1-9][0-9]* FAIL' "$rf" || bad "report: totals wrong"

# 8. usage
rc=0; run m1 u1 X=1 -- nonsense || rc=$?
[ "$rc" = 2 ] || bad "unknown command: exit $rc, want 2"

[ "$fail" = 0 ] && echo "sonora-acceptance: all checks passed"
exit "$fail"
