# ADR-0055 — MCU and HUI control surfaces over MIDI ports (drivers.surfaces.mcu, .hui)

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
- `HuiProtocol` (drivers.surfaces.hui) is the second `SurfaceProtocol` on the
  same `MidiPort`, `SurfaceLink` and `EngineSurfaceBridge`: zone/port
  switches, faders with touch, V-Pots, LEDs, the 4-character display, the
  time display, meters, and the ping keep-alive. It banks in the protocol
  too, as the MCU does.
- Both protocols are described only by community write-ups, not by an
  official specification; the sources are in the research notes and no real
  device has confirmed either.
- Protocol details and sources: `brain/research/mcu-protocol.md`,
  `brain/research/hui-protocol.md`.

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

## Proposed amendment to the surface contract (for the contract owner)

Nothing under `contracts/` is changed by this ADR. The gaps above, as one
amendment the owner of ADR-0040 can take, in the order the surfaces need them:

1. `FeedbackKind::Time` (or a time struct on the link): a transport position.
   Both protocols have a time display and today the owner of the surface
   thread must call `set_time_display`. Decide whether the engine side sends
   text or numbers; the digit counts differ (MCU 10, HUI 8).
2. `TrackSnapshot::pan` (api) so the bridge can push `FeedbackKind::Pan`;
   today rings follow only the surface's own turns.
3. `TransportCmd::Scrub`, or an `EventKind::Jog` with a signed detent count,
   so the jog wheel stops being a capped burst of rewind/forward commands.
4. A master strip: a strip index or flag for the master fader, plus master
   fader feedback. The MCU has one (pitch bend channel 8), which we ignore
   today.
5. Not a contract change: move the fader taper to `common/` as one table.
6. Optional: a short name in the scribble feedback; HUI shows 4 characters of
   the 7 and the first four are not always the best four.

Also needed once a real device is attached, and configuration rather than
contract: which MIDI device node is a surface, and MCU or HUI for each.

## Unverified

No real MCU, HUI, X-Touch or other device was attached. See the research
notes for the lists (MCU: acceptance of the handshake reply by a unit, the
cycle note on a device, meter scaling, ring capacity; HUI: everything
single-source, the transport and channel-select port order, the blank digit).
