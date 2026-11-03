#!/usr/bin/env bash
# Static check of the live-build configuration: the PipeWire pins agree
# everywhere they are stated, the RT kernel is the bootstrap flavour, and
# nothing leaves the operator at a login prompt or a shell. Each check is a
# way the image boots wrong without the build saying so.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AUTO="$ROOT/iso/auto/config"
LIST="$ROOT/iso/config/archives/sonora-backports.list.chroot"
PREF="$ROOT/iso/config/archives/sonora-backports.pref.chroot"
HOOK="$ROOT/iso/config/hooks/normal/0100-sonora-system.hook.chroot"
PKGS="$ROOT/iso/config/package-lists/sonora.list.chroot"
INC="$ROOT/iso/config/includes.chroot"
UNIT="$INC/etc/systemd/system/sonora-session.service"
LOGIND="$INC/etc/systemd/logind.conf.d/sonora.conf"
RUNTIME="$ROOT/tools/Dockerfile.runtime"
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
for f in "$AUTO" "$LIST" "$PREF" "$HOOK" "$PKGS" "$UNIT" "$LOGIND" "$RUNTIME"; do
  [ -f "$f" ] || bad "missing ${f#"$ROOT"/}"
done
[ "$fail" -eq 0 ] || exit 1

# --- backports: a dated snapshot, exact versions, checked by the hook ---
src="$(grep -E '^deb ' "$LIST" || true)"
[ "$(printf '%s\n' "$src" | grep -c .)" = 1 ] || bad "backports list: expected exactly one deb line"
printf '%s\n' "$src" | grep -Eq 'snapshot\.debian\.org/archive/debian/[0-9]{8}T[0-9]{6}Z trixie-backports ' ||
  bad "backports list: not a dated snapshot.debian.org trixie-backports source"
printf '%s\n' "$src" | grep -Eq 'deb\.debian\.org| (unstable|sid|testing)( |$)' && bad "backports list names a moving archive"
# --backports true would add live deb.debian.org backports beside the snapshot.
grep -Eq -- '--backports[[:space:]]+true' "$AUTO" && bad "auto/config: --backports true adds an undated backports source"

# --- the rest of the archive: the same snapshot (ADR-0002, 2026-11-02) ---
# A live trixie mirror lets a point release change the RT kernel between two
# builds of one commit; a second timestamp lets the base and PipeWire drift apart.
bp_ts="$(printf '%s\n' "$src" | grep -Eo '[0-9]{8}T[0-9]{6}Z' | head -n 1)"
auto_ts="$(sed -nE 's/^SONORA_SNAPSHOT=([0-9]{8}T[0-9]{6}Z)$/\1/p' "$AUTO")"
[ -n "$auto_ts" ] || bad "auto/config: no SONORA_SNAPSHOT=<YYYYMMDDTHHMMSSZ> line"
[ "$auto_ts" = "$bp_ts" ] || bad "auto/config snapshot '$auto_ts' differs from the backports list's '$bp_ts'"
# mirror_arg OPTION: the value auto/config passes to `lb config OPTION`.
mirror_arg() {
  sed -nE "s/^[[:space:]]*$1[[:space:]]+\"?([^\" ]+)\"?.*$/\1/p" "$AUTO" | head -n 1
}
snap='http://snapshot.debian.org/archive'
# $SONORA_SNAPSHOT is expanded by lb config, not here: compare the text.
for opt in --mirror-bootstrap --mirror-chroot; do
  [ "$(mirror_arg "$opt")" = "$snap/debian/\$SONORA_SNAPSHOT/" ] || bad "auto/config: $opt is not $snap/debian/\$SONORA_SNAPSHOT/"
done
[ "$(mirror_arg --mirror-chroot-security)" = "$snap/debian-security/\$SONORA_SNAPSHOT/" ] ||
  bad "auto/config: --mirror-chroot-security is not $snap/debian-security/\$SONORA_SNAPSHOT/"
# The shipped system's sources: live, or it never sees a security update.
for opt in --mirror-binary --mirror-binary-security; do
  v="$(mirror_arg "$opt")"
  [ -n "$v" ] || bad "auto/config: $opt not set; live-build's default is not ours to rely on"
  case "$v" in *snapshot.debian.org*) bad "auto/config: $opt ships a frozen source in the image" ;; esac
done
# --parent-mirror-* would override the mirrors above for a non-derivative build.
grep -Eq -- '--parent-mirror-' "$AUTO" && bad "auto/config: a --parent-mirror-* option bypasses the snapshot mirrors"
# security's Release expires a week after the snapshot date; the override must
# be build-time only (.conf.chroot), or the shipped system stops checking expiry.
SNAPCONF="$ROOT/iso/config/archives/sonora-snapshot.conf.chroot"
grep -Eq '^Acquire::Check-Valid-Until[[:space:]]+"false";' "$SNAPCONF" 2>/dev/null ||
  bad "archives: sonora-snapshot.conf.chroot does not turn off Check-Valid-Until for the build"
for f in "$ROOT"/iso/config/archives/*.conf "$ROOT"/iso/config/archives/*.conf.binary "$ROOT"/iso/config/apt/*; do
  [ -e "$f" ] && grep -qi 'Check-Valid-Until' "$f" && bad "${f#"$ROOT"/} ships Check-Valid-Until in the image"
done
grep -qF 'E: build sources outside snapshot' "$HOOK" ||
  bad "hook does not check that every build source is the snapshot"

# pin_version GLOB: the Pin: version of the stanza whose Package line starts with GLOB.
pin_version() {
  awk -v pkg="$1" '
    /^Package:/ { hit = ($2 == pkg) }
    hit && /^Pin: version / { print $3; exit }' "$PREF"
}
pw_pin="$(pin_version 'pipewire*')"
wp_pin="$(pin_version 'wireplumber')"
[ -n "$pw_pin" ] || bad "pref: no exact version pin for pipewire*"
[ -n "$wp_pin" ] || bad "pref: no exact version pin for wireplumber"
case "$pw_pin" in *~bpo13*) ;; *) bad "pref: pipewire pin '$pw_pin' is not a trixie backport" ;; esac
case "$wp_pin" in *~bpo13*) ;; *) bad "pref: wireplumber pin '$wp_pin' is not a trixie backport" ;; esac
grep -Eq '^Pin: release' "$PREF" && bad "pref: a release pin follows new uploads; pin versions"
awk '/^Pin-Priority:/ && $2 <= 1000 { exit 1 }' "$PREF" || bad "pref: Pin-Priority must exceed 1000 to win over trixie"

hook_pw="$(sed -nE 's/^SONORA_PIPEWIRE_VERSION=(.*)$/\1/p' "$HOOK")"
hook_wp="$(sed -nE 's/^SONORA_WIREPLUMBER_VERSION=(.*)$/\1/p' "$HOOK")"
[ "$hook_pw" = "$pw_pin" ] || bad "hook checks pipewire '$hook_pw', pref pins '$pw_pin'"
[ "$hook_wp" = "$wp_pin" ] || bad "hook checks wireplumber '$hook_wp', pref pins '$wp_pin'"

# The container tests must run the PipeWire the image ships, from the same files.
grep -q 'COPY iso/config/archives/sonora-backports.list.chroot ' "$RUNTIME" || bad "Dockerfile.runtime does not use the image's backports source"
grep -q 'COPY iso/config/archives/sonora-backports.pref.chroot ' "$RUNTIME" || bad "Dockerfile.runtime does not use the image's pins"

# --- RT kernel ---
grep -Eq -- '--linux-flavours[[:space:]]+rt-amd64' "$AUTO" || bad "auto/config: RT kernel is not the bootstrap flavour"
grep -Eq '^linux-image' "$PKGS" && bad "package list names a kernel; it would pull the generic one in"
grep -Eq '^libjack-jackd2-0$|^jackd' "$PKGS" && bad "package list installs a libjack that shadows pipewire-jack"
for p in pipewire pipewire-jack wireplumber libspa-0.2-modules; do
  grep -qx "$p" "$PKGS" || bad "package list lacks $p"
done

# --- no display manager, no login prompt, never a shell ---
grep -Eqx 'gdm3|lightdm|sddm|lxdm|slim|nodm|xdm|greetd|task-.*desktop' "$PKGS" && bad "package list installs a display manager"
grep -Eq '^ExecStart=/usr/local/bin/sonora-engine( |$)' "$UNIT" || bad "unit does not start the engine"
grep -qx 'Restart=always' "$UNIT" || bad "unit: engine is not restarted on every exit"
grep -qx 'StartLimitIntervalSec=0' "$UNIT" || bad "unit: a start limit would leave tty1 blank after a crash loop"
grep -qx 'TTYPath=/dev/tty1' "$UNIT" || bad "unit does not own tty1"
grep -qx 'Conflicts=getty@tty1.service' "$UNIT" || bad "unit does not displace the tty1 getty"
grep -qx 'User=sonora' "$UNIT" || bad "unit does not run as the operator"
grep -qx 'WantedBy=multi-user.target' "$UNIT" || bad "unit is not part of the default target"
grep -qx 'NAutoVTs=0' "$LOGIND" || bad "logind spawns VT gettys: Ctrl+Alt+F2 is a passwordless login"
grep -qx 'ReserveVT=0' "$LOGIND" || bad "logind reserves a VT with a getty on it"
grep -qx 'systemctl enable sonora-session.service' "$HOOK" || bad "hook does not enable the session unit"
grep -q 'systemctl disable getty@tty1.service' "$HOOK" || bad "hook leaves the tty1 getty enabled"
grep -q 'systemctl set-default multi-user.target' "$HOOK" || bad "hook does not boot to multi-user.target"
grep -q 'display-manager.service' "$HOOK" || bad "hook does not refuse an installed display manager"

# NAutoVTs=0 only stops logind's on-demand gettys. An enabled getty@tty2..6
# (or autovt@) would still put a passwordless login behind Ctrl+Alt+F2..F6,
# whatever logind.conf says, so nothing in the tree may start one.
vt='(getty|autovt)@tty[2-6]'
# In a hook, any non-comment mention other than a mask or disable counts:
# systemctl enable, a hand-made getty.target.wants symlink, a sed on NAutoVTs.
grep -EHn "$vt" "$ROOT"/iso/config/hooks/*/*.hook.* 2>/dev/null |
  grep -Ev '^[^:]*:[0-9]+:[[:space:]]*#' |
  grep -Ev "systemctl (mask|disable) ([^ ]+ )*$vt" &&
  bad "a hook enables a VT getty on tty2..6"
unit_dirs=()
for d in "$INC/etc/systemd" "$INC/usr/lib/systemd" "$INC/lib/systemd"; do
  [ -d "$d" ] && unit_dirs+=("$d")
done
# grep exits 2 on a missing directory even after a match, hence the list.
grep -EHnr "^(Wants|Requires|Upholds|BindsTo|Also|WantedBy|RequiredBy|UpheldBy)=.*$vt" "${unit_dirs[@]}" &&
  bad "a unit pulls in a VT getty on tty2..6"
while IFS= read -r f; do
  # A symlink to /dev/null is a mask, which is the safe direction.
  [ "$(readlink "$f" 2>/dev/null)" = /dev/null ] && continue
  bad "includes.chroot ships ${f#"$ROOT"/}"
done < <(find "$INC" \( -name 'getty@tty[2-6].service' -o -name 'autovt@tty[2-6].service' \) 2>/dev/null)

# --- display fallback (ADR-0010 stage 1): it must always end on the session ---
FALLBACK="$INC/etc/systemd/system/sonora-display-fallback.service"
flag=/run/sonora-display-failed
if [ -f "$FALLBACK" ]; then
  # Without '-' a failed stop ends the oneshot before the session starts, and
  # tty1 is left with neither the display nor the engine.
  grep -Eq '^ExecStart=-/usr/bin/systemctl stop ' "$FALLBACK" || bad "fallback: a failed stop would skip starting the session (needs '-')"
  grep -Eq '^ExecStart=/usr/bin/systemctl stop ' "$FALLBACK" && bad "fallback: a systemctl stop without '-'"
  grep -qx "ExecStart=/usr/bin/touch $flag" "$FALLBACK" || bad "fallback does not create $flag"
  grep -Eq '^ExecStart=-?/usr/bin/systemctl (--no-block )?start sonora-session\.service$' "$FALLBACK" || bad "fallback does not start sonora-session"
  grep -qx "ConditionPathExists=|$flag" "$UNIT" || bad "session condition does not accept $flag"
  grep -qx 'ConditionKernelCommandLine=|!sonora.gui' "$UNIT" || bad "session condition is not '|!sonora.gui'"
  grep -q 'chvt 2' "$FALLBACK" && bad "fallback switches to tty2, which has no terminal UI before ADR-0010 stage 2"
fi

# --- boot smoke test: one orderly poweroff on success ---
# A forced poweroff on success skips the unit stops, so the persistence
# overlay is never unmounted and test-write-usb.sh's clean-fsck check after
# boot 3 measures luck. A poweroff in the script races the unit's action.
SMOKE_UNIT="$INC/etc/systemd/system/sonora-smoketest.service"
SMOKE_SCRIPT="$INC/usr/local/bin/sonora-smoketest"
if [ -f "$SMOKE_UNIT" ]; then
  # [Unit] keys only: under [Service] systemd ignores them with a warning.
  smoke_unit_keys="$(awk '/^\[/ { u = ($0 == "[Unit]") } u' "$SMOKE_UNIT")"
  printf '%s\n' "$smoke_unit_keys" | grep -qx 'SuccessAction=poweroff' ||
    bad "smoketest unit: [Unit] SuccessAction= is not an orderly 'poweroff'"
  printf '%s\n' "$smoke_unit_keys" | grep -qx 'FailureAction=poweroff-force' ||
    bad "smoketest unit: [Unit] FailureAction= is not 'poweroff-force'; a wedged check would hold the guest up"
  grep -Ev '^[[:space:]]*#' "$SMOKE_SCRIPT" | grep -Eq '(systemctl[[:space:]].*(poweroff|halt|reboot)|^[[:space:]]*(poweroff|halt|reboot|shutdown)([[:space:]]|$))' &&
    bad "sonora-smoketest powers off by itself; the unit's SuccessAction= does that"
fi

if [ "$fail" -eq 0 ]; then
  echo "ok: whole archive from snapshot $auto_ts; pipewire $pw_pin, wireplumber $wp_pin; rt kernel; tty1 owned, no VT gettys; smoketest powers off in order"
fi
exit "$fail"
