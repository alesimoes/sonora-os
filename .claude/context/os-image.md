# Context: OS image

Local CI context: `os-image`. Agents: distro-engineer, driver-engineer.

## Where
- `iso/` (live-build: `auto`, `config`, `staged.list`), `audio-stack/`
  (PipeWire, RT limits, sysctl, WirePlumber), `drivers/alsa`.
- Scripts: `tools/build-iso.sh`, `tools/run-qemu.sh`, `tools/test-*-qemu.sh`,
  `tools/make-stick-image.sh`, `tools/iso-preflight.sh`.

## Bound by
- ADR-0002 Debian stable base; ADR-0003 PipeWire + JACK API; ADR-0005 hybrid
  ISO + persistence partition; ADR-0010 labwc kiosk with `OnFailure` fallback
  to tty2.
- The image must boot the same way every time on unseen hardware.

## Build and test
- `tools/build-iso.sh`, then `tools/test-boot-qemu.sh`, `test-install-qemu.sh`,
  `test-policycheck-qemu.sh`, `test-virtualcard-qemu.sh` (KVM, this machine);
  `audio-stack/` changes also run `tools/test-quantum-pin.sh`. The nightly
  builds the ISO and runs the USB writer against it on GitHub.
- The image takes the machine for an hour or more: through
  `tools/build-slot.sh` only, and only for iso PRs and releases.

## Traps
- Pin versions; nothing from `unstable`.
- No stick work for features that do not exist yet (product before appliance).
- Hardware numbers (UMC1820) are assumptions until measured: hand to researcher.
- Read-only root (ADR-0074, closed by ADR-0178): takes must land on `/home` (persistence or internal disk), never on `/`; running-mount and full-overlay checks are unverified until the KVM QEMU test exists.
