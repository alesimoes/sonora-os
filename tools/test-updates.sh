#!/usr/bin/env bash
# Static check of the update policy (ADR-0034). It proves the files say what
# the ADR says; it cannot prove apt's behaviour on an installed machine.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INC="$ROOT/iso/config/includes.chroot/etc"
PREF="$INC/apt/preferences.d/sonora.pref"
APTC="$INC/apt/apt.conf.d/99sonora-no-auto-update"
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
for f in "$PREF" "$APTC"; do [ -f "$f" ] || bad "missing ${f#"$ROOT"/}"; done
[ "$fail" -eq 0 ] || exit 1

# pin_of PATTERN: priority of the stanza whose Package or Pin line matches.
pin_of() {
  awk -v pat="$1" '
    /^[[:space:]]*#/ { next }
    /^Package:/ { pkg=$0 }
    /^Pin:/ { pin=$0 }
    /^Pin-Priority:/ { if (index(pkg " " pin, pat)) { print $2; exit } }' "$PREF"
}
for r in unstable testing experimental; do
  [ "$(pin_of "a=$r")" = "-1" ] || bad "sonora.pref: $r is not pinned to -1"
done
for p in 'linux-image-*' 'linux-headers-*' 'linux-image-rt-amd64'; do
  [ "$(pin_of "$p")" = "-1" ] || bad "sonora.pref: $p is not held (pin -1)"
done

for k in Enable Update-Package-Lists Unattended-Upgrade Download-Upgradeable-Packages; do
  grep -Eq "^APT::Periodic::$k \"0\";" "$APTC" || bad "apt.conf: APT::Periodic::$k is not \"0\""
done
grep -Eq '^Unattended-Upgrade::Automatic-Reboot "false";' "$APTC" || bad "apt.conf: automatic reboot not disabled"

for u in apt-daily apt-daily-upgrade; do
  d="$INC/systemd/system/$u.service.d/sonora.conf"
  [ -f "$d" ] || { bad "missing drop-in for $u.service"; continue; }
  grep -Eq '^ConditionPathExists=/var/lib/sonora/updates-allowed$' "$d" ||
    bad "$u.service: not conditioned on the updates-allowed flag"
done

if grep -Eq '^[[:space:]]*unattended-upgrades' "$ROOT"/iso/config/package-lists/*.chroot; then
  bad "package list installs unattended-upgrades"
fi

[ "$fail" -eq 0 ] || exit 1
echo "ok: update policy files agree with ADR-0034 (UNVERIFIED on an installed machine)"
