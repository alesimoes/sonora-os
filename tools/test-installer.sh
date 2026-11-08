#!/usr/bin/env bash
# sonora-install against a file image, in a privileged Debian container.
#
# Real here: sfdisk, mkfs, loop devices, unsquashfs of a small squashfs, the
# mounts, fstab, the UUID and self-check logic, exit codes. Stubbed: the
# chroot'ed commands (update-initramfs, grub-install, update-grub), which a
# fake rootfs cannot run. Whether GRUB really boots the result is for the QEMU
# check (ADR-0032 "Unverified"). Needs Docker that allows --privileged.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "${1:-}" != "--inside" ]; then
    export MSYS_NO_PATHCONV=1
    src="$ROOT"
    command -v cygpath >/dev/null 2>&1 && src="$(cygpath -w "$ROOT")"
    docker build -q --label org.sonora=1 -t sonora-installer-test:trixie - >/dev/null <<'DOCKER'
FROM debian:trixie
RUN ok=; A="-o Acquire::Retries=5 -o Acquire::http::Timeout=30"; \
 for d in 0 15 45; do [ "$d" = 0 ] || sleep "$d"; \
 if apt-get $A update && apt-get $A install -y --no-install-recommends fdisk parted dosfstools e2fsprogs \
      squashfs-tools util-linux mount \
 ; then ok=1; break; fi; echo "apt attempt $d failed" >&2; done; \
 [ -n "$ok" ] || { echo "apt-get update/install failed after 3 attempts (mirror unreachable?)" >&2; exit 1; }; \
 rm -rf /var/lib/apt/lists/*
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
            # grub-install --uefi-secure-boot also copies the signed shim
            echo x >"$r/boot/efi/EFI/sonora/shimx64.efi"
        fi ;;
    update-grub)
        # Stands in for a copy that went wrong after the files were checked.
        [ -z "${FAKE_CORRUPT:-}" ] || echo junk >>"$r/home/sonora/Sessions/Take 1/take one.wav"
        mkdir -p "$r/boot/grub"
        echo "  linux /boot/vmlinuz-${FAKE_KERNEL:-6.12.0-rt-amd64} root=UUID=x" >"$r/boot/grub/grub.cfg" ;;
esac
exit 0
STUB
chmod +x $T/fakechroot

export SONORA_SQUASHFS=$T/fs.squashfs SONORA_ALLOW_FILE=1 SONORA_EFI=1
export SONORA_IN_TARGET=$T/fakechroot SONORA_SYSBLOCK=$T/sys
# The layout library from the tree, and a fixed RAM so swap is 2 GiB and the
# smallest accepted disk is known (ADR-0090: about 26.5 GiB with EFI).
export SONORA_LIB=/src/iso/config/includes.chroot/usr/local/lib/sonora SONORA_RAM_MIB=4096
mkdir -p $T/sys

# The stick's sessions: a name with a space, an empty directory, a symlink.
export SONORA_SESSIONS=$T/sessions
mkdir -p "$T/sessions/Take 1/Audio" "$T/sessions/Empty"
head -c 300000 /dev/urandom >"$T/sessions/Take 1/take one.wav"
echo '{"name":"Take 1"}' >"$T/sessions/Take 1/session.json"
ln -s "take one.wav" "$T/sessions/Take 1/latest.wav"
SRC_SUMS=$(cd "$T/sessions" && find . -type f -exec sha256sum {} + | sort -k2)

fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
expect_code() { # name want got
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (exit $3, want $2)"; fi
}
# Detaches every loop device backed by the test image, deleted file included
# (losetup -j misses a backing file that was already removed).
detach_all() {
    local l
    mountpoint -q $T/m/home/sonora/Sessions 2>/dev/null && umount $T/m/home/sonora/Sessions 2>/dev/null
    mountpoint -q $T/m 2>/dev/null && umount $T/m 2>/dev/null
    for l in $(losetup -nO NAME,BACK-FILE 2>/dev/null | awk -v f="$T/disk.img" '$2 == f || $2 == f "(deleted)" {print $1}'); do
        losetup -d "$l" 2>/dev/null || true
    done
}
trap detach_all EXIT
detach_all
# Sparse: only what mkfs and the copy write takes space on the host.
newimg() { detach_all; rm -f $T/disk.img; truncate -s "${1:-28G}" $T/disk.img; : >$T/calls; }
# Mounts the root partition of the image read-only at $T/m. Sets INSPECT_OK;
# a failure is reported and the run goes on, so one flaky mount cannot hide
# the checks after it.
inspect() {
    INSPECT_OK=0
    L=$(losetup -f --show -P $T/disk.img) || { bad "inspect: losetup failed"; return 0; }
    local i p n m ROOTDEV='' DATADEV='' SWAPDEV=''
    # The kernel scans partitions after the attach returns: wait for them.
    for i in $(seq 50); do
        [ -e "/sys/class/block/${L##*/}/${L##*/}p2" ] && break
        [ "$i" = 10 ] && { partx -u "$L" 2>/dev/null || blockdev --rereadpt "$L" 2>/dev/null || true; }
        sleep 0.1
    done
    for n in 1 2 3 4 5; do
        p="${L}p$n"
        m=$(cat "/sys/class/block/${p##*/}/dev" 2>/dev/null) || continue
        rm -f "$p"
        mknod "$p" b "${m%:*}" "${m#*:}"
    done
    # This loop's partitions only: a leaked loop may carry the same label.
    for p in "${L}"p*; do
        [ -b "$p" ] || continue
        case $(blkid -s LABEL -o value "$p" 2>/dev/null) in
            sonora-root) ROOTDEV=$p ;;
            sonora-data) DATADEV=$p ;;
            sonora-swap) SWAPDEV=$p ;;
        esac
    done
    if [ -z "$ROOTDEV" ]; then bad "inspect: no sonora-root partition on $L ($(echo "${L}"p*))"; return 0; fi
    mkdir -p $T/m
    if mount -o ro "$ROOTDEV" $T/m; then INSPECT_OK=1; else bad "inspect: mount -o ro $ROOTDEV $T/m failed"; fi
    ROOTDEV_LAST=$ROOTDEV
    DATADEV_LAST=$DATADEV
    SWAPDEV_LAST=$SWAPDEV
    if [ "$INSPECT_OK" = 1 ] && [ -n "$DATADEV" ] && ! mount -o ro "$DATADEV" $T/m/home/sonora/Sessions; then
        bad "inspect: mount -o ro $DATADEV failed"
    fi
}
release() { detach_all; }

# 1. UEFI success
newimg
out=$("$INST" --yes --target $T/disk.img 2>&1) && rc=0 || rc=$?
expect_code "uefi install exits 0" 0 "$rc"
[ "$rc" -eq 0 ] || echo "$out"
for n in 1 2 3 4 5 6 7; do
    if grep -q "^STEP $n/7 " <<<"$out"; then ok "STEP $n/7 announced"; else bad "STEP $n/7 missing"; fi
done
if [ "$(sfdisk -d $T/disk.img | grep -c 'start=')" = 4 ]; then ok "four partitions (ESP, root, swap, data)"; else bad "partition count"; fi
if sfdisk -d $T/disk.img | grep -q 'size= *4194304,.*name="sonora-swap"'; then ok "swap is RAM/2 (2 GiB)"; else bad "swap size"; fi
if swapon --show=NAME --noheadings | grep -q loop; then bad "swap enabled on the installing host"; else ok "swap not enabled on the host"; fi
inspect
if [ "$(grep -c '^UUID=' $T/m/etc/fstab)" = 4 ]; then ok "fstab has four lines"; else bad "fstab lines: $(cat $T/m/etc/fstab)"; fi
du=$(sed -n 's/^UUID=\([^ ]*\) \/home\/sonora\/Sessions ext4 noatime,errors=remount-ro 0 2$/\1/p' $T/m/etc/fstab)
if [ -n "$du" ] && [ "$(blkid -s UUID -o value "$DATADEV_LAST")" = "$du" ]; then ok "fstab data line is sonora-data's"; else bad "fstab data line"; fi
if grep -q nofail $T/m/etc/fstab; then bad "nofail in fstab"; else ok "no nofail in fstab"; fi
if grep -q '^UUID=[^ ]* none swap sw 0 0$' $T/m/etc/fstab; then ok "fstab swap line"; else bad "fstab swap line"; fi
# the four fstab UUIDs are four different partitions that exist
fu=$(sed -n 's/^UUID=\([^ ]*\) .*/\1/p' $T/m/etc/fstab | sort -u)
fu_ok=1
for u in $fu; do blkid -U "$u" >/dev/null || fu_ok=0; done
if [ "$(wc -l <<<"$fu")" = 4 ] && [ "$fu_ok" = 1 ]; then ok "fstab names four distinct UUIDs that exist"; else bad "fstab UUIDs: $fu"; fi
if [ -n "${SWAPDEV_LAST:-}" ] && [ "$(blkid -s TYPE -o value "$SWAPDEV_LAST")" = swap ]; then ok "blkid TYPE=swap on the swap partition"; else bad "swap partition type"; fi
su=$(sed -n 's/^UUID=\([^ ]*\) none swap.*/\1/p' $T/m/etc/fstab)
if [ -n "$su" ] && [ "$(blkid -s UUID -o value "${SWAPDEV_LAST:-/dev/null}")" = "$su" ]; then ok "fstab swap UUID is the swap partition's"; else bad "fstab swap UUID"; fi
if [ "$(findmnt -n -o SOURCE "$T/m/home/sonora/Sessions" 2>/dev/null)" = "$DATADEV_LAST" ]; then ok "data partition mounted at Sessions"; else bad "Sessions not on the data partition"; fi
if grep -qx 'RequiresMountsFor=/home/sonora/Sessions' $T/m/etc/systemd/system/sonora-engine.service.d/sessions-mount.conf 2>/dev/null; then ok "engine requires the Sessions mount"; else bad "engine drop-in"; fi
if [ -s $T/m/var/lib/sonora/installed ]; then ok "marker written"; else bad "marker missing"; fi
if [ -f $T/m/etc/machine-id ] && [ ! -s $T/m/etc/machine-id ]; then ok "machine-id emptied"; else bad "machine-id"; fi
if [ "$INSPECT_OK" = 1 ] && [ ! -e $T/m/var/lib/dbus/machine-id ]; then ok "dbus machine-id removed"; else bad "dbus machine-id"; fi
ru=$(sed -n 's/^UUID=\([^ ]*\) \/ .*/\1/p' $T/m/etc/fstab)
if [ "$(blkid -s UUID -o value "$ROOTDEV_LAST")" = "$ru" ]; then ok "fstab root UUID is the partition's"; else bad "fstab root UUID"; fi
if grep -q 'ext4 noatime' $T/m/etc/fstab; then ok "root mounted noatime"; else bad "noatime"; fi
if grep -q 'GRUB_CMDLINE_LINUX_DEFAULT=.*threadirqs' $T/m/etc/default/grub.d/90-sonora.cfg; then ok "cmdline keeps threadirqs"; else bad "threadirqs"; fi
# session carry-over: identical on the target, and the stick's copy untouched
if diff -r -x lost+found "$T/sessions" "$T/m/home/sonora/Sessions" >/dev/null 2>&1; then ok "sessions carried over byte for byte"; else bad "sessions differ on the target"; fi
if [ -d "$T/m/home/sonora/Sessions/Empty" ] && [ -L "$T/m/home/sonora/Sessions/Take 1/latest.wav" ]; then ok "empty directory and symlink carried"; else bad "directory or symlink lost"; fi
if [ "$(cd "$T/sessions" && find . -type f -exec sha256sum {} + | sort -k2)" = "$SRC_SUMS" ]; then ok "stick's sessions untouched"; else bad "stick's sessions changed"; fi
if grep -q '^INFO carried over 2 session files' <<<"$out"; then ok "carry-over reported"; else bad "carry-over not reported"; fi
release
if grep -q 'chown -R sonora:sonora /home/sonora/Sessions' $T/calls; then ok "sessions given to sonora"; else bad "chown call"; fi
if grep -q 'systemctl enable fstrim.timer' $T/calls; then ok "fstrim.timer enabled"; else bad "fstrim call"; fi
if grep -q 'update-initramfs -u -k all' $T/calls; then ok "initramfs rebuilt for all kernels"; else bad "initramfs call"; fi
if grep -q -- '--bootloader-id=sonora' $T/calls && grep -q -- '--removable' $T/calls; then ok "NVRAM entry and removable path"; else bad "grub-install calls"; fi

# 2. default entry not rt: must fail with 9 and leave no marker
newimg
FAKE_KERNEL=6.12.0-amd64 "$INST" --yes --target $T/disk.img >/dev/null 2>$T/err && rc=0 || rc=$?
expect_code "non-rt default entry exits 9" 9 "$rc"
if grep -q 'not the rt kernel' $T/err; then ok "names the failed check"; else bad "failed check not named"; fi
inspect
if [ "$INSPECT_OK" = 1 ] && [ ! -e $T/m/var/lib/sonora/installed ]; then ok "no marker after failed check"; else bad "marker after failure"; fi
release

# 2b. a carried file that does not verify: exit 11, no marker, source intact
newimg
FAKE_CORRUPT=1 "$INST" --yes --target $T/disk.img >/dev/null 2>$T/err && rc=0 || rc=$?
expect_code "corrupt carry-over exits 11" 11 "$rc"
if grep -q 'do not match the stick' $T/err; then ok "mismatch is named"; else bad "mismatch not named"; fi
inspect
if [ "$INSPECT_OK" = 1 ] && [ ! -e $T/m/var/lib/sonora/installed ]; then ok "no marker after a failed carry-over"; else bad "marker after failed carry-over"; fi
release
if [ "$(cd "$T/sessions" && find . -type f -exec sha256sum {} + | sort -k2)" = "$SRC_SUMS" ]; then ok "stick's sessions untouched after a failure"; else bad "stick's sessions changed by a failure"; fi

# 2c. a stick with no sessions at all installs fine
newimg
SONORA_SESSIONS=$T/nonexistent "$INST" --yes --target $T/disk.img >$T/out 2>&1 && rc=0 || rc=$?
expect_code "no sessions directory exits 0" 0 "$rc"
if grep -q 'no sessions on this system' $T/out; then ok "says there is nothing to carry"; else bad "silent about missing sessions"; fi

# 3. BIOS layout
newimg
SONORA_EFI=0 "$INST" --yes --target $T/disk.img >/dev/null 2>&1 && rc=0 || rc=$?
expect_code "bios install exits 0" 0 "$rc"
if [ "$(sfdisk -d $T/disk.img | grep -c 'start=')" = 5 ]; then ok "bios: boot partition + ESP + root + swap + data"; else bad "bios partition count"; fi

# 3b. a disk under the smallest layout is refused before anything is written
newimg 20G
"$INST" --yes --target $T/disk.img >/dev/null 2>$T/err && rc=0 || rc=$?
expect_code "too small a disk exits 4" 4 "$rc"
if grep -q 'too small' $T/err; then ok "names the size refusal"; else bad "size refusal not named"; fi
if [ "$(sfdisk -d $T/disk.img 2>/dev/null | grep -c 'start=')" = 0 ]; then ok "small disk left unpartitioned"; else bad "small disk was partitioned"; fi

# 3c. an image too big for the root partition is refused before any write
newimg
mkdir -p $T/fakebin
cat >$T/fakebin/unsquashfs <<'STUB'
#!/bin/sh
# 60 GiB of regular files in the listing; anything else is the real tool.
if [ "$1" = -lls ]; then echo "-rw-r--r-- root/root 64424509440 2024-01-01 00:00 squashfs-root/big"; exit 0; fi
exec /usr/bin/unsquashfs "$@"
STUB
chmod +x $T/fakebin/unsquashfs
PATH=$T/fakebin:$PATH "$INST" --yes --target $T/disk.img >/dev/null 2>$T/err && rc=0 || rc=$?
expect_code "oversize image exits 4" 4 "$rc"
if grep -q 'does not fit the root partition' $T/err; then ok "names the fit refusal"; else bad "fit refusal not named: $(cat $T/err)"; fi
if [ "$(sfdisk -d $T/disk.img 2>/dev/null | grep -c 'start=')" = 0 ]; then ok "oversize image left the disk unpartitioned"; else bad "oversize image partitioned the disk"; fi

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
# --list: DEVICE, BYTES, MODEL, tab separated; internal disks only
mkdir -p $T/sys/sda/device
echo 2048 >$T/sys/sda/size
echo 'QEMU HARDDISK   ' >$T/sys/sda/device/model
"$INST" --list >$T/out 2>&1 && rc=0 || rc=$?
expect_code "--list exits 0" 0 "$rc"
if [ "$(cat $T/out)" = "$(printf '/dev/sda	1048576	QEMU HARDDISK')" ]; then ok "--list line is device, bytes, model"; else bad "--list output: $(cat $T/out)"; fi
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
