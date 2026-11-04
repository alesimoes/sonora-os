#!/usr/bin/env bash
# Unit test of the installed-root helpers: swap sizing and fstab rendering.
# Not proven here: that a real boot mounts these options and that the swapfile
# is created and activated (needs sonora-install on a disk).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
D="$ROOT/iso/config/includes.chroot/usr/share/sonora/install"
# shellcheck source=/dev/null
. "$D/sonora-rootfs.sh"
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
eq() { [ "$2" = "$3" ] || bad "$1: got '$2' want '$3'"; }

eq "1G ram, big disk" "$(swap_mib 1024 200000)" 2048
eq "4G ram" "$(swap_mib 4096 200000)" 2048
eq "16G ram" "$(swap_mib 16384 200000)" 4096
eq "128G ram capped" "$(swap_mib 131072 500000)" 4096
eq "small disk capped to a tenth" "$(swap_mib 16384 20000)" 2000
eq "tiny disk none" "$(swap_mib 16384 2000)" 0
eq "empty ram" "$(swap_mib '' 100000 || true)" 0
eq "non numeric" "$(swap_mib 8G 100000 || true)" 0

out="$(render_fstab "$D/fstab.template" aaaa-1 BBBB-2 2048)"
live="$(grep -v '^#' <<<"$out")"
grep -Eq '^UUID=aaaa-1 +/ +ext4 +noatime,' <<<"$out" || bad "root line lacks UUID or noatime"
grep -q '^UUID=BBBB-2 ' <<<"$out" || bad "esp uuid"
grep -q '^/swapfile .*swap' <<<"$out" || bad "swap line"
if grep -q '@' <<<"$live"; then bad "unreplaced marker"; fi
if grep -q 'discard' <<<"$live"; then bad "inline discard"; fi
out0="$(render_fstab "$D/fstab.template" a b 0)"
if grep -q '^/swapfile' <<<"$out0"; then bad "swap line with 0 MiB"; fi

# Tuning lives once, in audio-stack.
if grep -Eq '^[[:space:]]*(vm\.)?swappiness' "$D/sonora-rootfs.sh"; then bad "swappiness duplicated"; fi
grep -Eq '^vm.swappiness *= *[0-9]+' "$ROOT/audio-stack/sysctl-sonora.conf" || bad "sysctl lost swappiness"

if [ "$fail" -eq 0 ]; then
  echo "ok: swap sizing and fstab rendering (UNVERIFIED on a real boot)"
fi
exit "$fail"
