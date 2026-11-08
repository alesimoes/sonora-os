#!/bin/sh
# Unit tests of the installed disk layout arithmetic (ADR-0090), with fake RAM
# and disk sizes. Pure: no root, no Docker, no device. Expected numbers are
# written out here, not derived from the library's constants, so a changed
# constant fails the test instead of moving it.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LIB=$ROOT/iso/config/includes.chroot/usr/local/lib/sonora/install-layout.sh
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
eq() { [ "$2" = "$3" ] || bad "$1: got '$2', want '$3'"; }
key() { printf '%s\n' "$1" | sed -n "s/^$2=//p"; }
script_of() { printf '%s\n' "$1" | sed -n '/^label: /,$p'; }

# shellcheck source=/dev/null
. "$LIB"

# --- swap: clamp(RAM/2, 2048, 8192), rounded down to 256 -----------------
eq "swap 2 GiB" "$(layout_swap_mib 2048)" 2048
eq "swap 4 GiB" "$(layout_swap_mib 4096)" 2048
eq "swap 16 GiB" "$(layout_swap_mib 16384)" 8192
eq "swap 64 GiB" "$(layout_swap_mib 65536)" 8192
eq "swap 6000 MiB rounds down" "$(layout_swap_mib 6000)" 2816
eq "swap 12 GiB" "$(layout_swap_mib 12288)" 6144
layout_swap_mib 4G >/dev/null 2>&1 && bad "swap accepts '4G'"

# --- plan, EFI, 16 GiB RAM, 256 GiB disk ----------------------------------
p=$(layout_plan 262144 16384 1) || bad "plan refuses 256 GiB"
eq "efi bios_mib" "$(key "$p" bios_mib)" 0
eq "efi esp_mib" "$(key "$p" esp_mib)" 512
eq "efi root_mib" "$(key "$p" root_mib)" 16384
eq "efi swap_mib" "$(key "$p" swap_mib)" 8192
eq "efi data_mib" "$(key "$p" data_mib)" $((262144 - 2 - 512 - 16384 - 8192))
eq "efi esp_part" "$(key "$p" esp_part)" 1
eq "efi data_part" "$(key "$p" data_part)" 4
s=$(script_of "$p")
want="label: gpt
size=512MiB, type=U, name=ESP
size=16384MiB, type=L, name=sonora-root
size=8192MiB, type=S, name=sonora-swap
type=L, name=sonora-data"
eq "efi sfdisk script" "$s" "$want"

# --- plan, BIOS: boot partition first, everything shifts by one ----------
p=$(layout_plan 262144 4096 0) || bad "plan refuses 256 GiB (BIOS)"
eq "bios bios_mib" "$(key "$p" bios_mib)" 1
eq "bios swap_mib" "$(key "$p" swap_mib)" 2048
eq "bios data_mib" "$(key "$p" data_mib)" $((262144 - 2 - 1 - 512 - 16384 - 2048))
eq "bios esp_part" "$(key "$p" esp_part)" 2
eq "bios root_part" "$(key "$p" root_part)" 3
eq "bios swap_part" "$(key "$p" swap_part)" 4
eq "bios data_part" "$(key "$p" data_part)" 5
s=$(script_of "$p")
eq "bios first partition" "$(printf '%s\n' "$s" | sed -n 2p)" \
    "size=1MiB, type=21686148-6449-6E6F-744E-656564454649, name=bios"
eq "bios partitions" "$(printf '%s\n' "$s" | grep -c 'name=')" 5
eq "bios data last" "$(printf '%s\n' "$s" | tail -n 1)" "type=L, name=sonora-data"

# --- smallest accepted disk, and one MiB under it ------------------------
# ram efi smallest: GPT 2 + (BIOS 1) + ESP 512 + root 16384 + swap + data 8192
for c in "2048 1 27138" "2048 0 27139" "4096 1 27138" "16384 1 33282" \
    "65536 0 33283"; do
    # shellcheck disable=SC2086 # split the case into its three fields
    set -- $c
    eq "min disk ram=$1 efi=$2" "$(layout_min_disk_mib "$1" "$2")" "$3"
    p=$(layout_plan "$3" "$1" "$2") || bad "plan refuses smallest disk ram=$1 efi=$2"
    eq "smallest data ram=$1 efi=$2" "$(key "$p" data_mib)" 8192
    out=$(layout_plan $(($3 - 1)) "$1" "$2" 2>"${TMPDIR:-/tmp}/til-err.$$")
    rc=$?
    eq "one under rc ram=$1 efi=$2" "$rc" 4
    eq "one under stdout ram=$1 efi=$2" "$out" ""
    grep -q 'disk too small' "${TMPDIR:-/tmp}/til-err.$$" ||
        bad "one under ram=$1 efi=$2: no reason on stderr"
done
rm -f "${TMPDIR:-/tmp}/til-err.$$"

# --- bad arguments -------------------------------------------------------
layout_plan 262144 4096 2 >/dev/null 2>&1
eq "efi=2 rc" "$?" 2
layout_plan 256G 4096 1 >/dev/null 2>&1
eq "disk '256G' rc" "$?" 2

# --- root fit: squashfs size x 1.5 must fit -------------------------------
layout_fits_root 4096 16384 2>/dev/null || bad "4 GiB image refused"
layout_fits_root 10922 16384 2>/dev/null || bad "10922 MiB image refused (16383 needed)"
layout_fits_root 10923 16384 2>/dev/null
eq "10923 MiB image rc" "$?" 4
layout_fits_root 10923 16384 2>&1 | grep -q 'root has 16384' ||
    bad "fit refusal: no reason on stderr"

[ $fail -eq 0 ] && echo "test-install-layout: all passed"
exit $fail
