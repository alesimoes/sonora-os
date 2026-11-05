#!/usr/bin/env bash
# Static check of the UMC1820 device profile in audio-stack/wireplumber-sonora.conf,
# in a debian:trixie container with wireplumber installed (no USB, no sound
# card): the file parses as SPA JSON, and the rules WirePlumber's own matcher
# applies to fake ALSA nodes give the UMC1820 no suspend-on-idle and a 128
# period, and leave every other node alone. Proves nothing about the hardware;
# the commands that do are in brain/research/umc1820-profile.md.
#
#   tools/test-umc-profile.sh        # needs docker and network (apt)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

command -v docker >/dev/null || { echo "docker is required" >&2; exit 2; }

inner='
set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends wireplumber pipewire pipewire-bin >/dev/null
spa-json-dump /w/audio-stack/wireplumber-sonora.conf >/dev/null
# A private WirePlumber configuration: the stock main config, our fragment as
# conf.d, and one extra component that runs the matcher script.
mkdir -p /cfg/test.conf.d /run/u
cp /usr/share/wireplumber/wireplumber.conf /cfg/test.conf
cp /w/audio-stack/wireplumber-sonora.conf /cfg/test.conf.d/99-sonora.conf
cat > /cfg/test.conf.d/98-test.conf <<EOT
wireplumber.components = [
  { name = /w/tools/umc-profile-match.lua, type = script/lua, provides = test.match }
]
wireplumber.profiles = { main = { test.match = required } }
EOT
export XDG_RUNTIME_DIR=/run/u WIREPLUMBER_CONFIG_DIR=/cfg
pipewire >/tmp/pw.log 2>&1 &
sleep 2
# The script cannot quit the daemon; the matches are printed within a second.
timeout 8 wireplumber -c test.conf 2>&1 || true
'
inner="mkdir -p /w && tar -x -i -C /w && $inner"
# The files travel on stdin: a bind mount of the checkout breaks under Git
# Bash path conversion on Windows. SONORA_WP_CONF substitutes the rules file,
# to run the test against an older copy and see it fail.
conf="${SONORA_WP_CONF:-$ROOT/audio-stack/wireplumber-sonora.conf}"
out="$( { tar -c -C "$ROOT" tools/umc-profile-match.lua
          tar -c -C "$(dirname "$conf")" --transform "s|^.*\$|audio-stack/wireplumber-sonora.conf|" "$(basename "$conf")"
        } | docker run --rm -i debian:trixie sh -c "$inner" 2>&1)" || { printf '%s\n' "$out" | tail -30 >&2; exit 1; }
printf '%s\n' "$out" | grep -E '^MATCH' || { printf '%s\n' "$out" | tail -20 >&2; }

fail=0
expect() {
  printf '%s\n' "$out" | grep -qF "$1" || { echo "FAIL: missing '$1'" >&2; fail=1; }
}
expect 'MATCH alsa_input.usb-BEHRINGER_UMC1820_1234567-00.pro-input-0 suspend=0 period=128'
expect 'MATCH alsa_output.usb-BEHRINGER_UMC1820_1234567-00.pro-output-0 suspend=0 period=128'
expect 'MATCH alsa_input.pci-0000_00_1f.3.pro-input-0 suspend=nil period=nil'
expect 'MATCH alsa_input.usb-BEHRINGER_UMC202HD_1-00.pro-input-0 suspend=nil period=nil'
[ "$fail" -eq 0 ] || exit 1
echo "test-umc-profile: ok"
