#!/usr/bin/env bash
# Tests the pure parts of tools/iso-preflight.sh: pin parsing, package list
# extraction, retry with backoff, the dev-snapshot gate and the simulated
# install check. The apt half runs in a container (build-iso.sh --check).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/iso-preflight.sh
. "$ROOT/tools/iso-preflight.sh"
set +e
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
export SONORA_RETRY_DELAY=0

PREF="$ROOT/iso/config/archives/sonora-backports.pref.chroot"
[ "$(pf_pin_version "$PREF" 'wireplumber')" = "0.5.12-1~bpo13+1" ] || bad "wireplumber pin not read"
case "$(pf_pin_version "$PREF" 'pipewire*')" in 1.6.9-2~bpo13+1) ;; *) bad "pipewire pin not read" ;; esac
[ -z "$(pf_pin_version "$PREF" 'nosuch')" ] || bad "pin for an unknown package"
[ -n "$(pf_snapshot "$ROOT/iso/auto/config")" ] || bad "snapshot not read from auto/config"

pkgs="$(pf_packages "$ROOT/iso/config/package-lists")"
printf '%s\n' "$pkgs" | grep -qx linux-image-rt-amd64 || bad "RT kernel not in the checked packages"
printf '%s\n' "$pkgs" | grep -qx pipewire || bad "pipewire not in the checked packages"
printf '%s\n' "$pkgs" | grep -q '#' && bad "a comment leaked into the package list"

# retry: succeeds on the third try, gives up after the limit.
calls=0
# shellcheck disable=SC2329  # called through pf_retry
flaky() { calls=$((calls + 1)); [ "$calls" -ge 3 ]; }
pf_retry 4 flaky 2>/dev/null || bad "retry did not succeed on the 3rd attempt"
[ "$calls" = 3 ] || bad "retry ran $calls times, expected 3"
calls=0
# shellcheck disable=SC2329
never() { calls=$((calls + 1)); return 1; }
pf_retry 3 never 2>/dev/null && bad "retry returned success for a command that always fails"
[ "$calls" = 3 ] || bad "retry ran $calls times, expected 3 attempts"

# dev snapshot: off by default, refused for a release, validated.
[ -z "$(SONORA_DEV_SNAPSHOT='' pf_dev_env)" ] || bad "dev snapshot on by default"
[ "$(SONORA_DEV_SNAPSHOT=20261025T000000Z pf_dev_env)" = 20261025T000000Z ] || bad "dev snapshot not accepted"
SONORA_DEV_SNAPSHOT=20261025T000000Z SONORA_RELEASE=1 pf_dev_env 2>/dev/null && bad "dev snapshot accepted for a release"
SONORA_DEV_SNAPSHOT=yesterday pf_dev_env 2>/dev/null && bad "malformed dev snapshot accepted"

# simulated install: pins honoured, then trixie's PipeWire slipped in, then no RT kernel.
cat >"$tmp/good" <<'EOT'
Inst linux-image-6.12.57+deb13-rt-amd64 (6.12.57-1 Debian:13/stable [amd64])
Inst pipewire (1.6.9-2~bpo13+1 Debian Backports:13-backports [amd64])
Inst wireplumber (0.5.12-1~bpo13+1 Debian Backports:13-backports [amd64])
EOT
pf_check_simulated "$tmp/good" pipewire=1.6.9-2~bpo13+1 wireplumber=0.5.12-1~bpo13+1 2>/dev/null || bad "good simulation rejected"
sed 's/0.5.12-1~bpo13+1/0.5.8-2/' "$tmp/good" >"$tmp/oldwp"
pf_check_simulated "$tmp/oldwp" pipewire=1.6.9-2~bpo13+1 wireplumber=0.5.12-1~bpo13+1 2>"$tmp/err" && bad "trixie's wireplumber passed"
grep -q "wireplumber would install at '0.5.8-2'" "$tmp/err" || bad "message does not name the version apt chose"
grep -v linux-image "$tmp/good" >"$tmp/nokernel"
pf_check_simulated "$tmp/nokernel" pipewire=1.6.9-2~bpo13+1 2>/dev/null && bad "missing RT kernel passed"
grep -v '^Inst pipewire' "$tmp/good" >"$tmp/nopw"
pf_check_simulated "$tmp/nopw" pipewire=1.6.9-2~bpo13+1 2>"$tmp/err" && bad "missing pipewire passed"
grep -q '<not installed>' "$tmp/err" || bad "missing package not reported"

# build-iso.sh wiring.
grep -q -- '--check' "$ROOT/tools/build-iso.sh" || bad "build-iso.sh has no --check"
grep -q 'iso-preflight.sh inside' "$ROOT/tools/build-iso.sh" || bad "build-iso.sh does not run the pre-flight"

[ "$fail" -eq 0 ] && echo "ok: pre-flight pins, package list, retry, dev-snapshot gate, simulated install"
exit "$fail"
