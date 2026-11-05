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
# The kiosk test's image is built on main from tools/Dockerfile.labwc.
KIOSK="$ROOT/tools/Dockerfile.labwc"
digest_of() { sed -nE 's/^FROM (debian:[^ ]*@sha256:[0-9a-f]{64})$/\1/p' "$1" | head -n 1; }
rt_from="$(digest_of "$RUNTIME")"
[ -n "$rt_from" ] || bad "Dockerfile.runtime: base image not pinned by digest"
[ "$(digest_of "$KIOSK")" = "$rt_from" ] || bad "Dockerfile.labwc: base is not Dockerfile.runtime's '$rt_from'"
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
  # systemd 257 runs OnFailure= on every failure, restarted or not; without
  # this guard a single kill -9 of the UI ends the display for the boot.
  grep -qx 'ExecCondition=/usr/local/bin/sonora-display-gave-up' "$FALLBACK" || bad "fallback runs on a failure that systemd is about to restart (no ExecCondition=sonora-display-gave-up)"
  grep -qx 'StartLimitIntervalSec=0' "$FALLBACK" || bad "fallback has a start limit; its skipped runs would use it up before the one that matters"
  grep -q 'auto-restart' "$INC/usr/local/bin/sonora-display-gave-up" 2>/dev/null || bad "sonora-display-gave-up does not look at auto-restart"
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

# --- display stack staged into the image (ADR-0010 stage 1) ---
# Each is a way sonora.gui boots to a blank screen with no build error.
for p in libpam-systemd labwc xwayland qt6-wayland libqt6quick6 qml6-module-qtquick; do
  grep -qx "$p" "$PKGS" || bad "package list lacks $p (display block)"
done
# seatd would satisfy libseat1 and give labwc no logind session.
grep -Eqx 'seatd|libseat1-seatd' "$PKGS" && bad "package list installs seatd; ADR-0010 section 3 wants logind"
for f in usr/share/sonora/labwc/rc.xml usr/share/sonora/labwc/menu.xml etc/pam.d/sonora-ui \
         usr/local/bin/sonora-compositor usr/local/bin/sonora-wait-for-display \
         etc/systemd/system/sonora-compositor.service etc/systemd/system/sonora-ui.service; do
  [ -f "$INC/$f" ] || bad "includes.chroot lacks $f"
done
for b in sonora-compositor sonora-wait-for-display sonora-display-gave-up; do
  grep -qx "chmod 0755 /usr/local/bin/$b" "$HOOK" || bad "hook does not chmod $b"
done
grep -qx 'systemctl enable sonora-compositor.service' "$HOOK" || bad "hook does not enable the compositor unit"
grep -q 'sonora-ui' "$HOOK" || bad "hook does not handle sonora-ui"
COMP="$INC/etc/systemd/system/sonora-compositor.service"
UIU="$INC/etc/systemd/system/sonora-ui.service"
if [ -f "$COMP" ] && [ -f "$UIU" ]; then
  grep -qx 'ConditionKernelCommandLine=sonora.gui' "$COMP" || bad "compositor unit is not gated on sonora.gui"
  grep -qx 'ConditionKernelCommandLine=sonora.gui' "$UIU" || bad "UI unit is not gated on sonora.gui"
  grep -qx 'PAMName=sonora-ui' "$COMP" || bad "compositor unit has no PAM session (no logind seat)"
  grep -qx 'Conflicts=getty@tty1.service' "$COMP" || bad "compositor unit does not displace the tty1 getty"
  grep -qx 'OnFailure=sonora-display-fallback.service' "$COMP" || bad "compositor failure does not start the fallback"
  grep -qx 'OnFailure=sonora-display-fallback.service' "$UIU" || bad "UI failure does not start the fallback"
  grep -qx 'BindsTo=sonora-compositor.service' "$UIU" || bad "UI unit is not bound to the compositor"
  grep -qx 'RestartPreventExitStatus=3' "$UIU" || bad "UI unit retries the 'unfit display' exit"
fi
# --- kiosk stage 2: the engine as its own unit, editors on labwc ---
# The engine and the display stack must fail and restart separately: a
# compositor or UI crash that restarts the engine ends the take in progress.
ENG="$INC/etc/systemd/system/sonora-engine.service"
RUN="$INC/usr/local/bin/sonora-engine-run"
for f in "$ENG" "$RUN" "$INC/usr/share/sonora/labwc/autostart"; do
  [ -f "$f" ] || bad "kiosk stage 2 lacks ${f#"$INC"/}"
done
if [ -f "$ENG" ] && [ -f "$UIU" ]; then
  grep -qx 'ConditionKernelCommandLine=sonora.gui' "$ENG" || bad "engine unit is not gated on sonora.gui"
  grep -qx 'ConditionPathExists=!/run/sonora-display-failed' "$ENG" || bad "engine unit does not stand down after the display fallback"
  # Not Conflicts=: the session is enabled at boot, and a Conflicts= between two
  # units in one boot transaction made systemd drop the session (measured in
  # tools/test-display-fallback.sh). The fallback stops the engine by hand.
  grep -Eq '^Conflicts=.*sonora-(session|engine)' "$ENG" "$UNIT" && bad "Conflicts= between the engine and the session unit"
  grep -Eq '^ExecStart=-/usr/bin/systemctl stop .*sonora-engine\.service' "$FALLBACK" || bad "fallback does not stop the display stack's engine"
  grep -qx 'Restart=always' "$ENG" || bad "engine unit does not restart"
  grep -qx 'StartLimitIntervalSec=0' "$ENG" || bad "engine unit gives up after a crash loop"
  grep -qx 'LimitRTPRIO=95' "$ENG" || bad "engine unit has no RT priority grant"
  grep -qx 'LimitMEMLOCK=infinity' "$ENG" || bad "engine unit has no memlock grant"
  grep -qx 'ExecStart=/usr/local/bin/sonora-engine-run' "$ENG" || bad "engine unit does not start the launcher"
  grep -Eq '^(BindsTo|Requires|PartOf)=' "$ENG" && bad "engine unit is tied to another unit's stop; a UI or labwc crash would end the take"
  grep -qx 'Wants=sonora-engine.service' "$UIU" || bad "UI unit does not pull in the engine"
  grep -qx 'After=sonora-engine.service' "$UIU" || bad "UI unit is not ordered after the engine"
  grep -Eq '^(BindsTo|Requires)=.*sonora-engine' "$UIU" && bad "UI unit is bound to the engine; the engine's restart would restart the UI"
  grep -Eq '^ExecStart=/usr/local/bin/sonora-ui --kiosk$' "$UIU" || bad "UI unit does not run the real-engine kiosk (--demo left in?)"
  grep -qx 'Restart=always' "$UIU" || bad "UI unit does not restart"
fi
grep -qx 'chmod 0755 /usr/local/bin/sonora-engine-run' "$HOOK" || bad "hook does not chmod sonora-engine-run"
grep -qx 'chmod 0755 /usr/share/sonora/labwc/autostart' "$HOOK" || bad "hook does not chmod the labwc autostart"
# The engine is pulled in by the UI; enabling it would start it without sonora.gui checks.
grep -q 'systemctl enable sonora-engine.service' "$HOOK" && bad "hook enables the engine unit; the UI unit pulls it in"
grep -q 'display.env' "$RUN" 2>/dev/null || bad "engine launcher does not read the display file"
grep -q 'display.env' "$INC/usr/share/sonora/labwc/autostart" 2>/dev/null || bad "labwc autostart does not write the display file"
grep -Eq -- '--headless' "$RUN" 2>/dev/null || bad "engine launcher is not headless"
RC="$INC/usr/share/sonora/labwc/rc.xml"
# Editors are every window but the UI: no rule may strip their title bar, the
# operator's only way to move or close a misbehaving one.
if [ -f "$RC" ]; then
  grep -c 'serverDecoration="no"' "$RC" | grep -qx 1 || bad "rc.xml undecorates more than one window rule"
  grep -q 'windowRule identifier="sonora-ui" serverDecoration="no"' "$RC" || bad "rc.xml: the only undecorated window is not sonora-ui"
  grep -q '<policy>center</policy>' "$RC" || bad "rc.xml: editors are not centred"
  grep -q '<repeatRate>' "$RC" || bad "rc.xml sets no key repeat"
fi
grep -q 'XKB_DEFAULT_LAYOUT' "$INC/usr/local/bin/sonora-compositor" || bad "compositor wrapper does not map the keyboard layout"

grep -q '^session .*pam_systemd.so' "$INC/etc/pam.d/sonora-ui" 2>/dev/null || bad "pam sonora-ui does not open a logind session"
grep -Ev '^[[:space:]]*#' "$INC/etc/pam.d/sonora-ui" 2>/dev/null | grep -q pam_limits && bad "pam sonora-ui loads pam_limits; the UI would inherit rtprio 95"

# sonora-ui is built by tools/build-ui.sh and staged on request.
BUILD_ISO="$ROOT/tools/build-iso.sh"
grep -q 'SONORA_WITH_UI' "$BUILD_ISO" || bad "build-iso.sh cannot stage sonora-ui"
# shellcheck disable=SC2016 # a literal $UI in the script text
grep -q 'install -m 0755 "\$UI" "\$STAGE/sonora-ui"' "$BUILD_ISO" || bad "build-iso.sh does not install sonora-ui into includes.chroot"

# --- the smoke test's display checks and the QEMU modes that reach them ---
# The recovery and fallback checks exist only if a boot asks for them, so a
# harness that never passes the parameter would turn them into dead code.
BOOT_QEMU="$ROOT/tools/test-boot-qemu.sh"
if [ -f "$SMOKE_SCRIPT" ]; then
  grep -q 'first frame after' "$SMOKE_SCRIPT" || bad "smoketest does not look for the UI's first-frame line"
  grep -q 'kill_and_recover sonora-ui.service' "$SMOKE_SCRIPT" || bad "smoketest does not SIGKILL the UI"
  grep -q 'kill_and_recover sonora-compositor.service' "$SMOKE_SCRIPT" || bad "smoketest does not SIGKILL labwc"
  grep -qx 'RECOVER_MS=3000' "$SMOKE_SCRIPT" || bad "smoketest recovery limit is not ADR-0010's 3 s"
  grep -q 'sonora\.break_ui' "$SMOKE_SCRIPT" || bad "smoketest cannot inject a repeated UI failure"
  grep -q 'fgconsole' "$SMOKE_SCRIPT" || bad "smoketest does not check the foreground VT after the fallback"
  grep -q 'find /dev/dri' "$SMOKE_SCRIPT" || bad "smoketest does not record the render node situation"
fi
grep -q -- '--gui)' "$BOOT_QEMU" || bad "test-boot-qemu.sh has no --gui mode"
grep -q -- '--gui-broken)' "$BOOT_QEMU" || bad "test-boot-qemu.sh has no --gui-broken mode"
grep -q 'sonora.gui sonora.break_ui' "$BOOT_QEMU" || bad "--gui-broken does not pass sonora.break_ui"
grep -q 'iso size' "$BOOT_QEMU" || bad "test-boot-qemu.sh does not record the ISO size"

# --- Windows plugins: Wine and yabridge in the image (ADR-0017 Y6) ---
YB_HOOK="$ROOT/iso/config/hooks/normal/0200-sonora-yabridge.hook.chroot"
YB_LIST="$ROOT/iso/config/package-lists/windows-plugins.list.chroot"
YB_UNIT="$INC/etc/systemd/system/sonora-windows-plugins.service"
YB_SCRIPT="$INC/usr/local/bin/sonora-windows-plugins"
YB_DOCKER="$ROOT/tools/Dockerfile.yabridge"
for f in "$YB_HOOK" "$YB_LIST" "$YB_UNIT" "$YB_SCRIPT" "$YB_DOCKER"; do
  [ -f "$f" ] || bad "missing ${f#"$ROOT"/}"
done
if [ -f "$YB_HOOK" ] && [ -f "$YB_DOCKER" ]; then
  for k in VERSION SHA256; do
    h="$(sed -nE "s/^SONORA_YABRIDGE_$k=([^[:space:]]+)$/\1/p" "$YB_HOOK")"
    d="$(sed -nE "s/^ARG YABRIDGE_$k=([^[:space:]]+)$/\1/p" "$YB_DOCKER")"
    [ -n "$h" ] || bad "yabridge hook: no SONORA_YABRIDGE_$k pin"
    [ "$h" = "$d" ] || bad "yabridge $k: hook '$h' differs from Dockerfile.yabridge '$d'"
  done
  h="$(sed -nE 's/^SONORA_YABRIDGE_SHA256=(.*)$/\1/p' "$YB_HOOK")"
  printf '%s' "$h" | grep -Eq '^[0-9a-f]{64}$' || bad "yabridge hook: SHA-256 pin is not 64 hex digits"
  grep -q 'sha256sum -c' "$YB_HOOK" || bad "yabridge hook does not verify the tarball"
  grep -Eq 'SONORA_WINE_SERIES=[0-9]+\.[0-9]+' "$YB_HOOK" || bad "yabridge hook does not pin the Wine series"
  grep -q 'chmod 0755 /usr/local/bin/sonora-windows-plugins' "$YB_HOOK" || bad "yabridge hook does not chmod the setup script"
  grep -q 'systemctl enable sonora-windows-plugins.service' "$YB_HOOK" || bad "yabridge hook does not enable the setup unit"
fi
if [ -f "$YB_LIST" ]; then
  grep -Eqx 'wine64' "$YB_LIST" || bad "windows-plugins list lacks wine64"
  grep -Eqx 'wine32|libwine:i386' "$YB_LIST" && bad "windows-plugins list pulls 32-bit Wine"
fi
if [ -f "$YB_UNIT" ]; then
  grep -Eqx 'User=sonora' "$YB_UNIT" || bad "windows-plugins unit does not run as the operator"
  grep -Eq '^Type=oneshot' "$YB_UNIT" || bad "windows-plugins unit is not oneshot"
  grep -Eq '^(Before|Requires|RequiredBy)=.*sonora-session' "$YB_UNIT" && bad "windows-plugins unit is ordered against the engine"
  grep -Eq '^LimitRTPRIO' "$YB_UNIT" && bad "windows-plugins unit asks for real time"
fi
if [ -f "$YB_SCRIPT" ]; then
  grep -q 'yabridgectl sync' "$YB_SCRIPT" || bad "sonora-windows-plugins does not run yabridgectl sync"
  grep -q 'wineboot --init' "$YB_SCRIPT" || bad "sonora-windows-plugins does not create the prefix"
  grep -v "^[[:space:]]*#" "$YB_SCRIPT" | grep -q "set --path" && bad "script uses yabridgectl set --path (panics in 5.1.1)"
  # Behaviour, without Wine: --check changes nothing and never fails.
  chk_home="$(mktemp -d)"
  out="$(HOME="$chk_home" PATH="$chk_home/bin:$PATH" sh -c '
    mkdir -p "$HOME/bin"; for t in wineboot yabridgectl; do printf "#!/bin/sh
exit 0
" > "$HOME/bin/$t"; chmod +x "$HOME/bin/$t"; done
    sh "'"$YB_SCRIPT"'" --check "$HOME/p"' 2>&1)" || bad "sonora-windows-plugins --check failed: $out"
  printf '%s
' "$out" | grep -q 'would be created' || bad "--check does not report the missing prefix: $out"
  [ -e "$chk_home/.wine" ] && bad "--check created the prefix"
  rm -rf "$chk_home"
fi

# --- every binary the image runs is in the image ---
# A unit or a launcher that names /usr/local/bin/<x> for an <x> that nothing
# stages boots into a restart loop (or, for the plugin host, into an engine
# that silently has no inserts). Each such path must be a file tracked in
# includes.chroot or listed in iso/staged.list, which build-iso.sh stages.
STAGED="$ROOT/iso/staged.list"
BIN="$INC/usr/local/bin"
if [ ! -f "$STAGED" ]; then
  bad "missing iso/staged.list"
else
  staged_paths="$(awk '!/^[[:space:]]*(#|$)/ { print $1 }' "$STAGED")"
  required_paths="$(awk '$2 == "required" { print $1 }' "$STAGED")"
  refs="$(grep -rhEv '^[[:space:]]*#' "$INC/etc/systemd/system" "$BIN" "$HOOK" 2>/dev/null |
    grep -Eo '/usr/local/bin/[A-Za-z0-9._-]+' | sort -u || true)"
  [ -n "$refs" ] || bad "found no /usr/local/bin reference in the units and launchers: the check is blind"
  for p in $refs; do
    [ -f "$INC$p" ] && continue
    printf '%s\n' "$staged_paths" | grep -qxF "$p" || bad "$p is run by a unit or launcher but is neither tracked in includes.chroot nor in iso/staged.list"
  done
  # A listed binary needs a way in: build-iso.sh must name it, or the producer
  # it comes from (sonora-plugin-host and its siblings are installed by a loop).
  for p in $required_paths; do
    case "$p" in
      */) continue ;;
    esac
    n="$(basename "$p")"
    grep -q "$n" "$BUILD_ISO" || grep -q "$n" "$ROOT/tools/build-plugins.sh" || bad "$n is required by iso/staged.list but build-iso.sh never stages it"
  done
  for p in $required_paths; do
    case "$p" in
      /usr/local/bin/*) printf '%s\n' "$refs" | grep -qxF "$p" || [ "$p" = /usr/local/bin/sonora-tui ] || bad "$p is staged but nothing runs it" ;;
    esac
  done
  printf '%s\n' "$staged_paths" | grep -qxF /usr/local/bin/sonora-plugin-host || bad "iso/staged.list does not stage sonora-plugin-host"
  printf '%s\n' "$staged_paths" | grep -qxF /usr/local/lib/lv2/ || bad "iso/staged.list does not stage the LV2 bundles"
  printf '%s\n' "$staged_paths" | grep -qxF /usr/local/lib/vst3/ || bad "iso/staged.list does not stage the VST3 bundles"
  printf '%s\n' "$staged_paths" | grep -qxF /usr/share/sonora/keymaps/ || bad "iso/staged.list does not stage the keymap profiles"
fi
grep -q -- '--list-staged' "$BUILD_ISO" || bad "build-iso.sh has no --list-staged"
grep -q -- '--stage' "$ROOT/tools/build-plugins.sh" || bad "build-plugins.sh has no --stage mode for the image"
grep -q 'build-plugins.sh" --stage' "$BUILD_ISO" || bad "build-iso.sh does not build the plugin host and bundles"
grep -q 'usr/local/lib/lv2' "$BUILD_ISO" || bad "build-iso.sh does not stage the LV2 bundles"
grep -q 'usr/local/lib/vst3' "$BUILD_ISO" || bad "build-iso.sh does not stage the VST3 bundles"
grep -qE 'SONORA_WITH_UI:-1' "$BUILD_ISO" || bad "build-iso.sh no longer stages sonora-ui by default"
ls "$ROOT"/keymaps/*.json >/dev/null 2>&1 || bad "no keymaps/*.json to stage"
# Runtime libraries of what is staged, from the package list.
for pkg in liblilv-0-0 libx11-6 qt6-wayland libqt6quick6 qml6-module-qtquick; do
  grep -qx "$pkg" "$PKGS" || bad "package list lacks $pkg, which sonora-plugin-host or sonora-ui needs at run time"
done

# --- the terminal UI on tty2 (ADR-0010 stage 2) ---
TUIU="$INC/etc/systemd/system/sonora-tui.service"
if [ ! -f "$TUIU" ]; then
  bad "missing sonora-tui.service"
else
  grep -qx 'ExecStart=/usr/local/bin/sonora-tui' "$TUIU" || bad "tui unit does not run sonora-tui"
  grep -qx 'TTYPath=/dev/tty2' "$TUIU" || bad "tui unit is not on tty2"
  grep -qx 'Conflicts=getty@tty2.service' "$TUIU" || bad "tui unit does not displace the tty2 getty"
  grep -qx 'Restart=always' "$TUIU" || bad "tui unit does not restart"
  grep -qx 'StandardInput=tty' "$TUIU" || bad "tui unit has no tty input"
  grep -qx 'ConditionKernelCommandLine=sonora.gui' "$TUIU" || bad "tui unit runs without the display stack"
  grep -qx 'ConditionKernelCommandLine=!sonora.install' "$TUIU" || bad "tui unit fights the installer for tty2"
  grep -qx 'Wants=sonora-engine.service' "$TUIU" || bad "tui unit does not pull in the engine"
  grep -qx 'systemctl enable sonora-tui.service' "$HOOK" || bad "hook does not enable the tui unit"
fi

if [ "$fail" -eq 0 ]; then
  echo "ok: whole archive from snapshot $auto_ts (hook checks .list and deb822); test bases by digest; pipewire $pw_pin, wireplumber $wp_pin; rt kernel; tty1 owned, no VT gettys; sysctl ${#sc[@]} keys; smoketest powers off in order"
fi
exit "$fail"
