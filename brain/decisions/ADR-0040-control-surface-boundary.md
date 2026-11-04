# ADR-0040 — Control surface boundary as landed (ADR-0007 4.4, M11)

Status: accepted. Amends ADR-0007 section 4.4.

## Summary

- `contracts/surfaces/` holds `SurfaceEvent` (surface to engine) and
  `SurfaceFeedback` (engine to surface), both trivially copyable, and
  `SurfaceLink`, two `RingBuffer`s with drop counters.
- 4.4 as first sketched gave protocols a direct `api::EngineControl&` and a
  `StateView`. That puts HUI/MCU parsing on a thread that would call into the
  engine. It is replaced: the surface thread only pushes events; the engine
  side drains them at a block boundary and issues the same commands as the UI.
- A full ring drops the item and counts it; neither side waits.
- Strips are the surface's own indices; banking maps them to tracks on the
  engine side.
- No protocol code here. HUI and MCU are separate items that implement
  `SurfaceProtocol`. `kProtocolVersion` is not touched.

## Decision

Events cover fader (with touch), pan, mute, solo, arm, select and transport.
Feedback covers motor fader, pan, mute, solo, arm, meters (latest wins),
scribble strip text (7 characters, space padded) and transport lights.
Meters and fader targets are state: the engine side may coalesce them per
tick and a drop is harmless. A dropped button event is visible in the
counter, never silent.

## Unverified

The engine-side mapper (strip to track, bank offset, touch-gated fader
write-back) is not built; the test uses a fake engine. Ring capacity 256 is a
guess until a real surface is measured.
