#!/usr/bin/env bash
# Static check of the Secure Boot chain required by ADR-0034: the signed
# packages are in the image and the installer's grub-install calls ask for
# Secure Boot. It cannot prove that firmware accepts the chain; that needs a
# machine or OVMF with enrolled keys.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIST="$ROOT/iso/config/package-lists/secureboot.list.chroot"
AUTO="$ROOT/iso/auto/config"
INSTALLER="$ROOT/iso/config/includes.chroot/usr/local/bin/sonora-install"
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

[ -f "$LIST" ] || { echo "FAIL: missing ${LIST#"$ROOT"/}" >&2; exit 1; }
pkgs="$(grep -Ev '^[[:space:]]*(#|$)' "$LIST" | awk '{print $1}')"
for p in shim-signed grub-efi-amd64-signed grub-efi-amd64-bin efibootmgr mokutil; do
  grep -qx "$p" <<<"$pkgs" || bad "secureboot.list.chroot: $p missing"
done
if grep -Eq '=|/' <<<"$pkgs"; then
  bad "secureboot.list.chroot: version or suite syntax; the suite is fixed by the build"
fi

# The RT kernel must stay Debian's: a flavour from the archive, no local kernel.
grep -Eq -- '--linux-flavours rt-amd64' "$AUTO" || bad "auto/config: --linux-flavours rt-amd64 missing; the signed Debian rt kernel would not be installed"

installer_state="installer absent (checked when it lands)"
if [ -f "$INSTALLER" ]; then
  # Join backslash continuations: a call's flags sit on several lines.
  calls="$(awk '{ if (sub(/\\$/, "")) { printf "%s ", $0 } else print }' "$INSTALLER" | grep -E 'grub-install' | grep -Ev '^[[:space:]]*#' || true)"
  efi="$(grep -E -- 'x86_64-efi|--removable|--efi-directory|--uefi' <<<"$calls" || true)"
  [ -n "$efi" ] || bad "sonora-install: no UEFI grub-install call"
  if grep -Eq -- '--no-uefi-secure-boot' <<<"$calls"; then
    bad "sonora-install: --no-uefi-secure-boot"
  fi
  while IFS= read -r l; do
    [ -z "$l" ] && continue
    grep -Eq -- '--uefi-secure-boot' <<<"$l" || bad "sonora-install: UEFI grub-install without --uefi-secure-boot: $l"
  done <<<"$efi"
  grep -Eq -- '--removable' <<<"$efi" || bad "sonora-install: no --removable fallback call"
  installer_state="installer flags checked"
fi

if [ "$fail" -eq 0 ]; then
  echo "ok: signed shim/GRUB packages listed, rt flavour from the archive, $installer_state; UNVERIFIED: firmware acceptance, rt kernel signature, removable boot, MOK need a Secure Boot machine or OVMF"
fi
exit "$fail"
