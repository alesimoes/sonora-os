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

if [ "$fail" -eq 0 ]; then
  echo "ok: pipewire $pw_pin, wireplumber $wp_pin from $(printf '%s\n' "$src" | grep -Eo '[0-9]{8}T[0-9]{6}Z'); rt kernel; tty1 owned, no VT gettys"
fi
exit "$fail"
