#!/bin/sh
# shellcheck disable=SC2015,SC2016,SC2018,SC2019,SC2094
# Static check of the A/B update wiring (ADR-0044) and the unprivileged slot
# tests. It cannot prove GRUB reads the env or that a machine boots a slot; the
# ok line says so.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
INC=$ROOT/iso/config/includes.chroot
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

for f in usr/local/lib/sonora/slots.sh usr/local/bin/sonora-update \
    usr/local/share/sonora/grub-ab.cfg etc/systemd/system/sonora-confirm.service; do
    [ -f "$INC/$f" ] || bad "missing includes.chroot/$f"
done
[ "$fail" -eq 0 ] || exit 1

UPD=$INC/usr/local/bin/sonora-update
UNIT=$INC/etc/systemd/system/sonora-confirm.service
HOOK=$ROOT/iso/config/hooks/normal/0100-sonora-system.hook.chroot

# The gate of ADR-0035 is the same flag path, and apply never reboots.
grep -q '/var/lib/sonora/updates-allowed' "$UPD" || bad "sonora-update: not the updates-allowed flag of ADR-0035"
grep -Eq '^[^#]*\b(reboot|shutdown|poweroff|systemctl[[:space:]]+(reboot|poweroff))\b' "$UPD" && bad "sonora-update: reboots the machine"
# The signature is checked before any hash or write.
sig=$(grep -n 'slots_verify_signature "\$work"' "$UPD" | head -n 1 | cut -d: -f1)
wr=$(grep -n 'slots_write_image' "$UPD" | head -n 1 | cut -d: -f1)
[ -n "$sig" ] && [ -n "$wr" ] && [ "$sig" -lt "$wr" ] || bad "sonora-update: signature check must come before the write"
# Only https, never plain http.
grep -q "proto '=https'" "$UPD" || bad "sonora-update: fetch is not restricted to https"
grep -Eq 'http://' "$UPD" && bad "sonora-update: plain http"

# The confirm unit is inert on the live stick and waits before confirming.
grep -q '^ConditionKernelCommandLine=sonora.slot' "$UNIT" || bad "sonora-confirm.service: not conditioned on sonora.slot"
grep -q '^ExecStartPre=/bin/sleep' "$UNIT" || bad "sonora-confirm.service: no settle time"
grep -q 'systemctl enable sonora-confirm.service' "$HOOK" || bad "hook does not enable sonora-confirm.service"

# GRUB entry pins its slot by label, so live-boot cannot pick the other one.
CFG=$INC/usr/local/share/sonora/grub-ab.cfg
grep -q 'live-media=/dev/disk/by-label/sonora-\$sonora_boot' "$CFG" || bad "grub-ab.cfg: entry does not pin the slot by label"
grep -q 'sonora.slot=\$sonora_boot' "$CFG" || bad "grub-ab.cfg: entry does not tell the system its slot"

# The installer is not edited by this item; it must be able to source the logic.
if [ -e "$INC/usr/local/bin/sonora-install" ]; then
    grep -q 'sonora/slots.sh' "$INC/usr/local/bin/sonora-install" || echo "note: sonora-install does not use slots.sh yet (follow-up)"
fi

sh "$ROOT/tools/test-slots.sh" || fail=1

[ "$fail" -eq 0 ] && echo "ok: A/B wiring and slot logic checked against files; UNVERIFIED: GRUB reading the env, a real slot boot, a real rollback"
exit "$fail"
