#!/bin/sh
# shellcheck disable=SC2015,SC2016,SC2018,SC2019,SC2094
# Unit tests of the A/B slot logic and of sonora-update, against files only: a
# fake grubenv, file images, a fake signature check and a fake session probe.
# Needs no root. Without mke2fs the image write is replaced by a stub and the
# test says so; with it (Debian: e2fsprogs) the slot image is really built.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
INC=$ROOT/iso/config/includes.chroot
LIB=$INC/usr/local/lib/sonora/slots.sh
UPD=$INC/usr/local/bin/sonora-update
T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
eq() { [ "$2" = "$3" ] || bad "$1: got '$2', want '$3'"; }

# shellcheck source=/dev/null
. "$LIB"

# --- layout --------------------------------------------------------------
lay=$(slots_layout_script)
eq "layout: partitions" "$(printf '%s\n' "$lay" | grep -c 'name=')" 4
printf '%s\n' "$lay" | grep -q 'name="sonora-b"' || bad "layout: no slot b"
eq "layout: data takes the rest" "$(printf '%s\n' "$lay" | tail -n 1)" 'type=L, name="sonora-data"'
eq "min disk" "$(slots_min_disk_mib)" $((512 + 2 * 6144 + 4096 + 2))
# Same arithmetic as sfdisk: the fixed partitions must leave room for data.
eq "min disk fits fixed parts" "$(( $(slots_min_disk_mib) - 2 - SLOTS_DATA_MIN_MIB ))" $((512 + 2 * 6144))
slots_layout_script 1000 >/dev/null 2>&1 && bad "layout accepts a 1000 MiB disk"
slots_layout_script "$(slots_min_disk_mib)" >/dev/null 2>&1 || bad "layout refuses the minimum disk"
SLOTS_SLOT_MIB=64 SLOTS_ESP_MIB=512 slots_layout_script 400 >/dev/null 2>&1 && bad "layout accepts 400 MiB with a 512 MiB ESP"
eq "label" "$(slots_label b)" sonora-b
slots_label c >/dev/null 2>&1 && bad "label accepts c"

# --- grubenv -------------------------------------------------------------
E=$T/grubenv
slots_env_init "$E"
eq "env size" "$(wc -c <"$E" | tr -d ' ')" 1024
eq "env header" "$(head -n 1 "$E")" "# GRUB Environment Block"
eq "init slot" "$(slots_env_get "$E" sonora_slot)" a
slots_env_set "$E" sonora_x 7
eq "set keeps size" "$(wc -c <"$E" | tr -d ' ')" 1024
eq "set value" "$(slots_env_get "$E" sonora_x)" 7
slots_env_set "$E" sonora_x 8
eq "set replaces" "$(grep -c '^sonora_x=' "$E")" 1
eq "empty value" "$(slots_env_get "$E" sonora_trial)" ""
eq "cmdline" "$(slots_current 'ro quiet sonora.slot=b x')" b
slots_current 'quiet' >/dev/null && bad "cmdline without slot accepted"

# --- the happy path: update, trial boot, confirm ----------------------------
slots_env_init "$E"
eq "inactive from a" "$(slots_inactive "$E" a)" b
slots_inactive "$E" b >/dev/null 2>&1 && bad "update allowed from a non-committed slot"
slots_mark_written "$E" b
eq "trial armed" "$(slots_env_get "$E" sonora_trial)" b
eq "tries armed" "$(slots_env_get "$E" sonora_tries)" 3
slots_inactive "$E" a >/dev/null 2>&1 && bad "second update during a trial allowed"
eq "boot 1" "$(slots_boot_choice "$E")" b
eq "tries after 1" "$(slots_env_get "$E" sonora_tries)" 2
slots_confirm "$E" a >/dev/null 2>&1 && bad "wrong slot confirmed"
slots_confirm "$E" b
eq "committed" "$(slots_env_get "$E" sonora_slot)" b
eq "trial cleared" "$(slots_env_get "$E" sonora_trial)" ""
eq "boots b after confirm" "$(slots_boot_choice "$E")" b
eq "valid a kept for rollback" "$(slots_env_get "$E" sonora_valid_a)" 1

# --- automatic rollback: the trial never confirms ---------------------------
slots_env_init "$E"
slots_mark_written "$E" b
eq "try 1" "$(slots_boot_choice "$E")" b
eq "try 2" "$(slots_boot_choice "$E")" b
eq "try 3" "$(slots_boot_choice "$E")" b
eq "fallback after 3 failures" "$(slots_boot_choice "$E")" a
eq "trial forgotten" "$(slots_env_get "$E" sonora_trial)" ""
eq "failed slot invalid" "$(slots_env_get "$E" sonora_valid_b)" 0
eq "stays on a" "$(slots_boot_choice "$E")" a
slots_rollback "$E" >/dev/null 2>&1 && bad "rollback to an invalid slot allowed"

# --- manual rollback --------------------------------------------------------
slots_env_init "$E"
slots_mark_written "$E" b
slots_boot_choice "$E" >/dev/null
slots_confirm "$E" b
slots_rollback "$E"
eq "rollback commits a" "$(slots_env_get "$E" sonora_slot)" a
slots_env_init "$E"
slots_mark_written "$E" b
slots_rollback "$E"
eq "rollback cancels a trial" "$(slots_env_get "$E" sonora_trial)" ""
eq "and stays on a" "$(slots_env_get "$E" sonora_slot)" a

# --- grub-ab.cfg agrees with the shell ----------------------------------------
CFG=$INC/usr/local/share/sonora/grub-ab.cfg
for k in sonora_slot sonora_trial sonora_tries sonora_valid_a sonora_valid_b; do
    grep -q "^load_env .*$k" "$CFG" || bad "grub-ab.cfg does not load $k"
done
grep -q 'sonora_tries" = 3 \]; then set sonora_tries=2' "$CFG" || bad "grub-ab.cfg ladder is not 3 -> 2"
eq "SLOTS_TRIES matches the ladder" "$SLOTS_TRIES" 3

# --- bundles ----------------------------------------------------------------
printf 'x\n' >"$T/names"
printf '%s  %s\n' 0000000000000000000000000000000000000000000000000000000000000000 ../evil >"$T/S1"
slots_bundle_names "$T/S1" >/dev/null 2>&1 && bad "path escape accepted"
printf '%s  %s\n' 0000000000000000000000000000000000000000000000000000000000000000 .hidden >"$T/S2"
slots_bundle_names "$T/S2" >/dev/null 2>&1 && bad "dotfile accepted"
slots_newer 2026.11.2 2026.11.1 || bad "newer"
slots_newer 2026.11.10 2026.11.9 || bad "newer is numeric"
slots_newer 1 1 && bad "equal is not newer"
slots_newer 1 2 && bad "older is not newer"

# --- sonora-update against fakes --------------------------------------------
B=$T/bundle
mkdir -p "$B" "$T/bin"
printf 'kernel\n' >"$B/vmlinuz"
printf 'initrd\n' >"$B/initrd.img"
head -c 100000 /dev/zero >"$B/filesystem.squashfs"
printf '2026.12.1\n' >"$B/VERSION"
(cd "$B" && sha256sum VERSION vmlinuz initrd.img filesystem.squashfs >SHA256SUMS)
printf 'GOOD\n' >"$B/SHA256SUMS.sig"
# The fake check: "signed" means the .sig file says GOOD.
cat >"$T/bin/fake-verify" <<'EOT'
#!/bin/sh
[ "$(cat "$1")" = GOOD ]
EOT
printf '#!/bin/sh\nexit 1\n' >"$T/bin/probe-none"
printf '#!/bin/sh\nexit 0\n' >"$T/bin/probe-open"
printf '#!/bin/sh\nexit 9\n' >"$T/bin/probe-broken"
chmod +x "$T/bin/"*
if ! command -v mke2fs >/dev/null 2>&1; then
    # The device is the word after the -d directory.
    printf '#!/bin/sh\nwhile [ $# -gt 0 ]; do [ "$1" = -d ] && { printf "stub\\n" >"$3"; exit 0; }; shift; done\nexit 1\n' >"$T/bin/mke2fs"
    chmod +x "$T/bin/mke2fs"
    echo "note: no mke2fs; slot image write is stubbed"
fi
printf '2026.11.1\n' >"$T/release"
printf 'ro quiet sonora.slot=a\n' >"$T/cmdline"

run() {  # run PROBE ARGS...
    _p=$1
    shift
    PATH="$T/bin:$PATH" SONORA_LIB="$LIB" SONORA_GRUBENV="$E" SONORA_CMDLINE_FILE="$T/cmdline" \
        SONORA_UPDATES_FLAG="$T/flag" SONORA_SCRATCH="$T/scratch" SONORA_RELEASE_FILE="$T/release" \
        SONORA_SESSION_PROBE="$T/bin/$_p" SONORA_VERIFY="$T/bin/fake-verify" SONORA_NO_REMOUNT=1 \
        SONORA_SLOT_DEV_B="$T/slot-b.img" SLOTS_FS_KIB=2048 SONORA_HEALTH_CMD=true \
        sh "$UPD" "$@"
}
expect() {  # expect LABEL WANT-EXIT PROBE ARGS...
    _l=$1 _w=$2
    shift 2
    run "$@" >"$T/out" 2>&1
    _g=$?
    [ "$_g" = "$_w" ] || { bad "$_l: exit $_g, want $_w"; sed 's/^/    /' "$T/out" >&2; }
}

slots_env_init "$E"
expect "session open refuses" 2 probe-open apply --from "$B"
[ ! -e "$T/slot-b.img" ] || bad "image written while a session was open"
eq "env untouched by refusal" "$(slots_env_get "$E" sonora_trial)" ""
expect "no probe refuses" 2 probe-broken apply --from "$B"
expect "dry run" 0 probe-none apply --from "$B" --dry-run
[ ! -e "$T/slot-b.img" ] || bad "dry run wrote the slot"
eq "dry run leaves env" "$(slots_env_get "$E" sonora_trial)" ""
[ ! -e "$T/flag" ] || bad "dry run left the allowed flag"

printf 'BAD\n' >"$B/SHA256SUMS.sig"
expect "bad signature" 2 probe-none apply --from "$B"
[ ! -e "$T/slot-b.img" ] || bad "image written with a bad signature"
printf 'GOOD\n' >"$B/SHA256SUMS.sig"

printf 'tampered\n' >>"$B/filesystem.squashfs"
expect "tampered payload" 2 probe-none apply --from "$B"
[ ! -e "$T/slot-b.img" ] || bad "tampered payload written"
eq "tampered: slot b not valid" "$(slots_env_get "$E" sonora_valid_b)" 0
(cd "$B" && sha256sum VERSION vmlinuz initrd.img filesystem.squashfs >SHA256SUMS)

printf '2026.11.1\n' >"$B/VERSION"
(cd "$B" && sha256sum VERSION vmlinuz initrd.img filesystem.squashfs >SHA256SUMS)
expect "same version refused" 2 probe-none apply --from "$B"
expect "same version, downgrade flag" 0 probe-none apply --from "$B" --allow-downgrade --dry-run
printf '2026.12.1\n' >"$B/VERSION"
(cd "$B" && sha256sum VERSION vmlinuz initrd.img filesystem.squashfs >SHA256SUMS)

expect "apply" 0 probe-none apply --from "$B"
[ -s "$T/slot-b.img" ] || bad "apply wrote no image"
[ ! -e "$T/flag" ] || bad "allowed flag left behind"
eq "apply arms the trial" "$(slots_env_get "$E" sonora_trial)" b
eq "apply arms the counter" "$(slots_env_get "$E" sonora_tries)" 3
eq "apply keeps a committed" "$(slots_env_get "$E" sonora_slot)" a
if command -v debugfs >/dev/null 2>&1 && command -v mke2fs >/dev/null 2>&1; then
    debugfs -R "ls /live" "$T/slot-b.img" 2>/dev/null | grep -q filesystem.squashfs || bad "slot image lacks live/filesystem.squashfs"
fi
expect "second apply during trial" 2 probe-none apply --from "$B" --allow-downgrade
printf 'ro quiet sonora.slot=b\n' >"$T/cmdline"
expect "confirm" 0 probe-none confirm
eq "confirm commits b" "$(slots_env_get "$E" sonora_slot)" b
expect "rollback dry run" 0 probe-none rollback --dry-run
eq "dry rollback keeps b" "$(slots_env_get "$E" sonora_slot)" b
expect "rollback" 0 probe-none rollback
eq "rollback to a" "$(slots_env_get "$E" sonora_slot)" a
printf 'ro quiet\n' >"$T/cmdline"
expect "no slot on cmdline" 2 probe-none apply --from "$B"

[ "$fail" -eq 0 ] && echo "ok: slot logic and sonora-update against files (no real GRUB, no real boot)"
exit "$fail"
