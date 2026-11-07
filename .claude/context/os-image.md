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
- `tools/local-ci.sh --os` (ISO build then QEMU tests; `audio-stack/` also
  runs the quantum pin test). Not part of `--full`; `--release` adds it.
- The image takes the machine for an hour or more: through
  `tools/build-slot.sh` only, and only for iso PRs and releases.

## Traps
- Pin versions; nothing from `unstable`.
- No stick work for features that do not exist yet (product before appliance).
- Hardware numbers (UMC1820) are assumptions until measured: hand to researcher.
