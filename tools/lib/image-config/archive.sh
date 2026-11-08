# Sourced by tools/test-image-config.sh: the archive the image is built from.
# One dated snapshot for everything, exact PipeWire pins stated once and agreed
# everywhere, and the test containers built from the same base.

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
