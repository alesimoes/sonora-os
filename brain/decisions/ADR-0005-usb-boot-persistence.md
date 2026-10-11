# ADR-0005 — Hybrid ISO plus a persistence partition

- Status: Accepted
- Date: 2026-10-11

## Context

The system has to run from a USB stick on unknown hardware, and a session
recorded on it must survive a reboot. A live ISO alone is read-only: every take
would die at shutdown.

## Decision

`live-build` produces an isohybrid image that boots under both UEFI and legacy
BIOS. The stick carries a second partition labelled `persistence`, enabled with
the `persistence` boot parameter.

Layout written by `tools/write-usb.sh`:

| Partition | Content |
|---|---|
| 1 | The ISO image, written as-is. Read-only at runtime. |
| 2 | ext4, label `persistence`, holding `persistence.conf` with `/home union`. |

Projects live in `/home/sonora/Sessions`, which lands on partition 2.

## Rationale

- `/home union` as the only persistence mount keeps the system directories
  immutable. A corrupted overlay then cannot stop the machine from booting: the
  worst case is that settings revert, not that the studio is down.
- Recording to a USB stick is acceptable for a demo and wrong for real work.
  The image therefore also mounts any internal disk it finds and offers it as
  the session location. Flash wear and USB bus contention are real. A 32-track
  session at 24-bit 48 kHz is about 4.6 MB/s sustained, which a cheap stick will
  not hold while also serving the root filesystem.

## Consequences

- UEFI Secure Boot must be off, or we sign the bootloader. Unsigned is the
  initial state; the documentation says so plainly.
- `dd` of the ISO alone still boots, just without persistence. That is the
  documented fallback for someone testing on borrowed hardware.
