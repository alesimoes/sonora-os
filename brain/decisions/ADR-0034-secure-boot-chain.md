# ADR-0034 — Secure Boot: Debian's signed chain, no keys of our own

Status: accepted (2026-11-04). Settles `system.install.secure_boot`; completes
ADR-0032 section 3.

## Summary

The installed system boots under Secure Boot with Debian's own chain: Microsoft
signs `shim-signed`, shim trusts Debian's CA, which signed
`grub-efi-amd64-signed` and Debian's kernel packages, RT flavour included. We
sign nothing and ship no key. The installer calls `grub-install
--uefi-secure-boot` for both the NVRAM entry and the `--removable` fallback.
A custom or DKMS module is not shipped; if one ever is, it goes through MOK
enrolment, which needs the operator at the console once. Secure Boot off is a
supported fallback and boots the same files.

## Context

ADR-0002 chose Debian partly for this chain. ADR-0032 left Secure Boot open.
The target is a desktop or laptop with a UMC1820 on USB; many such machines
ship with Secure Boot on and the owner should not have to enter firmware setup
to install.

## Decision

1. **Chain.** firmware (db: Microsoft UEFI CA) -> `shimx64.efi` (Debian's
   `shim-signed`) -> `grubx64.efi` (`grub-efi-amd64-signed`, checked against
   Debian's CA embedded in shim) -> kernel (checked by GRUB through shim's
   `shim_lock` protocol). Sources: Debian wiki SecureBoot, `shim-signed` and
   `grub2` package documentation, `grub-install(8)`.
2. **Packages** in `iso/config/package-lists/secureboot.list.chroot`:
   `shim-signed`, `grub-efi-amd64-signed`, `grub-efi-amd64-bin`, `efibootmgr`,
   `mokutil`. Same suite as the rest of the image (pinned by the build, never
   unstable).
3. **Installer flags.** Every UEFI `grub-install` in `sonora-install` passes
   `--uefi-secure-boot` explicitly, never `--no-uefi-secure-boot`. Without
   `--removable` it writes `\EFI\debian\shimx64.efi` plus the NVRAM entry; with
   `--removable` it writes shim as `\EFI\BOOT\BOOTX64.EFI` with the signed GRUB
   beside it, so a firmware that forgets its NVRAM still reaches shim, not an
   unsigned GRUB.
4. **The RT kernel.** The image builds with `--linux-flavours rt-amd64`
   (`iso/auto/config`), which installs Debian's `linux-image-*-rt-amd64`. Debian
   builds that flavour from the same source as the generic kernel and, as we
   understand the `linux-signed-amd64` source package, signs it with the same
   archive key, so shim accepts it with no enrolment. The build and the
   installer must not replace it with a locally built kernel. That the rt
   flavour is in the signed set for the pinned suite is the one claim here we
   have not seen; it is checked on hardware (below).
5. **Lockdown.** A Secure Boot boot turns on kernel lockdown (integrity): no
   unsigned modules, no `/dev/mem` writes, no kexec of unsigned images. The
   engine, PipeWire and the USB audio class driver need none of these. A
   real-time tuning that does (a custom module, `/dev/mem`) is rejected in
   review.
6. **Custom modules.** None shipped. An out-of-tree driver would need DKMS plus
   a Machine Owner Key: `mokutil --import`, reboot, the MokManager screen,
   password typed once at the console. That is a one-time manual step the
   installer does not automate; the item that introduces such a module owns the
   instructions.
7. **Fallback.** With Secure Boot off, firmware runs the same shim, which
   passes straight to GRUB. No second image or flag exists. The installer's
   self-check (ADR-0032 1.6) should also assert the ESP holds shim and signed
   GRUB, not only "a loader" (follow-up for the installer owner).
8. **Not decided here.** Signing our own kernel or GRUB, a custom shim, TPM
   measured boot, LUKS. Each reopens this ADR.

## Consequences

- A few MiB more in the image. No key management.
- The stick (live-build ISO) keeps its own bootloader and is unaffected; this
  covers the installed disk.
- A machine whose firmware db lacks Microsoft's CA (rare) needs Secure Boot
  off.

## Checked offline and not

`tools/test-secure-boot.sh` checks the package list, the rt flavour in
`auto/config` and, when `sonora-install` exists, its `grub-install` flags.
UNVERIFIED, needs a real or OVMF machine with Secure Boot keys enrolled: that
shim accepts the rt kernel, that the removable path boots with NVRAM cleared,
lockdown behaviour, MOK enrolment, and that `linux-image-*-rt-amd64` in the
pinned suite is the signed package.
