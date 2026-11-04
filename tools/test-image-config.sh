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

# The hook's snapshot check itself, run against a scratch /etc/apt. A grep for
# its error text cannot tell whether a deb822 file slips past it.
snapcheck="$(awk '/^# --- every package came from the one snapshot ---$/ { on = 1; next }
                  on && /^# --- / { exit } on' "$HOOK")"
[ -n "$snapcheck" ] || bad "hook: no '# --- every package came from the one snapshot ---' section"
apt_tmp="$(mktemp -d)"
trap 'rm -rf "$apt_tmp"' EXIT
# hook_snapcheck: 0 when the hook's check accepts $apt_tmp/etc/apt.
hook_snapcheck() {
  printf '%s\n' "$snapcheck" | sed "s#/etc/apt/#$apt_tmp/etc/apt/#g" | sh >/dev/null 2>&1
}
# apt_case NAME SOURCES_CONTENT [LIST_LINE]: a fresh /etc/apt with the
# backports list, sources.list.d/debian.sources and an optional sources.list.
apt_case() {
  rm -rf "${apt_tmp:?}/etc"
  mkdir -p "$apt_tmp/etc/apt/sources.list.d"
  cp "$LIST" "$apt_tmp/etc/apt/sources.list.d/sonora-backports.list"
  [ -n "$2" ] && printf '%s\n' "$2" > "$apt_tmp/etc/apt/sources.list.d/debian.sources"
  [ -n "${3:-}" ] && printf '%s\n' "$3" > "$apt_tmp/etc/apt/sources.list"
  return 0
}
S="http://snapshot.debian.org/archive"
good822="Types: deb
URIs: $S/debian/$bp_ts/
Suites: trixie trixie-updates
Components: main

Types: deb
URIs: $S/debian-security/$bp_ts/
Suites: trixie-security
Components: main"
apt_case all-snapshot "$good822" "deb $S/debian/$bp_ts/ trixie main"
hook_snapcheck || bad "hook: rejects sources that are all the snapshot"
apt_case live-822 "$good822

Types: deb
URIs: http://deb.debian.org/debian
Suites: trixie
Components: main"
hook_snapcheck && bad "hook: a deb822 stanza on deb.debian.org passes the snapshot check"
apt_case continued-822 "Types: deb
URIs: $S/debian/$bp_ts/
  http://deb.debian.org/debian
Suites: trixie
Components: main"
hook_snapcheck && bad "hook: a second URI on a continuation line passes the snapshot check"
apt_case other-ts-822 "Types: deb
URIs: $S/debian-security/20250101T000000Z/
Suites: trixie-security
Components: main"
hook_snapcheck && bad "hook: a deb822 stanza at another timestamp passes the snapshot check"
apt_case disabled-822 "$good822

Types: deb
URIs: http://deb.debian.org/debian
Suites: trixie
Components: main
Enabled: no"
hook_snapcheck || bad "hook: an 'Enabled: no' stanza, which apt does not read, fails the build"
apt_case live-list "" "deb http://deb.debian.org/debian trixie main"
hook_snapcheck && bad "hook: a one-line deb.debian.org source passes the snapshot check"

# The test containers take the base from the same snapshot, on a pinned image.
KIOSK="$ROOT/tools/test-labwc-kiosk.sh"
digest_of() { sed -nE 's/^FROM (debian:[^ ]*@sha256:[0-9a-f]{64})$/\1/p' "$1" | head -n 1; }
rt_from="$(digest_of "$RUNTIME")"
[ -n "$rt_from" ] || bad "Dockerfile.runtime: base image not pinned by digest"
[ "$(digest_of "$KIOSK")" = "$rt_from" ] || bad "test-labwc-kiosk.sh: base is not Dockerfile.runtime's '$rt_from'"
for f in "$RUNTIME" "$KIOSK"; do
  grep -q 'URIs: http://snapshot.debian.org/archive/debian/' "$f" ||
    bad "${f#"$ROOT"/}: trixie is read from the live mirror, not the snapshot"
  grep -q 'URIs: http://snapshot.debian.org/archive/debian-security/' "$f" ||
    bad "${f#"$ROOT"/}: trixie-security is read from the live mirror, not the snapshot"
done

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

# --- sysctl (audio-stack/sysctl-sonora.conf) ---
# systemd-sysctl logs a bad line and goes on booting, and the kernel quietly
# rewrites some combinations, so each of these leaves the default in force
# with nothing on screen.
SYSCTL="$ROOT/audio-stack/sysctl-sonora.conf"
SYSCTL_IMG="$INC/usr/share/sonora/sysctl-sonora.conf"
declare -A sc=()
if [ ! -f "$SYSCTL" ]; then
  bad "missing audio-stack/sysctl-sonora.conf"
else
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in ''|'#'*|';'*) continue ;; esac
    if [[ "$line" =~ ^([a-z0-9_]+(\.[a-z0-9_]+)+)[[:space:]]*=[[:space:]]*([^[:space:]]+)[[:space:]]*$ ]]; then
      k="${BASH_REMATCH[1]}"
      [ -n "${sc[$k]:-}" ] && bad "sysctl: $k set twice; only the last one counts"
      sc[$k]="${BASH_REMATCH[3]}"
    else
      bad "sysctl: line systemd-sysctl cannot parse: '$line'"
    fi
  done < "$SYSCTL"
  # int_in KEY LO HI: KEY is set to an integer in [LO, HI].
  int_in() {
    local v="${sc[$1]:-}"
    [[ "$v" =~ ^[0-9]+$ ]] && [ "$v" -ge "$2" ] && [ "$v" -le "$3" ] ||
      bad "sysctl: $1='$v', expected an integer in $2..$3"
  }
  int_in vm.swappiness 0 200
  int_in vm.dirty_background_ratio 1 100
  int_in vm.dirty_ratio 1 100
  int_in fs.inotify.max_user_watches 8192 4194304
  # Background at or above the hard limit is halved by the kernel without a word.
  bgr="${sc[vm.dirty_background_ratio]:-}" dr="${sc[vm.dirty_ratio]:-}"
  [[ "$bgr" =~ ^[0-9]+$ && "$dr" =~ ^[0-9]+$ ]] && [ "$bgr" -ge "$dr" ] &&
    bad "sysctl: dirty_background_ratio $bgr is not below dirty_ratio $dr; the kernel uses $dr/2"
  # Writing a *_bytes knob zeroes its *_ratio twin, whichever line comes later.
  for k in vm.dirty_bytes vm.dirty_background_bytes; do
    [ -n "${sc[$k]:-}" ] && bad "sysctl: $k overrides the ratio it pairs with"
  done
  # A misspelt key is a boot-time warning nobody reads. Checked against the
  # running kernel's /proc/sys, which CI's Linux runner has.
  if [ -d /proc/sys/vm ]; then
    for k in "${!sc[@]}"; do
      [ -e "/proc/sys/${k//.//}" ] || bad "sysctl: $k is not a kernel parameter here (/proc/sys/${k//.//})"
    done
  else
    echo "note: no /proc/sys/vm on this host; sysctl key names not checked against a kernel"
  fi
  cmp -s "$SYSCTL" "$SYSCTL_IMG" || bad "sysctl: the image's copy differs from audio-stack/ (tools/sync-audio-stack.sh)"
fi
# systemd-sysctl reads only *.conf in sysctl.d, and a later name wins per key.
grep -Eq '^[[:space:]]*/etc/sysctl\.d/99-[a-z0-9-]+\.conf$' "$HOOK" ||
  bad "hook: sysctl-sonora.conf not installed as /etc/sysctl.d/99-*.conf"
grep -Eq 'install .*/usr/share/sonora/sysctl-sonora\.conf' "$HOOK" || bad "hook does not install sysctl-sonora.conf"
for d in "$INC/etc/sysctl.d" "$INC/usr/lib/sysctl.d" "$INC/lib/sysctl.d"; do
  [ -d "$d" ] || continue
  for f in "$d"/*.conf; do
    [ -e "$f" ] || continue
    for k in "${!sc[@]}"; do
      grep -Eq "^[[:space:]]*-?${k//./[./]}[[:space:]]*=" "$f" && bad "${f#"$ROOT"/} also sets $k"
    done
  done
done

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
  echo "ok: whole archive from snapshot $auto_ts (hook checks .list and deb822); test bases by digest; pipewire $pw_pin, wireplumber $wp_pin; rt kernel; tty1 owned, no VT gettys; sysctl ${#sc[@]} keys; smoketest powers off in order"
fi
exit "$fail"
