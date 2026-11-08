#!/bin/sh
# Unit test of sonora-session-open (ADR-0101 §2) and of sonora-update's gate
# fed by it. Temp dirs, env overrides and a fake systemctl; no Docker, no root.
# Each state is checked twice: the probe's exit code, then that
# `sonora-update apply --dry-run` refuses on open and on cannot-tell and passes
# the gate only on none.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
INC=$ROOT/iso/config/includes.chroot
PROBE=$INC/usr/local/bin/sonora-session-open
LIB=$INC/usr/local/lib/sonora/slots.sh
UPD=$INC/usr/local/bin/sonora-update
SH=$(command -v sh) || exit 1
T=$(mktemp -d) || exit 1
trap 'chmod -R u+rwx "$T" 2>/dev/null; rm -rf "$T"' EXIT
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

[ -f "$PROBE" ] || { echo "FAIL: missing $PROBE" >&2; exit 1; }

# Fake systemctl: answers is-active for one unit with $FAKE_ACTIVE (0 active,
# 3 inactive) and logs its arguments so we can see what was asked.
mkdir -p "$T/bin" "$T/nobin"
cat >"$T/bin/systemctl" <<'EOT'
#!/bin/sh
echo "$*" >>"$FAKE_LOG"
[ "$1" = is-active ] || exit 1
exit "${FAKE_ACTIVE:-3}"
EOT
chmod +x "$T/bin/systemctl"
: >"$T/systemctl.log"

STATE=$T/run/sonora/session-open
reset_state() { chmod -R u+rwx "$T/run" 2>/dev/null; rm -rf "$T/run"; }

probe() {  # probe [PATH]: the probe's exit code in the current state
    PATH=${1:-$T/bin:$PATH} SONORA_SESSION_STATE="$STATE" FAKE_LOG="$T/systemctl.log" \
        "$SH" "$PROBE" >/dev/null 2>&1
    echo $?
}
want() {  # want LABEL WANT-EXIT [PATH]
    _g=$(probe "${3:-}")
    [ "$_g" = "$2" ] || bad "probe $1: exit $_g, want $2"
}

# --- sonora-update fixture (as tools/test-slots.sh) --------------------------
B=$T/bundle
mkdir -p "$B"
printf 'kernel\n' >"$B/vmlinuz"
printf 'initrd\n' >"$B/initrd.img"
printf 'squash\n' >"$B/filesystem.squashfs"
printf '2026.12.1\n' >"$B/VERSION"
(cd "$B" && sha256sum VERSION vmlinuz initrd.img filesystem.squashfs >SHA256SUMS)
printf 'GOOD\n' >"$B/SHA256SUMS.sig"
cat >"$T/bin/fake-verify" <<'EOT'
#!/bin/sh
[ "$(cat "$1")" = GOOD ]
EOT
chmod +x "$T/bin/fake-verify"
printf '2026.11.1\n' >"$T/release"
printf 'ro quiet sonora.slot=a\n' >"$T/cmdline"
E=$T/grubenv
(
    # shellcheck source=/dev/null
    . "$LIB" && slots_env_init "$E"
) || { echo "FAIL: cannot build the grubenv fixture" >&2; exit 1; }

update() {  # update: exit code of apply --dry-run with the real probe
    PATH="$T/bin:$PATH" SONORA_SESSION_STATE="$STATE" FAKE_LOG="$T/systemctl.log" \
        SONORA_LIB="$LIB" SONORA_GRUBENV="$E" SONORA_CMDLINE_FILE="$T/cmdline" \
        SONORA_UPDATES_FLAG="$T/flag" SONORA_SCRATCH="$T/scratch" SONORA_RELEASE_FILE="$T/release" \
        SONORA_SESSION_PROBE="$PROBE" SONORA_VERIFY="$T/bin/fake-verify" SONORA_NO_REMOUNT=1 \
        SONORA_SLOT_DEV_B="$T/slot-b.img" SONORA_HEALTH_CMD=true \
        sh "$UPD" apply --from "$B" --dry-run >"$T/out" 2>&1
    echo $?
}
refuses() {  # refuses LABEL PATTERN
    _g=$(update)
    if [ "$_g" != 2 ] || ! grep -q "$2" "$T/out"; then
        bad "update $1: exit $_g, want 2 with '$2'"
        sed 's/^/    /' "$T/out" >&2
    fi
    [ ! -e "$T/flag" ] || bad "update $1: left the allowed flag"
}
passes() {  # passes LABEL
    _g=$(update)
    if [ "$_g" != 0 ] || ! grep -q 'dry run: nothing' "$T/out"; then
        bad "update $1: exit $_g, want 0 (dry run past the gate)"
        sed 's/^/    /' "$T/out" >&2
    fi
}

# --- open: the file exists, empty or not -------------------------------------
reset_state
mkdir -p "$T/run/sonora"
: >"$STATE"
want "open (empty file)" 0
refuses "open" 'a session is open'
printf '/home/user/Sessions/take.sonora\n' >"$STATE"
want "open (non-empty file)" 0

# --- none: the app started this boot and closed its session ------------------
rm -f "$STATE"
want "none" 1
passes "none"
# The directory alone decides; the opt-in does not turn a none into a doubt.
FAKE_ACTIVE=3 SONORA_ENGINE_UP=1
export FAKE_ACTIVE SONORA_ENGINE_UP
want "none, engine opt-in, unit inactive" 1
unset FAKE_ACTIVE SONORA_ENGINE_UP

# --- dir missing: the app has not run this boot ------------------------------
reset_state
FAKE_ACTIVE=0
export FAKE_ACTIVE
want "dir missing (no opt-in, even with the unit active)" 2
refuses "dir missing" 'cannot tell'
[ -s "$T/systemctl.log" ] && bad "probe asked systemctl without SONORA_ENGINE_UP=1"
unset FAKE_ACTIVE

# --- engine down / up through the opt-in -------------------------------------
SONORA_ENGINE_UP=1
export SONORA_ENGINE_UP
FAKE_ACTIVE=3
export FAKE_ACTIVE
want "engine down" 2
refuses "engine down" 'cannot tell'
grep -q '^is-active --quiet sonora-engine.service$' "$T/systemctl.log" ||
    bad "probe did not ask 'systemctl is-active --quiet sonora-engine.service' (log: $(cat "$T/systemctl.log"))"
FAKE_ACTIVE=0
want "engine up, dir missing" 1
passes "engine up, dir missing"
want "systemctl unavailable" 2 "$T/nobin"
unset FAKE_ACTIVE SONORA_ENGINE_UP

# --- unreadable: the directory exists but cannot be searched -----------------
reset_state
mkdir -p "$T/run/sonora"
: >"$STATE"
chmod 000 "$T/run/sonora"
if [ -x "$T/run/sonora" ]; then
    # Root (or a filesystem without modes) still searches it: nothing to test.
    echo "note: chmod 000 does not hide the directory here (root or no modes); UNVERIFIED: unreadable state dir"
else
    want "unreadable dir" 2
    refuses "unreadable" 'cannot tell'
fi
chmod 755 "$T/run/sonora"
# An unsearchable parent hides the directory itself: still a doubt, not a none.
rm -f "$STATE"
chmod 000 "$T/run"
if [ ! -x "$T/run" ]; then
    want "unsearchable parent" 2
fi
chmod 755 "$T/run"

# --- read-only: the probe never writes ---------------------------------------
reset_state
mkdir -p "$T/run/sonora"
before=$(ls -la "$T/run/sonora")
probe >/dev/null
[ "$before" = "$(ls -la "$T/run/sonora")" ] || bad "probe changed the state dir"

[ "$fail" -eq 0 ] && echo "ok: sonora-session-open states and sonora-update's gate checked against files; UNVERIFIED: the app writing the state file, a real sonora-engine.service"
exit "$fail"
