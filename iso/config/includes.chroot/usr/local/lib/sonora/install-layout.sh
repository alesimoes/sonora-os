#!/bin/sh
# Installed disk layout arithmetic (ADR-0090 sections 1-3 and 7). Sourced by
# sonora-install. Pure functions over sizes in MiB: nothing here opens a
# device, so tools/test-install-layout.sh drives all of it with fake sizes.
#
# Order on disk: (BIOS boot, BIOS only,) ESP, sonora-root, sonora-swap,
# sonora-data. Data is last so it takes whatever is left.

LAYOUT_BIOS_MIB=1
LAYOUT_ESP_MIB=512
LAYOUT_ROOT_MIB=16384
LAYOUT_DATA_MIN_MIB=8192
# 1 MiB before the first partition (alignment) and room for the backup GPT
# at the end; sfdisk aligns to 1 MiB, so the backup header costs a whole one.
LAYOUT_GPT_MIB=2
LAYOUT_SWAP_MIN_MIB=2048
LAYOUT_SWAP_MAX_MIB=8192
LAYOUT_SWAP_STEP_MIB=256
LAYOUT_BIOS_TYPE=21686148-6449-6E6F-744E-656564454649

layout_err() { echo "install-layout: $*" >&2; }

layout_is_count() {
    case "$1" in '' | *[!0-9]*) return 1 ;; *) return 0 ;; esac
}

# layout_swap_mib RAM_MIB: clamp(RAM/2, 2048, 8192), rounded down to 256.
# An OOM kill of the engine loses the take; a swapping box only slows down.
layout_swap_mib() {
    layout_is_count "$1" || { layout_err "RAM size '$1' is not a MiB count"; return 2; }
    _ls_half=$(($1 / 2))
    [ "$_ls_half" -lt "$LAYOUT_SWAP_MIN_MIB" ] && _ls_half=$LAYOUT_SWAP_MIN_MIB
    [ "$_ls_half" -gt "$LAYOUT_SWAP_MAX_MIB" ] && _ls_half=$LAYOUT_SWAP_MAX_MIB
    echo $((_ls_half / LAYOUT_SWAP_STEP_MIB * LAYOUT_SWAP_STEP_MIB))
}

# layout_bios_mib EFI: the BIOS boot partition exists only where GRUB needs it.
layout_bios_mib() {
    if [ "$1" -eq 1 ]; then echo 0; else echo "$LAYOUT_BIOS_MIB"; fi
}

# layout_fixed_mib EFI SWAP_MIB: everything except the data partition.
layout_fixed_mib() {
    echo $(($(layout_bios_mib "$1") + LAYOUT_ESP_MIB + LAYOUT_ROOT_MIB + $2 + LAYOUT_GPT_MIB))
}

# layout_min_disk_mib RAM_MIB EFI: smallest disk layout_plan accepts.
layout_min_disk_mib() {
    _lm_swap=$(layout_swap_mib "$1") || return 2
    echo $(($(layout_fixed_mib "$2" "$_lm_swap") + LAYOUT_DATA_MIN_MIB))
}

# layout_sfdisk EFI SWAP_MIB: the sfdisk script. Data has no size: the rest.
layout_sfdisk() {
    echo "label: gpt"
    [ "$1" -eq 1 ] ||
        echo "size=${LAYOUT_BIOS_MIB}MiB, type=$LAYOUT_BIOS_TYPE, name=bios"
    echo "size=${LAYOUT_ESP_MIB}MiB, type=U, name=ESP"
    echo "size=${LAYOUT_ROOT_MIB}MiB, type=L, name=sonora-root"
    echo "size=${2}MiB, type=S, name=sonora-swap"
    echo "type=L, name=sonora-data"
}

# layout_parts EFI: partition numbers, so the caller never counts by hand.
layout_parts() {
    _lp_first=1
    [ "$1" -eq 1 ] || _lp_first=2
    echo "esp_part=$_lp_first"
    echo "root_part=$((_lp_first + 1))"
    echo "swap_part=$((_lp_first + 2))"
    echo "data_part=$((_lp_first + 3))"
}

layout_check_args() {
    layout_is_count "$1" || { layout_err "disk size '$1' is not a MiB count"; return 2; }
    layout_is_count "$2" || { layout_err "RAM size '$2' is not a MiB count"; return 2; }
    case "$3" in 0 | 1) return 0 ;; esac
    layout_err "EFI must be 0 or 1, got '$3'"
    return 2
}

# layout_plan DISK_MIB RAM_MIB EFI: key=value lines, then the sfdisk script
# from its "label: gpt" line to the end. Returns 4 (nothing printed on
# stdout) when the data partition would be under LAYOUT_DATA_MIN_MIB.
layout_plan() {
    layout_check_args "$1" "$2" "$3" || return 2
    _lp_swap=$(layout_swap_mib "$2") || return 2
    _lp_data=$(($1 - $(layout_fixed_mib "$3" "$_lp_swap")))
    if [ "$_lp_data" -lt "$LAYOUT_DATA_MIN_MIB" ]; then
        layout_err "disk too small: $1 MiB leaves ${_lp_data} MiB for sonora-data," \
            "needs $LAYOUT_DATA_MIN_MIB (smallest disk: $(layout_min_disk_mib "$2" "$3") MiB)"
        return 4
    fi
    echo "bios_mib=$(layout_bios_mib "$3")"
    echo "esp_mib=$LAYOUT_ESP_MIB"
    echo "root_mib=$LAYOUT_ROOT_MIB"
    echo "swap_mib=$_lp_swap"
    echo "data_mib=$_lp_data"
    layout_parts "$3"
    layout_sfdisk "$3" "$_lp_swap"
}

# layout_fits_root SQUASH_MIB ROOT_MIB: the squashfs's uncompressed size
# times 1.5 must fit the root partition (headroom for logs and updates).
layout_fits_root() {
    layout_is_count "$1" || { layout_err "image size '$1' is not a MiB count"; return 2; }
    layout_is_count "$2" || { layout_err "root size '$2' is not a MiB count"; return 2; }
    [ $(($1 * 3)) -le $(($2 * 2)) ] && return 0
    layout_err "image needs $((($1 * 3 + 1) / 2)) MiB (1.5 x $1), root has $2 MiB"
    return 4
}
