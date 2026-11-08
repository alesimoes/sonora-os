# ADR-0099 — Secure Boot: a runtime check of the installed chain

Status: accepted (2026-11-08). Amends ADR-0034; does not contradict it.

## Summary

The chain stays Debian's shim plus signed GRUB, with no key of ours (ADR-0034).
This adds a verifiable contract: `sonora-secureboot-check`, a read-only POSIX
sh script in the image, reports the state of the installed machine as
`key=value` lines and fails when the ESP lacks shim, signed GRUB or the
removable loader. Secure Boot off is reported, not failed (ADR-0034 section 7).
It closes the follow-up in ADR-0034 section 7 (the self-check asserts shim and
signed GRUB, not just "a loader").

## Contract

`sonora-secureboot-check [--require-on]`, never writes.

| Key | Values | Source |
|---|---|---|
| `secure_boot` | `on` / `off` / `unsupported` | `mokutil --sb-state`; `unsupported` when it fails or says nothing (BIOS boot, no EFI variables) |
| `lockdown` | `none` / `integrity` / `confidential` | the bracketed word in `/sys/kernel/security/lockdown` |
| `kernel_rt` | `yes` / `no` | `uname -r` ends in `rt-amd64` |
| `esp_shim` | `yes` / `no` | `EFI/<id>/shimx64.efi` (any id but `BOOT`) on the ESP |
| `esp_grub` | `yes` / `no` | `EFI/<id>/grubx64.efi` |
| `removable` | `yes` / `no` | `EFI/BOOT/BOOTX64.EFI` |
| `nvram_entry` | `yes` / `no` / `unknown` | `efibootmgr` lists a `sonora` or `debian` entry; `unknown` if it cannot run |

Exit codes: 0 when `esp_shim`, `esp_grub` and `removable` are all yes; 3 when
any is missing, naming the file on stderr; 4 with `--require-on` when
`secure_boot` is not `on`; 2 on bad usage. Exit 3 outranks 4. `kernel_rt`,
`lockdown` and `nvram_entry` are informational: an NVRAM that firmware
forgot is exactly what the removable loader covers.

Test hooks: `SONORA_ROOT` (prefix for `/boot/efi` and `/sys`), `SONORA_ESP`,
`SONORA_MOKUTIL`, `SONORA_EFIBOOTMGR`, `SONORA_LOCKDOWN`, `SONORA_UNAME`.
`tools/test-secureboot-check.sh` runs it against a fake root and a stub
`mokutil`, without Docker: on, off, unsupported, missing shim, missing
removable loader, `--require-on`.

## Stays unverified

The fake-root test proves the script's logic, not the firmware. Not seen:
that firmware accepts the chain, that the `rt-amd64` kernel is in the signed
set for the pinned suite (ADR-0034 section 4), that the removable path boots
with an empty NVRAM, and the MOK flow. These need a Secure Boot machine or OVMF
with enrolled keys.

## Handed to the researcher

Boot the built image in OVMF with Secure Boot enabled (Microsoft keys
enrolled), run `sonora-secureboot-check --require-on`, and record in
`brain/research/` the OVMF version, the outputs, `lockdown` and whether the
`rt-amd64` kernel loads. Repeat with an empty NVRAM for the removable path.
