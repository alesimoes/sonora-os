#!/usr/bin/env bash
# Pre-flight for the image build: does every package and pin resolve?
#
#   tools/iso-preflight.sh inside   run in a debian:trixie container (build-iso.sh does)
#
# Why it exists: the first real build died minutes in, at lb bootstrap, because
# snapshot.debian.org answered 503/504 for hours; a pin that no longer resolves
# would have died later still, in the hook. Both are knowable in about a minute
# with `apt-get -s`, before any chroot is unpacked.
#
# Sourced by tools/test-iso-preflight.sh, which tests the functions below.
set -euo pipefail

PF_ROOT="${PF_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"

pf_die() { echo "E: $*" >&2; return 1; }

# pf_retry ATTEMPTS CMD...: backoff 5 s, 15 s, 45 s ... (SONORA_RETRY_DELAY
# is the first step; the tests set 0). snapshot.debian.org is often down for
# minutes, not seconds, so a bounded wait is worth more than failing at once.
pf_retry() {
  local attempts="$1" delay="${SONORA_RETRY_DELAY:-5}" n=1
  shift
  until "$@"; do
    if [ "$n" -ge "$attempts" ]; then
      echo "E: failed after $n attempts: $*" >&2
      return 1
    fi
    echo "retry $n/$attempts in ${delay}s: $*" >&2
    sleep "$delay"
    delay=$((delay * 3))
    n=$((n + 1))
  done
}

# pf_pin_version PREF GLOB: the Pin: version of the stanza whose Package line
# starts with GLOB.
pf_pin_version() {
  awk -v pkg="$2" '
    /^Package:/ { hit = ($2 == pkg) }
    hit && /^Pin: version / { print $3; exit }' "$1"
}

# pf_snapshot AUTO: the timestamp auto/config builds from.
pf_snapshot() { sed -nE 's/^SONORA_SNAPSHOT=([0-9]{8}T[0-9]{6}Z)$/\1/p' "$1"; }

# pf_packages LISTDIR: every package named in the *.list.chroot files, plus the
# RT kernel metapackage that --linux-flavours rt-amd64 makes live-build install.
pf_packages() {
  {
    cat "$1"/*.list.chroot
    echo linux-image-rt-amd64
  } | sed -E 's/#.*//' | tr -s '[:space:]' '\n' | grep -E '^[a-z0-9][a-z0-9+.-]*$' | sort -u
}

# pf_dev_env: the dev-build fallback is a different snapshot timestamp. It is
# never a release: SONORA_RELEASE=1 refuses it. Prints the timestamp or nothing.
pf_dev_env() {
  local ts="${SONORA_DEV_SNAPSHOT:-}"
  [ -n "$ts" ] || return 0
  if [ "${SONORA_RELEASE:-0}" = 1 ]; then
    pf_die "SONORA_DEV_SNAPSHOT is set with SONORA_RELEASE=1: a release is built from the committed snapshot only."
    return 1
  fi
  if ! printf '%s' "$ts" | grep -Eq '^[0-9]{8}T[0-9]{6}Z$'; then
    pf_die "SONORA_DEV_SNAPSHOT='$ts' is not YYYYMMDDTHHMMSSZ."
    return 1
  fi
  printf '%s\n' "$ts"
}

# pf_check_simulated FILE PIN...: FILE is `apt-get -s install` output; each PIN
# is "pkg=version". Fails naming every package that would install at another
# version, which is how an ignored pin shows (apt falls back to trixie's).
pf_check_simulated() {
  local out="$1" rc=0 spec pkg ver got
  shift
  grep -q '^Inst linux-image-.*rt-' "$out" ||
    { pf_die "the RT kernel (linux-image-rt-amd64) is not among the packages apt would install."; rc=1; }
  for spec in "$@"; do
    pkg="${spec%%=*}"
    ver="${spec#*=}"
    got="$(sed -nE "s/^Inst $pkg \(([^ ]+) .*/\1/p" "$out" | head -n 1)"
    if [ "$got" != "$ver" ]; then
      pf_die "$pkg would install at '${got:-<not installed>}', the pin is $ver (backports keeps one upload; is the snapshot right?)." || true
      rc=1
    fi
  done
  return "$rc"
}

# pf_has_version MADISON VERSION: madison output lists " VERSION " as a field.
pf_has_version() { grep -qF " $2 " <<<"$1"; }

# pf_has_candidate POLICY: `apt-cache policy` output names a real Candidate.
pf_has_candidate() { grep -Eq '^ *Candidate: [^(]' <<<"$1"; }

# pf_inside: the container half. PF_ROOT is the repository, read-only.
pf_inside() {
  local src="$PF_ROOT" ts dev pref pwv wpv work=/tmp/pf pkg
  local -a pins
  dev="$(pf_dev_env)" || return 1
  ts="$(pf_snapshot "$src/iso/auto/config")"
  [ -n "$ts" ] || { pf_die "iso/auto/config has no SONORA_SNAPSHOT."; return 1; }
  [ -z "$dev" ] || ts="$dev"
  pref="$src/iso/config/archives/sonora-backports.pref.chroot"
  pwv="$(pf_pin_version "$pref" 'pipewire*')"
  wpv="$(pf_pin_version "$pref" 'wireplumber')"
  [ -n "$pwv" ] && [ -n "$wpv" ] || { pf_die "no PipeWire/WirePlumber pin in $pref."; return 1; }

  rm -rf "$work"
  mkdir -p "$work/state/lists/partial" "$work/cache/archives/partial"
  local base="http://snapshot.debian.org/archive/debian/$ts/"
  {
    echo "deb $base trixie main contrib non-free non-free-firmware"
    echo "deb $base trixie-updates main contrib non-free non-free-firmware"
    echo "deb http://snapshot.debian.org/archive/debian-security/$ts/ trixie-security main contrib non-free non-free-firmware"
    echo "deb $base trixie-backports main"
  } >"$work/sources.list"
  cp "$pref" "$work/prefs"
  local -a apt=(
    -o "Dir::Etc::sourcelist=$work/sources.list" -o "Dir::Etc::sourceparts=-"
    -o "Dir::Etc::preferences=$work/prefs" -o "Dir::Etc::preferencesparts=-"
    -o "Dir::State::lists=$work/state/lists" -o "Dir::Cache=$work/cache"
    -o Acquire::Check-Valid-Until=false -o Acquire::Retries=3
    -o Acquire::http::Timeout=20
  )
  echo "pre-flight: snapshot $ts, pipewire $pwv, wireplumber $wpv"
  pf_retry 4 apt-get "${apt[@]}" update -qq ||
    { pf_die "snapshot $ts is not reachable (snapshot.debian.org down?). For a development build set SONORA_DEV_SNAPSHOT=<another timestamp>; it is never a release."; return 1; }

  # A pin names a version apt must be able to see: say so before the build.
  # Query into a variable, then match: `apt-cache | grep -q` under pipefail
  # fails when grep exits at the first hit and apt-cache dies of SIGPIPE, a
  # false "does not exist" that came and went with output timing.
  for pkg in pipewire pipewire-jack libspa-0.2-modules; do
    pf_has_version "$(apt-cache "${apt[@]}" madison "$pkg")" "$pwv" ||
      { pf_die "$pkg $pwv is not in snapshot $ts."; return 1; }
  done
  pf_has_version "$(apt-cache "${apt[@]}" madison wireplumber)" "$wpv" ||
    { pf_die "wireplumber $wpv is not in snapshot $ts (trixie itself ships an older one; the pin comes from trixie-backports, see iso/README.md Versions)."; return 1; }
  pf_has_candidate "$(apt-cache "${apt[@]}" policy linux-image-rt-amd64)" ||
    { pf_die "linux-image-rt-amd64 does not exist in trixie at snapshot $ts; --linux-flavours rt-amd64 cannot work."; return 1; }

  mapfile -t pins < <(pf_packages "$src/iso/config/package-lists")
  apt-get "${apt[@]}" -s install --no-install-recommends -y "${pins[@]}" >"$work/sim.txt" 2>"$work/sim.err" ||
    { cat "$work/sim.err" >&2; tail -n 20 "$work/sim.txt" >&2; pf_die "the package lists do not resolve together."; return 1; }
  pf_check_simulated "$work/sim.txt" "pipewire=$pwv" "pipewire-jack=$pwv" "libspa-0.2-modules=$pwv" "wireplumber=$wpv"
  echo "pre-flight ok: ${#pins[@]} packages resolve; pins and RT kernel present."
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  case "${1:-}" in
    inside) pf_inside ;;
    *) echo "usage: $0 inside   (build-iso.sh --check runs it in a container)" >&2; exit 2 ;;
  esac
fi
