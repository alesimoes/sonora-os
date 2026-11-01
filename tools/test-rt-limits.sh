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

grep -Eq '^@audio[[:space:]]+-[[:space:]]+rtprio[[:space:]]+95$' "$LIM" || bad "limits: @audio rtprio 95 missing"
grep -Eq '^@audio[[:space:]]+-[[:space:]]+memlock[[:space:]]+unlimited$' "$LIM" || bad "limits: @audio memlock unlimited missing"
grep -Eq '^LimitRTPRIO=95$' "$UNIT" || bad "unit: LimitRTPRIO=95 missing or differs from limits.d"
grep -Eq '^LimitMEMLOCK=infinity$' "$UNIT" || bad "unit: LimitMEMLOCK=infinity missing"
grep -Eq 'install .*limits-audio\.conf' "$HOOK" || bad "hook does not install limits-audio.conf"
grep -q '/etc/security/limits.d/95-audio.conf' "$HOOK" || bad "hook installs limits to the wrong path"
grep -Eq 'usermod -aG [^ ]*audio' "$HOOK" || bad "hook does not put the operator in the audio group"
if [ "$fail" -eq 0 ]; then echo "ok: RT limits agree across limits.d, unit and hook"; fi
exit "$fail"
