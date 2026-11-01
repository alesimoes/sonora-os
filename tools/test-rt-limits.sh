#!/usr/bin/env bash
# Static check that the RT privileges the engine needs are granted on both
# paths: limits.d covers PAM logins, but systemd ignores it for services, so
# the unit must carry the same numbers. A drift between the two means the
# engine silently loses SCHED_FIFO or mlock on one path.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIM="$ROOT/audio-stack/limits-audio.conf"
UNIT="$ROOT/iso/config/includes.chroot/etc/systemd/system/sonora-session.service"
HOOK="$ROOT/iso/config/hooks/normal/0100-sonora-system.hook.chroot"
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

# limits_value ITEM: the value of "@audio - ITEM value" in limits.d.
limits_value() {
  sed -nE "s/^@audio[[:space:]]+-[[:space:]]+$1[[:space:]]+([^[:space:]]+)[[:space:]]*\$/\1/p" "$LIM"
}
# unit_value KEY: the value of KEY=value in the unit.
unit_value() {
  sed -nE "s/^$1=([^[:space:]]+)[[:space:]]*\$/\1/p" "$UNIT"
}

rtprio="$(limits_value rtprio)"
memlock="$(limits_value memlock)"
[ -n "$rtprio" ] || bad "limits: @audio rtprio missing"
[ -n "$memlock" ] || bad "limits: @audio memlock missing"

# systemd spells "unlimited" as "infinity".
[ "$memlock" = unlimited ] && memlock=infinity
u_rtprio="$(unit_value LimitRTPRIO)"
u_memlock="$(unit_value LimitMEMLOCK)"
[ "$u_rtprio" = "$rtprio" ] || bad "unit: LimitRTPRIO='$u_rtprio' differs from limits.d rtprio '$rtprio'"
[ "$u_memlock" = "$memlock" ] || bad "unit: LimitMEMLOCK='$u_memlock' differs from limits.d memlock '$memlock'"

grep -Eq 'install .*limits-audio\.conf' "$HOOK" || bad "hook does not install limits-audio.conf"
grep -q '/etc/security/limits.d/95-audio.conf' "$HOOK" || bad "hook installs limits to the wrong path"
grep -Eq 'usermod -aG [^ ]*audio' "$HOOK" || bad "hook does not put the operator in the audio group"
if [ "$fail" -eq 0 ]; then echo "ok: RT limits agree across limits.d, unit and hook"; fi
exit "$fail"
