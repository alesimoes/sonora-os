# ADR-0120 — Closure record: drivers.surfaces.contract

Status: accepted (closure record, no new decision).

## Summary

- Item `drivers.surfaces.contract` (control surface boundary, ADR-0007 4.4,
  M11) is closed by reference to the work already on `main`.
- Landed by #358 (ADR-0040): `contracts/surfaces/include/sonora/surfaces/surface_link.h`
  (`SurfaceEvent`, `SurfaceFeedback`, `SurfaceLink`, `SurfaceProtocol`,
  `kSurfaceContractVersion` = 1), its test `contracts/surfaces/tests/test_surface_link.cpp`,
  and the `contracts/CONTRACTS.lock` entry for the header.
- `drivers/surfaces` carries HUI, MCU, `engine_bridge` and `raw_midi_port`,
  each with its own test.
- No code changed by this closure. No protocol change. `kProtocolVersion` untouched.

## Evidence

- Micro build on the branch (`tools/micro-build.sh --files-from` over the
  surface header, its test, `hui.cpp` and `mcu.cpp`): built
  `sonora_surface_mcu`, `test_hui`, `test_mcu`, `test_raw_midi_port`,
  `test_surface_bridge`, `test_surface_link`; ctest on
  `surface_link`, `surface_mcu`, `surface_hui`, `surface_engine_bridge`,
  `surface_raw_midi_port`: 5 of 5 passed.
- Host: the container build image `sonora-build` (the PR gate's image).

## Not in this item

- The engine-side mapper (strip to track, bank offset, touch-gated fader
  write-back) is not part of the contract; it stays open under the surface
  work in ADR-0040's Unverified section.
- Timecode sync is `drivers.sync.*` and is out of scope here.

## Unverified

- Ring capacity 256 remains a guess until a real surface is measured
  (carried over from ADR-0040).
