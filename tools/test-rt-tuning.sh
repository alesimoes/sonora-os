#!/usr/bin/env bash
# Tests for the RT tuning shipped in the image (brain/research/rt-tuning.md):
#   1. sonora-rtirq against a fake /proc and a stubbed chrt
#   2. sonora-cpu-latency against a fake /sys and a regular file for
#      /dev/cpu_dma_latency
#   3. the udev rule with udevadm verify, and the units with systemd-analyze
#      verify, both in a debian:trixie container (skipped without docker unless
#      SONORA_RT_REQUIRE_DOCKER=1)
# What this cannot show: any effect on xruns. That needs the UMC1820.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INC="$ROOT/iso/config/includes.chroot"
RTIRQ="$INC/usr/local/bin/sonora-rtirq"
CPULAT="$INC/usr/local/bin/sonora-cpu-latency"
fails=0
bad() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }
ok() { echo "ok: $*"; }

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

# ---- 1. rtirq ----
P="$T/proc"
mkdir -p "$P/101" "$P/102" "$P/103" "$P/104" "$P/105" "$P/1"
cat >"$P/interrupts" <<'EOF'
           CPU0       CPU1
  16:        120          0  IO-APIC   16-fasteoi   ehci_hcd:usb1
 128:       5000       300  PCI-MSI-0000:00:14.0   0-edge   xhci_hcd
 129:       4000       200  PCI-MSIX-0000:05:00.0   1-edge   xhci_hcd
 130:       9000       100  PCI-MSI-0000:00:1f.3   0-edge   snd_hda_intel:card0
 131:       7000         0  PCI-MSI-0000:00:19.0   0-edge   enp0s31f6
NMI:          0          0   Non-maskable interrupts
EOF
echo "irq/128-xhci_hcd" >"$P/101/comm"
echo "irq/129-xhci_hcd" >"$P/102/comm"
echo "irq/130-snd_hda_i" >"$P/103/comm"
echo "irq/131-enp0s31f" >"$P/104/comm"
echo "irq/1280-other" >"$P/105/comm" # 128 must not match irq/1280-
echo "systemd" >"$P/1/comm"
cat >"$T/chrt" <<EOF
#!/bin/sh
echo "\$*" >>"$T/chrt.log"
EOF
chmod +x "$T/chrt"

run_rtirq() { SONORA_PROC="$P" SONORA_CHRT="$T/chrt" sh "$RTIRQ"; }

: >"$T/chrt.log"
out="$(run_rtirq)"
want=$'-f -p 85 101\n-f -p 85 102'
[ "$(sort "$T/chrt.log")" = "$want" ] || bad "rtirq chrt calls: $(cat "$T/chrt.log")"
grep -q 103 "$T/chrt.log" && bad "rtirq touched the HDA audio IRQ thread"
grep -q 104 "$T/chrt.log" && bad "rtirq touched the NIC IRQ thread"
grep -q 105 "$T/chrt.log" && bad "rtirq matched irq/1280 for irq 128"
echo "$out" | grep -q "irq 128 thread pid 101" || bad "rtirq did not log what it did: $out"
[ "$fails" = 0 ] && ok "rtirq raises only the xhci_hcd IRQ threads to 85"

# Idempotent: same calls again.
: >"$T/chrt.log"
run_rtirq >/dev/null
[ "$(sort "$T/chrt.log")" = "$want" ] || bad "rtirq second run differs"

# The priority stays under PipeWire's rt.prio and above the threadirqs default.
prio="$(awk '/rt\.prio/ { print $3; exit }' "$ROOT/audio-stack/pipewire-sonora.conf")"
[ "${prio:-0}" -gt 85 ] || bad "rtirq prio 85 is not below the pipewire rt.prio ($prio)"

# Override and no-match / no-thread / chrt failure all exit 0.
: >"$T/chrt.log"
SONORA_RTIRQ_PRIO=70 SONORA_PROC="$P" SONORA_CHRT="$T/chrt" sh "$RTIRQ" >/dev/null
grep -q '^-f -p 70 101$' "$T/chrt.log" || bad "SONORA_RTIRQ_PRIO ignored"
SONORA_RTIRQ_NAMES=nosuchdriver run_rtirq | grep -q "nothing to do" || bad "no-match not logged"
mkdir -p "$T/empty"
: >"$T/empty/interrupts"
SONORA_PROC="$T/empty" SONORA_CHRT="$T/chrt" sh "$RTIRQ" >/dev/null || bad "rtirq nonzero on empty interrupts"
printf '#!/bin/sh\nexit 1\n' >"$T/chrt-fail"
chmod +x "$T/chrt-fail"
out="$(SONORA_PROC="$P" SONORA_CHRT="$T/chrt-fail" sh "$RTIRQ")"
grep -q "chrt failed" <<<"$out" || bad "chrt failure not logged"
SONORA_PROC="$P" SONORA_CHRT="$T/chrt-fail" sh "$RTIRQ" >/dev/null || bad "rtirq nonzero when chrt fails"
ok "rtirq override, empty and failing cases"

# ---- 2. cpu latency ----
S="$T/sys/devices/system/cpu"
mkdir -p "$S/cpu0/cpufreq" "$S/cpu1/cpufreq" "$S/cpu2"
echo powersave >"$S/cpu0/cpufreq/scaling_governor"
echo schedutil >"$S/cpu1/cpufreq/scaling_governor"
: >"$T/dma"
out="$(SONORA_SYS="$T/sys" SONORA_DMA="$T/dma" SONORA_HOLD=true SONORA_HOLD_ARG="" sh "$CPULAT")"
[ "$(cat "$S/cpu0/cpufreq/scaling_governor")" = performance ] || bad "cpu0 governor not performance"
[ "$(cat "$S/cpu1/cpufreq/scaling_governor")" = performance ] || bad "cpu1 governor not performance"
echo "$out" | grep -q "performance on 2 cpu" || bad "governor count not logged: $out"
[ "$(od -An -tu4 <"$T/dma" | tr -d ' ')" = 10 ] || bad "dma latency value: $(od -An -tx1 <"$T/dma")"
SONORA_CPU_LATENCY_US=300 SONORA_SYS="$T/sys" SONORA_DMA="$T/dma" SONORA_HOLD=true SONORA_HOLD_ARG="" sh "$CPULAT" >/dev/null
[ "$(od -An -tu4 <"$T/dma" | tr -d ' ')" = 300 ] || bad "dma latency 300 not written as u32"
out="$(SONORA_SYS="$T/nosys" SONORA_DMA="$T/missing/dma" SONORA_HOLD=true SONORA_HOLD_ARG="" sh "$CPULAT")" || bad "cpu-latency fails without cpufreq or dma"
echo "$out" | grep -q "not writable" || bad "missing dma not logged"
ok "cpu-latency sets governor, writes u32 microseconds, tolerates absence"

# ---- 3. wiring ----
grep -q '^Wants=.*sonora-rtirq.service' "$INC/etc/systemd/system/sonora-session.service" || bad "session does not Want sonora-rtirq"
grep -q '^Wants=.*sonora-cpu-latency.service' "$INC/etc/systemd/system/sonora-session.service" || bad "session does not Want sonora-cpu-latency"
grep -q 'systemctl enable sonora-rtirq.service sonora-cpu-latency.service' "$ROOT/iso/config/hooks/normal/0100-sonora-system.hook.chroot" || bad "hook does not enable the units"
grep -q 'chmod 0755 /usr/local/bin/sonora-rtirq' "$ROOT/iso/config/hooks/normal/0100-sonora-system.hook.chroot" || bad "hook does not chmod sonora-rtirq"
grep -q 'chmod 0755 /usr/local/bin/sonora-cpu-latency' "$ROOT/iso/config/hooks/normal/0100-sonora-system.hook.chroot" || bad "hook does not chmod sonora-cpu-latency"

# ---- 4. udev and systemd in a container ----
if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
  if [[ "$(uname -s)" == MINGW* || "$(uname -s)" == MSYS* ]]; then
    export MSYS2_ARG_CONV_EXCL='*'
    HP="$(cd "$INC" && pwd -W)"
  else
    HP="$INC"
  fi
  if docker run --rm -v "$HP:/inc:ro" debian:trixie sh -c '
      set -e
      apt-get update -qq >/dev/null
      apt-get install -y -qq udev systemd >/dev/null 2>&1
      cp -r /inc/usr/local /usr/
      cp /inc/etc/systemd/system/sonora-rtirq.service /inc/etc/systemd/system/sonora-cpu-latency.service /etc/systemd/system/
      chmod 755 /usr/local/bin/sonora-rtirq /usr/local/bin/sonora-cpu-latency
      udevadm verify /inc/etc/udev/rules.d/90-sonora-usb-audio.rules
      systemd-analyze verify /etc/systemd/system/sonora-rtirq.service /etc/systemd/system/sonora-cpu-latency.service
      # a rule that the verifier accepts must also be one that fails when broken
      printf "ACTION==\"add\" SUBSYSTEM==\"usb\"\n" >/tmp/bad.rules
      if udevadm verify /tmp/bad.rules >/dev/null 2>&1; then echo "udevadm verify accepts a broken rule"; exit 3; fi
    '; then
    ok "udevadm verify and systemd-analyze verify"
  else
    bad "udevadm/systemd-analyze verify in debian:trixie"
  fi
elif [ "${SONORA_RT_REQUIRE_DOCKER:-0}" = 1 ]; then
  bad "docker unavailable"
else
  echo "SKIP: docker unavailable, udev and systemd verification not run"
fi

if [ "$fails" -ne 0 ]; then
  echo "$fails failure(s)" >&2
  exit 1
fi
echo "test-rt-tuning: all passed"
