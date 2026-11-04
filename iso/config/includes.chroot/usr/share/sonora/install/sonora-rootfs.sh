#!/bin/sh
# Pure helpers for the installed root filesystem; sourced by sonora-install,
# no side effects on load. Swappiness and dirty ratios are NOT set here: they
# live once, in audio-stack/sysctl-sonora.conf (installed as 99-sonora.conf).

SWAPFILE=/swapfile

# swap_mib RAM_MIB ROOT_MIB -> swap size in MiB, 0 for none.
# Swap here is a safety net against the OOM killer taking the engine, not
# working memory: 2 GiB up to 4 GiB RAM, then 4 GiB, never more than a tenth of
# the root so a small disk keeps its room for takes. Hibernation is not
# supported, so RAM size does not need to fit in swap.
swap_mib() {
    ram=$1
    root=$2
    case "$ram:$root" in
        *[!0-9:]* | : | :* | *:) echo 0; return 1 ;;
    esac
    if [ "$ram" -le 4096 ]; then want=2048; else want=4096; fi
    cap=$((root / 10))
    if [ "$want" -gt "$cap" ]; then want=$cap; fi
    # Below 256 MiB a swapfile is noise.
    if [ "$want" -lt 256 ]; then want=0; fi
    echo "$want"
}

# fstab_swap_line MIB -> the fstab line for the swapfile, or a comment.
fstab_swap_line() {
    if [ "$1" -gt 0 ]; then
        echo "$SWAPFILE  none  swap  sw,pri=1  0 0"
    else
        echo "# no swap: the disk is too small to spare it"
    fi
}

# render_fstab TEMPLATE ROOT_UUID ESP_UUID SWAP_MIB -> fstab on stdout.
render_fstab() {
    line=$(fstab_swap_line "$4")
    sed -e "s|@ROOT_UUID@|$2|" -e "s|@ESP_UUID@|$3|" -e "s|@SWAP_LINE@|$line|" "$1"
}

# mkfs_root_opts -> options for mkfs.ext4. The label is what the boot entry and
# the self-check look for; lazy init is off so the first recording does not
# compete with the background inode-table writer.
mkfs_root_opts() {
    echo "-L sonora-root -E lazy_itable_init=0,lazy_journal_init=0"
}
