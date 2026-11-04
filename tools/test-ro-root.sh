#!/usr/bin/env bash
# Static check of the read-only root: the system lives on the ISO's squashfs,
# only /home is persisted, and the RAM-backed writable paths are capped. It
# cannot prove the running mounts; see "UNVERIFIED" in the ok line.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AUTO="$ROOT/iso/auto/config"
INC="$ROOT/iso/config/includes.chroot"
JOURNALD="$INC/etc/systemd/journald.conf.d/sonora.conf"
TMPMOUNT="$INC/etc/systemd/system/tmp.mount.d/sonora.conf"
WRITEUSB="$ROOT/tools/write-usb.sh"
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
for f in "$AUTO" "$JOURNALD" "$TMPMOUNT" "$WRITEUSB"; do
  [ -f "$f" ] || bad "missing ${f#"$ROOT"/}"
done
[ "$fail" -eq 0 ] || exit 1

# key FILE KEY: the last value of KEY= in FILE, comments ignored.
key() { grep -Ev '^[[:space:]]*#' "$1" | sed -nE "s/^[[:space:]]*$2[[:space:]]*=[[:space:]]*(.*)$/\1/p" | tail -n 1; }
# mib VALUE: a size such as 64M or 1G in MiB, empty if unparsable.
mib() {
  case "$1" in
    *[0-9]M) echo "${1%M}" ;;
    *[0-9]G) echo $((${1%G} * 1024)) ;;
    *) echo "" ;;
  esac
}

# --- boot: squashfs root, persistence only on the stick's partition ---
grep -Eq -- '--bootappend-live "[^"]*\bboot=live\b' "$AUTO" || bad "auto/config: boot=live missing; the root would not be the read-only squashfs"
grep -Eq -- '--bootappend-live "[^"]*\b(toram|overlay-?root)' "$AUTO" &&
  bad "auto/config: toram or a second overlay option changes where the root is written"
grep -Eq -- '--bootappend-live "[^"]*\bpersistence\b' "$AUTO" || bad "auto/config: persistence missing from the boot line"

# --- persistence.conf unions /home and nothing else ---
conf_lines="$(grep -E 'persistence\.conf' "$WRITEUSB" | grep -E 'echo|printf' || true)"
[ "$(printf '%s\n' "$conf_lines" | grep -c .)" = 1 ] || bad "write-usb.sh: expected exactly one line writing persistence.conf"
grep -Eq 'echo "/home union" > ' <<<"$conf_lines" || bad "write-usb.sh: persistence.conf is not exactly '/home union'"

# --- the image ships no fstab line naming the root; live-boot owns it ---
if [ -f "$INC/etc/fstab" ]; then
  fstab="$(grep -Ev '^[[:space:]]*#' "$INC/etc/fstab" || true)"
  grep -Eq '[[:space:]]/[[:space:]]' <<<"$fstab" &&
    bad "includes.chroot/etc/fstab: names the root"
fi

# --- RAM-backed writes are capped ---
[ "$(key "$JOURNALD" Storage)" = volatile ] || bad "journald: Storage= is not volatile; the journal would fill the overlay"
m="$(mib "$(key "$JOURNALD" RuntimeMaxUse)")"
{ [ -n "$m" ] && [ "$m" -ge 8 ] && [ "$m" -le 256 ]; } || bad "journald: RuntimeMaxUse= missing or outside 8M..256M"
k="$(mib "$(key "$JOURNALD" RuntimeKeepFree)")"
[ -n "$k" ] || bad "journald: RuntimeKeepFree= missing"
opts="$(key "$TMPMOUNT" Options)"
case ",$opts," in *,nosuid,*) ;; *) bad "tmp.mount: Options= lacks nosuid" ;; esac
case ",$opts," in *,nodev,*) ;; *) bad "tmp.mount: Options= lacks nodev" ;; esac
t="$(mib "$(printf '%s\n' "$opts" | tr ',' '\n' | sed -nE 's/^size=(.*)$/\1/p')")"
{ [ -n "$t" ] && [ "$t" -ge 64 ] && [ "$t" -le 2048 ]; } || bad "tmp.mount: size= missing or outside 64M..2G"

if [ "$fail" -eq 0 ]; then
  echo "ok: squashfs root (boot=live), persistence.conf is '/home union' only, journald volatile ${m}M, /tmp ${t}M; UNVERIFIED: running mounts need a boot"
fi
exit "$fail"
