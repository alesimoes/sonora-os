#!/usr/bin/env bash
# Tests sonora-secureboot-check against a fake root and a stub mokutil.
# No Docker, no root, no firmware.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHECK="$ROOT/iso/config/includes.chroot/usr/local/bin/sonora-secureboot-check"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

stub() { # name, stdout, exit
    printf '#!/bin/sh\necho "%s"\nexit %s\n' "$2" "$3" >"$T/$1"
    chmod +x "$T/$1"
}
stub mok_on "SecureBoot enabled" 0
stub mok_off "SecureBoot disabled" 0
stub mok_none "EFI variables are not supported on this system" 1
stub efi_ok "BootOrder: 0001
Boot0001* sonora" 0

mkroot() { # dir [skip-file]
    local r="$T/$1"
    mkdir -p "$r/boot/efi/EFI/sonora" "$r/boot/efi/EFI/BOOT" "$r/sys/kernel/security"
    : >"$r/boot/efi/EFI/sonora/shimx64.efi"
    : >"$r/boot/efi/EFI/sonora/grubx64.efi"
    : >"$r/boot/efi/EFI/BOOT/BOOTX64.EFI"
    echo "none [integrity] confidential" >"$r/sys/kernel/security/lockdown"
    [ -z "${2:-}" ] || rm "$r/boot/efi/$2"
}

run() { # root mok [args]; sets out, rc, err
    local r="$1" m="$2"; shift 2
    set +e
    out="$(SONORA_ROOT="$T/$r" SONORA_MOKUTIL="$T/$m" SONORA_EFIBOOTMGR="$T/efi_ok" \
        SONORA_UNAME=6.12.0-1-rt-amd64 sh "$CHECK" "$@" 2>"$T/err")"
    rc=$?
    set -e
    err="$(cat "$T/err")"
}
want() { grep -qx "$1" <<<"$out" || bad "$CASE: output lacks $1"; }
rc_is() { [ "$rc" -eq "$1" ] || bad "$CASE: exit $rc, want $1 ($err)"; }

CASE=on; mkroot a
run a mok_on
rc_is 0; want secure_boot=on; want lockdown=integrity; want kernel_rt=yes
want esp_shim=yes; want esp_grub=yes; want removable=yes; want nvram_entry=yes

CASE=off; run a mok_off
rc_is 0; want secure_boot=off

CASE=unsupported; run a mok_none
rc_is 0; want secure_boot=unsupported

CASE=missing-shim; mkroot b EFI/sonora/shimx64.efi
run b mok_on
rc_is 3; want esp_shim=no; grep -q shimx64 <<<"$err" || bad "$CASE: shim not named"

CASE=missing-removable; mkroot c EFI/BOOT/BOOTX64.EFI
run c mok_on
rc_is 3; want removable=no; grep -q BOOTX64 <<<"$err" || bad "$CASE: loader not named"

CASE=require-on-off; run a mok_off --require-on; rc_is 4
CASE=require-on-on; run a mok_on --require-on; rc_is 0
CASE=bad-arg; run a mok_on --bogus; rc_is 2

CASE=non-rt
set +e
out="$(SONORA_ROOT="$T/a" SONORA_MOKUTIL="$T/mok_on" SONORA_EFIBOOTMGR="$T/efi_ok" \
    SONORA_UNAME=6.12.0-1-amd64 sh "$CHECK")"
set -e
want kernel_rt=no

[ $fail -eq 0 ] && echo "secureboot-check: all cases pass"
exit $fail
