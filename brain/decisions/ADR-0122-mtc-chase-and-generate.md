# ADR-0122 - MIDI Time Code: codec, chase rule and generator over a MidiPort

- Status: Accepted
- Date: 2026-11-09
- Item: `drivers.sync.mtc` (this ADR travels with its contract slice)
- Relates to: ADR-0040 (control surface boundary), ADR-0055 (surfaces over a
  `MidiPort`), ADR-0007 §1 (drivers never depend on `engine/core`)

## Summary

MTC lives in `drivers/surfaces`, beside MCU and HUI, as one header-only
contract, `sonora/surfaces/mtc.h`: `Timecode` and `FrameRate` (24, 25, 29.97
drop-frame, 30) with frame and sample arithmetic; `MtcEncoder` (quarter-frame
`F1 nn`, eight per two frames, and the full-frame SysEx
`F0 7F 7F 01 01 hr mn sc fr F7`); `MtcDecoder` (byte-fed, split-chunk safe,
locks after eight consecutive pieces 0..7, drops the lock on a gap, takes a
full frame as an immediate locate); `ChaseRule`, the pure decision, and
`MtcChase`, which feeds it the engine's published transport and sends
`transport.locate` / `transport.play` / `transport.stop` through
`api::EngineControl`. No new engine type, no `contracts/` change, no protocol
version. Nothing here runs on the audio thread.

## Context

Working with a tape machine, a video deck or another DAW needs the engine
to follow (chase) an external MTC master and to generate MTC as a master.
The control surfaces already have a byte transport, `MidiPort`, polled on the
control thread, and an established way to drive the transport through the
same commands the UI sends (`EngineSurfaceBridge`). MTC is a byte protocol of
the same shape: what it needs is a codec, timecode arithmetic that gets
29.97 drop-frame right, and a rule for when to locate, roll and stop.

## Decision

1. **Timecode arithmetic.** `to_frames` / `from_frames` count frames from
   00:00:00:00 and wrap modulo one day. Drop-frame skips labels ;00 and ;01
   at every minute not divisible by ten (17982 frames per ten minutes,
   2589408 per day). `to_samples` is the first sample at or after the frame
   start (ceil of `frames * sample_rate * den / num`, 29.97 = 30000/1001);
   `from_samples` is the frame a sample falls in (floor). With ceil/floor the
   two invert exactly at any sample rate above the frame rate.
2. **Quarter frames.** Piece *p* carries nibble *p* of frames, seconds,
   minutes, hours (low then high); piece 7 also carries the rate code in bits
   1-2. The `FrameRate` enum values are the rate code. A cycle of 8 takes two
   frames; the running encoder (`start`, `next`) advances its carried value
   by two frames per cycle. Pacing one piece per quarter frame is the
   caller's.
3. **Decoder lock.** A position is reported only when pieces 0..7 arrived
   consecutively and in order, and then at the end of every further cycle.
   The reported position is the carried value **plus two frames**, the time
   the cycle took to send: where the master is when piece 7 lands. A piece
   out of sequence, or an out-of-range value, drops the lock; a fresh 0..7
   restores it. A cycle entered half way does not lock. Real-time bytes
   (`F8`..`FF`) are skipped wherever they fall, inside SysEx too; any other
   status byte ends the message in progress.
4. **Full frame.** Any device id is accepted (`7F` is all-call). It is
   reported at once as `FullFrame`, without the two-frame offset, and it
   restarts the quarter-frame lock because the master restarts its cycles.
5. **Chase.** `ChaseRule::on_position(tc, snapshot)`:
   - engine playhead more than 2 frames from the master: `Locate` (stopped or
     rolling);
   - inside the window, master moving (0 < delta <= 4 frames since the last
     position) and engine stopped: `Play`;
   - otherwise `None`.
   `tick(frame_periods, snapshot)` counts silence in frame periods supplied
   by the caller (from a sample or event count): after 3, `Stop` if the
   engine rolls, once, and the next position starts over. No wall clock is
   read anywhere. `MtcChase` reads `EngineControl::state().transport()`
   (Rolling and Recording both count as rolling) and sends the command:
   `transport.locate` with the target sample as its argument, as the
   conformance suite already uses it.
6. **Fake.** `drivers/surfaces/testing/fake_mtc_port.h`: `FakeMtcPort`, a
   `MidiPort` that records writes and returns scripted reads in chunks of a
   chosen size, so a test can split any message at any byte.
7. **Test.** `surface_mtc_contract` (label `drivers`): literal byte strings
   worked out by hand, encode/decode roundtrip for all four rates, the
   drop-frame vectors 00:01:00;02 = 1800 and 00:10:00;00 = 17982, chunk
   sizes 1, 3 and 7, real-time bytes interleaved, gap / half cycle / reverse
   / out-of-range all leaving the decoder unlocked, and the chase rule.

## Rationale

- Header-only and allocation-free keeps the codec usable from any control
  loop without a new library; it is small enough to read in one sitting.
- Splitting `ChaseRule` (pure) from `MtcChase` (wired) lets the contract
  test drive the decision with plain values, with no engine and no clock;
  the shape the testing rules ask for (no wall-clock waits).
- Reusing `transport.locate` / `play` / `stop` means the engine, the
  protocol and `contracts/` stay as they are; chase is a client like any
  surface.
- Reporting quarter-frame positions with the +2 frame offset puts the
  latency compensation in one place, the decoder, instead of in every user.

## Consequences

- The surface loop that owns the `MidiPort` calls `MtcDecoder::poll`, hands
  positions to `MtcChase::on_position`, calls `tick` once per frame period
  it has measured without a position, and calls `MtcDecoder::reset` when the
  chase returns `Stop`.
- Chasing is coarse: locate then play, re-locate past 2 frames. There is no
  varispeed and no sub-frame phase lock; the engine has no resampling
  transport to follow a drifting master.
- The chase reads the published transport, which lags the engine; a lag
  larger than the window could send repeated locates (see unverified).
- Generation needs a caller that paces `send_next` at quarter-frame rate
  from a sample-accurate source; that wiring is the item's next slice.

## Alternatives rejected

- **A new engine type or command for sync** (e.g. `transport.chase`): the
  existing commands suffice, and a contract change would bump the protocol
  for no capability the client cannot already express.
- **Reporting the carried value as is**: every user would then have to add
  the two-frame transmission time, and someone would forget.
- **Locking on any 8 consecutive pieces, not only 0..7**: a cycle started
  mid-way mixes two values (the low nibbles of one, the high of the next).
- **Silence measured with `steady_clock`**: a wall-clock timeout is exactly
  what the testing rules forbid; frame periods counted by the caller can be
  derived from the audio sample count and are testable.
- **Chasing reverse play**: rare in practice, and pieces 7..0 would need a
  second lock path; it reads as a gap and the chase stops.
- **Linking the engine's MIDI model (`common/midi`)**: MTC here is a sync
  stream on a surface port, not musical MIDI (ADR-0055's boundary), and MIDI
  is out of v1.

## Unverified

- No real MTC device or DAW has been connected; every byte check is against
  the published MTC layout worked by hand, not a capture from hardware.
- Whether the published transport's lag stays inside the 2-frame window (2
  frames = 80 ms at 25 fps, 66.7 ms at 30) so that rolling does not trigger
  repeated locates: not measured.
- The engine's locate-then-play latency, and so how many frames the engine
  starts behind the master: not measured; there is no latency figure for
  this path yet.
- Quarter-frame timing jitter through USB MIDI or a raw MIDI node on the
  target machine: not measured.
- The generator's pacing source and its drift against the audio clock: not
  written yet.
