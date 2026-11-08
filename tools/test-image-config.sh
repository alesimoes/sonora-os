#!/usr/bin/env bash
# Static check of the live-build configuration: the PipeWire pins agree
# everywhere they are stated, the RT kernel is the bootstrap flavour, and
# nothing leaves the operator at a login prompt or a shell. Each check is a
# way the image boots wrong without the build saying so.
#
# The checks live in tools/lib/image-config/, one file per domain, sourced
# here in order: they share these paths, bad() and a few results (the
# snapshot timestamp, the pins, the sysctl keys) that later parts and the
# summary line use. Run shellcheck with -x so the parts are checked in this
# context, as CI and tools/pre-pr.sh do.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIB="$ROOT/tools/lib/image-config"
AUTO="$ROOT/iso/auto/config"
LIST="$ROOT/iso/config/archives/sonora-backports.list.chroot"
PREF="$ROOT/iso/config/archives/sonora-backports.pref.chroot"
HOOK="$ROOT/iso/config/hooks/normal/0100-sonora-system.hook.chroot"
PKGS="$ROOT/iso/config/package-lists/sonora.list.chroot"
INC="$ROOT/iso/config/includes.chroot"
UNIT="$INC/etc/systemd/system/sonora-session.service"
LOGIND="$INC/etc/systemd/logind.conf.d/sonora.conf"
RUNTIME="$ROOT/tools/Dockerfile.runtime"
FALLBACK="$INC/etc/systemd/system/sonora-display-fallback.service"
BUILD_ISO="$ROOT/tools/build-iso.sh"
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
for f in "$AUTO" "$LIST" "$PREF" "$HOOK" "$PKGS" "$UNIT" "$LOGIND" "$RUNTIME" "$BUILD_ISO"; do
  [ -f "$f" ] || bad "missing ${f#"$ROOT"/}"
done
[ "$fail" -eq 0 ] || exit 1

# shellcheck source=tools/lib/image-config/archive.sh
. "$LIB/archive.sh"
# shellcheck source=tools/lib/image-config/packages.sh
. "$LIB/packages.sh"
# shellcheck source=tools/lib/image-config/session.sh
. "$LIB/session.sh"
# shellcheck source=tools/lib/image-config/sysctl.sh
. "$LIB/sysctl.sh"
# shellcheck source=tools/lib/image-config/display.sh
. "$LIB/display.sh"
# shellcheck source=tools/lib/image-config/windows-plugins.sh
. "$LIB/windows-plugins.sh"
# shellcheck source=tools/lib/image-config/staged.sh
. "$LIB/staged.sh"

# --- the stock effects set: list equals manifest (ADR-0076) ---
bash "$ROOT/tools/test-stock-set.sh" >/dev/null || bad "tools/test-stock-set.sh failed (stock list, manifest, licences or names)"

if [ "$fail" -eq 0 ]; then
  echo "ok: whole archive from snapshot $auto_ts (hook checks .list and deb822); test bases by digest; pipewire $pw_pin, wireplumber $wp_pin; rt kernel; firmware by name, no build-time download; tty1 owned, no VT gettys; sysctl ${#sc[@]} keys; smoketest powers off in order"
fi
exit "$fail"
