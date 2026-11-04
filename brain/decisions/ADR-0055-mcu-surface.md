# ADR-0055 — MCU control surface over MIDI ports (drivers.surfaces.mcu)

Status: accepted. Builds on ADR-0040; amends it on banking.

## Summary

- `drivers/surfaces/` implements the Mackie Control Universal protocol as a
  `SurfaceProtocol` (ADR-0040) over a `MidiPort`. It is not a MIDI
  instrument feature: nothing links the engine's MIDI model.
- Transport is `MidiPort` (read, write raw bytes). `RawMidiPort` opens a
  raw MIDI device node (`/dev/snd/midiC*D*`) non-blocking, with no ALSA
  library dependency; `FakeMidiPort` scripts a device for tests.
- Banking lives in the protocol, not the engine side. Events carry the
  absolute strip (bank offset + physical strip). This amends ADR-0040, which
  put the offset on the engine side, because there is no bank event in the
  contract and the engine should not learn what a bank button is. The cost:
  kMaxStrips = 32 bounds the reachable tracks.
- `EngineSurfaceBridge` is the engine side: it drains events into the same
  `CommandRequest` and `ParameterChange` the UI sends through
  `api::EngineControl`, and publishes `StateFrame` as feedback, only what
  changed. Fader touch is `Gesture::Touch` ... `Release`.
- Protocol details and sources: `brain/research/mcu-protocol.md`.

## Decision

The surface thread runs `McuProtocol::poll()`; whoever ticks the control
client runs the bridge's `drain()` and `publish()`. They meet only in the
`SurfaceLink`. A touched fader is not driven by the motor (the protocol) and
not fed back (the bridge), both, so that either side alone is safe.

Contract gaps found, left for the contract's owner, none worked around by
editing the contract:

1. No time feedback: the time display is set by `McuProtocol::set_time_display`
   from the surface thread's owner.
2. No pan in `TrackSnapshot`: V-Pot rings follow the surface's own turns and
   the engine's `Pan` feedback if a source pushes it; the bridge pushes none.
3. Jog has no scrub command: it is `Rewind`/`Forward` with `value` = detents,
   issued as at most four `transport.rewind` / `transport.fast_forward`.
4. Master fader and the master strip have no event.
5. The fader taper is a copy of `ui/qt/src/fader_taper.cpp`'s table; moving
   it to `common/` would make it one table.

## Unverified

No real MCU, X-Touch or other device was attached. See the research note for
the list (cycle note, handshake, meter scaling, ring capacity).
