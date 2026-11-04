#!/bin/sh
# shellcheck disable=SC2015,SC2016,SC2018,SC2019,SC2094
# A/B slot logic (ADR-0044). Sourced by sonora-update and, later, by
# sonora-install. Pure functions over a grubenv file and plain paths: nothing
# here opens a device unless the caller passes one, so tools/test-slots.sh
# drives all of it against files.
#
# State lives in the grubenv on the ESP, the one file GRUB can read and write
# before it has any driver for the slots:
#   sonora_slot    slot that boots by default ("committed"): a or b
#   sonora_trial   slot on probation after an update, empty otherwise
#   sonora_tries   boots left for the trial slot before GRUB gives up on it
#   sonora_valid_a / sonora_valid_b   1 once a slot holds a verified image

SLOTS_ESP_MIB=${SLOTS_ESP_MIB:-512}
SLOTS_SLOT_MIB=${SLOTS_SLOT_MIB:-6144}
# grub.cfg counts down 3 -> 2 -> 1 -> 0 by hand (GRUB has no arithmetic), so
# this must stay 3 unless grub-ab.cfg changes with it.
SLOTS_TRIES=${SLOTS_TRIES:-3}

slots_valid_name() { case "$1" in a | b) return 0 ;; *) return 1 ;; esac; }
slots_other() { case "$1" in a) echo b ;; b) echo a ;; *) return 1 ;; esac; }
slots_label() { slots_valid_name "$1" && echo "sonora-$1"; }

SLOTS_DATA_MIN_MIB=${SLOTS_DATA_MIN_MIB:-4096}

# slots_min_disk_mib: smallest disk the layout fits: ESP, two slots, the
# minimum data partition and 2 MiB for the GPT copies and alignment.
slots_min_disk_mib() { echo $((SLOTS_ESP_MIB + 2 * SLOTS_SLOT_MIB + SLOTS_DATA_MIN_MIB + 2)); }

# slots_layout_script: sfdisk input. The data partition takes the rest of the
# disk, so a bigger disk grows sessions, never the slots. With DISK_MIB it
# refuses a disk that is too small, before sfdisk fails with a raw error.
slots_layout_script() {
    if [ -n "${1:-}" ] && [ "$1" -lt "$(slots_min_disk_mib)" ]; then
        echo "disk of $1 MiB is smaller than the $(slots_min_disk_mib) MiB the A/B layout needs" >&2
        return 1
    fi
    cat <<EOT
label: gpt
size=${SLOTS_ESP_MIB}MiB, type=U, name="sonora-esp"
size=${SLOTS_SLOT_MIB}MiB, type=L, name="sonora-a"
size=${SLOTS_SLOT_MIB}MiB, type=L, name="sonora-b"
type=L, name="sonora-data"
EOT
}

# --- grubenv: the 1024-byte "# GRUB Environment Block" file ----------------

# slots_env_write FILE "k=v<newline>k=v": whole-file rewrite through a
# temporary, so a power cut leaves the old block or the new one, never half.
slots_env_write() {
    _tmp="$1.tmp.$$"
    { printf '# GRUB Environment Block\n'; printf '%s\n' "$2"; } >"$_tmp" || return 1
    _size=$(wc -c <"$_tmp")
    if [ "$_size" -gt 1024 ]; then
        rm -f "$_tmp"
        return 1
    fi
    # '#' padding is what grub-editenv writes; GRUB ignores it.
    _pad=$((1024 - _size))
    if [ "$_pad" -gt 0 ]; then
        head -c "$_pad" /dev/zero | tr '\0' '#' >>"$_tmp"
    fi
    mv "$_tmp" "$1"
}

# slots_env_init FILE: a new block with slot a committed.
slots_env_init() {
    slots_env_write "$1" "sonora_slot=a
sonora_trial=
sonora_tries=0
sonora_valid_a=1
sonora_valid_b=0"
}

# slots_env_get FILE KEY: value, empty when unset.
slots_env_get() {
    sed -n "s/^$2=//p" "$1" | head -n 1
}

# slots_env_set FILE KEY VALUE
slots_env_set() {
    _body=$(grep -E '^[a-z_]+=' "$1" | grep -v "^$2=" || true)
    slots_env_write "$1" "$(printf '%s\n%s=%s' "$_body" "$2" "$3" | sed '/^$/d')"
}

# --- state transitions -------------------------------------------------------

# slots_current CMDLINE: the slot the running system booted from.
slots_current() {
    for _w in $1; do
        case "$_w" in sonora.slot=a | sonora.slot=b) echo "${_w#sonora.slot=}"; return 0 ;; esac
    done
    return 1
}

# slots_inactive ENV CURRENT: the slot an update may write: the one that is not
# committed, and only while the running slot is the committed one. During a
# trial the previous image is the only known-good copy; refuse (exit 2).
slots_inactive() {
    _committed=$(slots_env_get "$1" sonora_slot)
    slots_valid_name "$_committed" || { echo "bad sonora_slot in $1" >&2; return 1; }
    if [ -n "$(slots_env_get "$1" sonora_trial)" ]; then
        echo "an earlier update is still on trial" >&2
        return 2
    fi
    if [ "$2" != "$_committed" ]; then
        echo "running slot $2 is not the committed slot $_committed" >&2
        return 2
    fi
    slots_other "$_committed"
}

# slots_mark_written ENV SLOT: the slot holds a verified image; boot it next,
# on probation.
slots_mark_written() {
    slots_env_set "$1" "sonora_valid_$2" 1 &&
        slots_env_set "$1" sonora_tries "$SLOTS_TRIES" &&
        slots_env_set "$1" sonora_trial "$2"
}

# slots_boot_choice ENV: what grub-ab.cfg does at every boot, in shell, so the
# test exercises the same decision. Prints the slot to boot. The counter is
# spent before the boot, not after, so a hang counts as a failed try.
slots_boot_choice() {
    _trial=$(slots_env_get "$1" sonora_trial)
    _slot=$(slots_env_get "$1" sonora_slot)
    if [ -n "$_trial" ]; then
        _n=$(slots_env_get "$1" sonora_tries)
        if [ "${_n:-0}" -gt 0 ]; then
            slots_env_set "$1" sonora_tries $((_n - 1)) || return 1
            echo "$_trial"
            return 0
        fi
        # Out of tries: the trial slot never confirmed. Forget it.
        slots_env_set "$1" "sonora_valid_$_trial" 0 || return 1
        slots_env_set "$1" sonora_trial "" || return 1
    fi
    echo "$_slot"
}

# slots_confirm ENV CURRENT: the running system is healthy. Only the trial slot
# can commit itself.
slots_confirm() {
    _trial=$(slots_env_get "$1" sonora_trial)
    [ -n "$_trial" ] || return 0
    if [ "$2" != "$_trial" ]; then
        echo "slot $2 is not the trial slot $_trial" >&2
        return 2
    fi
    slots_env_set "$1" sonora_slot "$_trial" &&
        slots_env_set "$1" sonora_trial "" &&
        slots_env_set "$1" sonora_tries 0
}

# slots_rollback ENV: cancel a trial, or else boot the other slot from now on
# if it holds a verified image.
slots_rollback() {
    _trial=$(slots_env_get "$1" sonora_trial)
    _slot=$(slots_env_get "$1" sonora_slot)
    if [ -n "$_trial" ]; then
        slots_env_set "$1" sonora_trial "" && slots_env_set "$1" sonora_tries 0
        return $?
    fi
    _other=$(slots_other "$_slot") || return 1
    if [ "$(slots_env_get "$1" "sonora_valid_$_other")" != 1 ]; then
        echo "slot $_other holds no verified image" >&2
        return 2
    fi
    slots_env_set "$1" sonora_slot "$_other"
}

# --- bundles -------------------------------------------------------------------

# slots_bundle_names SUMSFILE: file names of a (signed) manifest. A name with a
# slash or a leading dot is rejected, so nothing escapes the scratch dir.
slots_bundle_names() {
    awk '{ print $2 }' "$1" | sed 's/^\*//' >"$1.names" || return 1
    while read -r _n; do
        case "$_n" in
            '' | */* | .*) echo "bad name in manifest: $_n" >&2; rm -f "$1.names"; return 1 ;;
        esac
    done <"$1.names"
    cat "$1.names"
    rm -f "$1.names"
}

# slots_newer NEW OLD: true when NEW sorts strictly after OLD.
slots_newer() {
    [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -n 1)" = "$1" ]
}

# slots_verify_signature DIR: the signature over SHA256SUMS. SONORA_VERIFY is
# "cmd SIG DATA" with exit 0 = good; the default is gpgv against the Sonora
# release key. A missing key is a refusal, never a skip.
slots_verify_signature() {
    _verify=${SONORA_VERIFY:-"gpgv --keyring ${SONORA_KEYRING:-/usr/share/keyrings/sonora-release.gpg}"}
    # shellcheck disable=SC2086
    $_verify "$1/SHA256SUMS.sig" "$1/SHA256SUMS" >/dev/null 2>&1 ||
        { echo "signature check failed" >&2; return 2; }
}

# slots_verify_hashes DIR [NAME...]: the files against the signed manifest, all
# of it or only the named lines.
slots_verify_hashes() {
    _dir=$1
    shift
    if [ $# -eq 0 ]; then
        (cd "$_dir" && sha256sum -c SHA256SUMS >/dev/null 2>&1) ||
            { echo "checksum mismatch" >&2; return 2; }
        return 0
    fi
    for _n in "$@"; do
        (cd "$_dir" && grep -E "^[0-9a-f]{64} [ *]$_n\$" SHA256SUMS | sha256sum -c - >/dev/null 2>&1) ||
            { echo "checksum mismatch or $_n not in manifest" >&2; return 2; }
    done
}

# slots_write_image DIR TARGET LABEL: an ext4 filesystem holding the bundle's
# boot files. mke2fs -d fills it without mounting, so the same call works on a
# partition and on a file image (SLOTS_FS_KIB sizes a file image).
slots_write_image() {
    _stage="$1/.stage.$$"
    rm -rf "$_stage"
    mkdir -p "$_stage/live" || return 1
    for _f in vmlinuz initrd.img filesystem.squashfs VERSION; do
        if [ ! -f "$1/$_f" ]; then
            echo "bundle lacks $_f" >&2
            rm -rf "$_stage"
            return 2
        fi
        cp "$1/$_f" "$_stage/live/$_f" || return 1
    done
    if [ -n "${SLOTS_FS_KIB:-}" ]; then
        mke2fs -q -F -t ext4 -L "$3" -d "$_stage" "$2" "$SLOTS_FS_KIB"
    else
        mke2fs -q -F -t ext4 -L "$3" -d "$_stage" "$2"
    fi
    _rc=$?
    rm -rf "$_stage"
    return $_rc
}
