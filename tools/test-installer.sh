#!/usr/bin/env bash
# sonora-install against a file image, in a privileged Debian container.
#
# Real here: sfdisk, mkfs, loop devices, unsquashfs of a small squashfs, the
# mounts, fstab, the UUID and self-check logic, exit codes. Stubbed: the
# chroot'ed commands (update-initramfs, grub-install, update-grub), which a
# fake rootfs cannot run. Whether GRUB really boots the result is for the QEMU
# check (ADR-0032 "Unverified"). Needs Docker that allows --privileged.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "${1:-}" != "--inside" ]; then
    export MSYS_NO_PATHCONV=1
    src="$ROOT"
    command -v cygpath >/dev/null 2>&1 && src="$(cygpath -w "$ROOT")"
    docker build -q --label org.sonora=1 -t sonora-installer-test:trixie - >/dev/null <<'DOCKER'
FROM debian:trixie
RUN apt-get update \
 && apt-get install -y --no-install-recommends fdisk parted dosfstools e2fsprogs \
      squashfs-tools util-linux mount \
 && rm -rf /var/lib/apt/lists/*
DOCKER
    docker run --rm --privileged --label org.sonora=1 -v "$src:/src:ro" \
        sonora-installer-test:trixie bash /src/tools/test-installer.sh --inside
    exit
fi

INST=/src/iso/config/includes.chroot/usr/local/bin/sonora-install
T=/tmp/t
mkdir -p $T/rootfs/{etc,boot,var/lib/dbus,usr/bin}
echo old >$T/rootfs/var/lib/dbus/machine-id
mksquashfs $T/rootfs $T/fs.squashfs -quiet -noappend >/dev/null

# Stands in for chroot: $1 is the target root, the rest the command.
cat >$T/fakechroot <<'STUB'
#!/bin/bash
r=$1; shift
echo "$*" >>/tmp/t/calls
case $1 in
    dpkg) exit 1 ;;
    update-initramfs) touch "$r/boot/initrd.img-6.12.0-rt-amd64" ;;
    grub-install)
        if [[ " $* " == *" --removable "* ]]; then
            mkdir -p "$r/boot/efi/EFI/BOOT"; echo x >"$r/boot/efi/EFI/BOOT/BOOTX64.EFI"
        else
            mkdir -p "$r/boot/efi/EFI/sonora"; echo x >"$r/boot/efi/EFI/sonora/grubx64.efi"
        fi ;;
    update-grub)
        mkdir -p "$r/boot/grub"
        echo "  linux /boot/vmlinuz-${FAKE_KERNEL:-6.12.0-rt-amd64} root=UUID=x" >"$r/boot/grub/grub.cfg" ;;
esac
exit 0
STUB
chmod +x $T/fakechroot

export SONORA_SQUASHFS=$T/fs.squashfs SONORA_ALLOW_FILE=1 SONORA_EFI=1
export SONORA_IN_TARGET=$T/fakechroot SONORA_SYSBLOCK=$T/sys
mkdir -p $T/sys

fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
expect_code() { # name want got
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (exit $3, want $2)"; fi
}
newimg() { rm -f $T/disk.img; truncate -s 2G $T/disk.img; : >$T/calls; }
# Mounts the root partition of the image read-only at $T/m.
inspect() {
    L=$(losetup -f --show -P $T/disk.img)
    sleep 0.5
    for p in "${L}p1" "${L}p2" "${L}p3"; do
        if [ ! -b "$p" ] && m=$(cat "/sys/class/block/${p##*/}/dev" 2>/dev/null); then
            mknod "$p" b "${m%:*}" "${m#*:}"
        fi
    done
    ROOTDEV=$(blkid -t LABEL=sonora-root -o device | head -n 1)
    mkdir -p $T/m
    mount -o ro "$ROOTDEV" $T/m
}
release() { umount $T/m; losetup -d "$L"; }

# 1. UEFI success
newimg
out=$("$INST" --yes --target $T/disk.img 2>&1) && rc=0 || rc=$?
expect_code "uefi install exits 0" 0 "$rc"
[ "$rc" -eq 0 ] || echo "$out"
for n in 1 2 3 4 5 6 7; do
    if grep -q "^STEP $n/7 " <<<"$out"; then ok "STEP $n/7 announced"; else bad "STEP $n/7 missing"; fi
done
if [ "$(sfdisk -d $T/disk.img | grep -c 'start=')" = 2 ]; then ok "two partitions (ESP, root)"; else bad "partition count"; fi
inspect
if [ -s $T/m/var/lib/sonora/installed ]; then ok "marker written"; else bad "marker missing"; fi
if [ -f $T/m/etc/machine-id ] && [ ! -s $T/m/etc/machine-id ]; then ok "machine-id emptied"; else bad "machine-id"; fi
if [ ! -e $T/m/var/lib/dbus/machine-id ]; then ok "dbus machine-id removed"; else bad "dbus machine-id"; fi
ru=$(sed -n 's/^UUID=\([^ ]*\) \/ .*/\1/p' $T/m/etc/fstab)
if [ "$(blkid -s UUID -o value "$ROOTDEV")" = "$ru" ]; then ok "fstab root UUID is the partition's"; else bad "fstab root UUID"; fi
if grep -q 'ext4 noatime' $T/m/etc/fstab; then ok "root mounted noatime"; else bad "noatime"; fi
if grep -q 'GRUB_CMDLINE_LINUX_DEFAULT=.*threadirqs' $T/m/etc/default/grub.d/90-sonora.cfg; then ok "cmdline keeps threadirqs"; else bad "threadirqs"; fi
release
if grep -q 'update-initramfs -u -k all' $T/calls; then ok "initramfs rebuilt for all kernels"; else bad "initramfs call"; fi
if grep -q -- '--bootloader-id=sonora' $T/calls && grep -q -- '--removable' $T/calls; then ok "NVRAM entry and removable path"; else bad "grub-install calls"; fi

# 2. default entry not rt: must fail with 9 and leave no marker
newimg
FAKE_KERNEL=6.12.0-amd64 "$INST" --yes --target $T/disk.img >/dev/null 2>$T/err && rc=0 || rc=$?
expect_code "non-rt default entry exits 9" 9 "$rc"
if grep -q 'not the rt kernel' $T/err; then ok "names the failed check"; else bad "failed check not named"; fi
inspect
if [ ! -e $T/m/var/lib/sonora/installed ]; then ok "no marker after failed check"; else bad "marker after failure"; fi
release

# 3. BIOS layout
newimg
SONORA_EFI=0 "$INST" --yes --target $T/disk.img >/dev/null 2>&1 && rc=0 || rc=$?
expect_code "bios install exits 0" 0 "$rc"
if [ "$(sfdisk -d $T/disk.img | grep -c 'start=')" = 3 ]; then ok "bios: boot partition + ESP + root"; else bad "bios partition count"; fi

# 4. refusals
"$INST" --bogus >/dev/null 2>&1 && rc=0 || rc=$?
expect_code "bad option exits 2" 2 "$rc"
"$INST" --yes --target /nonexistent >/dev/null 2>&1 && rc=0 || rc=$?
expect_code "missing target exits 3" 3 "$rc"
SONORA_ALLOW_FILE=0 "$INST" --yes --target $T/disk.img >/dev/null 2>&1 && rc=0 || rc=$?
expect_code "regular file refused on the stick" 3 "$rc"
mkdir -p $T/sys/sda $T/sys/sdb $T/sys/loop0
echo 0 >$T/sys/sda/removable
echo 1 >$T/sys/sdb/removable
echo 0 >$T/sys/loop0/removable
# --select-only stops before any write: no path here reaches a real disk.
"$INST" --yes --select-only >$T/out 2>&1 && rc=0 || rc=$?
expect_code "unnamed target picks the one internal disk" 0 "$rc"
if grep -qx '/dev/sda' $T/out; then ok "selected /dev/sda, skipped removable and loop"; else bad "candidate choice: $(cat $T/out)"; fi
"$INST" --yes --select-only --target /dev/sda >/dev/null 2>&1 && rc=0 || rc=$?
expect_code "test hooks refuse a real disk" 3 "$rc"
echo 1 >$T/sys/sda/removable
"$INST" --yes --select-only >$T/out 2>&1 && rc=0 || rc=$?
if grep -q '0 internal candidates' $T/out; then ok "no internal disk is reported"; else bad "zero candidates"; fi
echo n | "$INST" --target $T/disk.img >/dev/null 2>&1 && rc=0 || rc=$?
expect_code "wrong confirmation exits 3" 3 "$rc"

# 5. the cmdline default follows iso/auto/config
for w in quiet splash threadirqs; do
    if grep -q "bootappend-live.*\b$w\b" /src/iso/auto/config; then ok "auto/config has $w"; else bad "auto/config lacks $w"; fi
done
if grep -q 'CMDLINE:-quiet splash threadirqs' $INST; then ok "installer default cmdline"; else bad "installer cmdline"; fi

echo "failures: $fails"
[ $fails -eq 0 ]
