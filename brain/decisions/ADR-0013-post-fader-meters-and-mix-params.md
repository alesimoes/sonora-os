# ADR-0013 — Post-fader and bus meters (state layout 3), bus gain and pan as live parameters (protocol 4)

- Status: **Proposed**, 2026-10-14. Not implemented. It changes two
  contracts under `contracts/api` additively (ADR-0007 §6: review by the
  ui-engineer and the rt-engineer, recorded in the PR) and adds work to
  `Session::process()` (the rt-safety-auditor reviews that part).
- Date: 2026-10-14
- Proposed by: rt-engineer (Track 1), from BACKLOG Track 1 item 9, which
  the Mix window (PR #61, `feature/ui-mixer-monitor`) filed.
- Numbering: follows ADR-0012.

## Context

The Mix window rides the monitor bus. Three things it needs are missing
from the engine's side of the control API:

1. **Post-fader track meters.** The state block carries one peak per track
   (`TrackState::peaks`), and it is the *input* peak: `Track::report_peak`
   runs on the capture samples, before the fader, the pan, mute and solo.
   The strips label their meters IN because of that, and pre/post-fader
   metering cannot be offered.
2. **The bus meter.** `Session` already measures the monitor bus's L/R
   peak after the bus gain (`bus_peak_[2]`, a CAS max in `process()`,
   taken with `take_bus_peak()`), but the publisher does not copy it to the
   block. The bus strip says NO DATA.
3. **Live bus gain and track pan.** `ParameterChange` has one `ParamId`,
   `Gain`. A ride of the bus fader is a `SetMonitorBus` per step and a pan
   ride a `SetMix` per step: each is a model operation, so each step is a
   document change sent to every replica, where a track-gain ride is one
   `ParameterChange` per step and one `SetMix` at release.

Both version constants guard these: `kStateLayoutVersion` (2 now) for any
field added to the block, and `kProtocolVersion` (3 now) because the wire
decoder refuses a `ParamId` past `kLastParam`. An engine that does not
know a value refuses the whole message, and `EngineControlServer::refuse`
drops a socket client for it. An interface sending `BusGain` to an older
engine would be disconnected, not told why, so the version has to change
and the Hello check has to name the mismatch.

## Decision

### 1. State layout 3

- `TrackState` gains `PeakRing post_peaks[2]`: the track's contribution to
  the monitor bus, left and right, after the ramped fader coefficient
  (gain, pan, mute and solo folded together as `track_target_` already is)
  and before the bus gain. Same tagging as `peaks`: the publish tick's low
  32 bits above the float bits, one 8-byte atomic per slot, each reader
  with its own cursor.
- The header gains `PeakRing bus_peaks[2]`: the monitor bus after the bus
  gain, what `bus_peak_` holds today.
- `peaks` keeps its meaning, the input peak, so a strip can show IN, PRE or
  POST. A pre-fader *playback* peak is not in this layout. It is the
  render's output before the coefficient. Add it with the pre/post switch
  if the owner wants that switch to cover playback as well as input.
- Size: a `PeakRing` is 512 bytes, so 1 KiB more per track and 1 KiB in the
  header. At the UMC1820's 18 tracks that is about 19 KiB on the block.
- `StateView`: `TrackSnapshot` gains `post_peak[2]`, `TransportSnapshot`
  (or a `BusSnapshot`) gains `bus_peak[2]`, read through the cursor logic
  `take_peaks` uses. `FakeEngine` publishes both in the same PR: the post
  peak as its scripted or synthetic level times its published gain, which
  follows `SetMix` as the real engine's does (BACKLOG Track 1 item 9). The
  bus peak is the maximum of those.
- `kStateLayoutVersion` becomes 3, with the comment line the header keeps
  for each version.

### 2. The audio thread

- `Track` gains `std::atomic<float> post_peak_[2]`, with a `take_post_peak`
  exchange for the publisher, like `peak_`.
- In `Session::process()`, where a track's rendered block is scaled by its
  ramped coefficients and summed into the bus, keep two running maxima of
  `|sample|` per track per block and CAS-max them into `post_peak_` once per
  block, as the bus does. It costs two compares per sample per side, no
  allocation, no lock, no new branch on silence. It is measured inside the
  existing per-sample loop, so a fader ramp is metered as heard.
- `session.cpp` is shared with checklist 5 (plugin-engineer) and the #60
  fix round. This change goes in after both, or rebased on them, and runs
  under the RTSan job (`ci/rtsan`) with `test_playback_mix` covering it.

### 3. Protocol 4: `ParamId::BusGain` and `ParamId::Pan`

- Append `BusGain = 1` (linear, 0 to 4 as `Gain`; `track` ignored and sent
  as 0) and `Pan = 2` (-1 to 1, NaN refused). `kLastParam` moves to `Pan`.
- `kProtocolVersion` becomes 4. The comment names these two values.
- The server range-checks per `ParamId`. `BusGain` writes
  `Session::set_bus_gain` and `Pan` writes `Track::set_pan`, so both are
  atomics the audio thread already reads per block.
- The ride rule does not change. A ride is live parameters, then one model
  operation at release (`SetMonitorBus` for the bus, `SetMix` for pan).
  `apply_mix()` runs only when an update changed the faders' part of the
  mix, or on a Resync (BACKLOG Track 1 item 9). A new connection does not
  apply it, so a client attaching mid-ride leaves the ride alone. A client
  that leaves mid-ride (detached, or dropped for a bad frame) has the
  document's value put back on the parameters it was the last to ride,
  and only those (PR #71). The server keeps that per-track rider record
  for each new `ParamId`: the bus gain record is the bus's, a pan record
  restores the track's pan. Today the server applies `ParameterChange` as a track gain
  whatever its `param`, because `Gain` is the only value. With three values
  it has to switch on `param`.
- `FakeEngine` and the conformance suite gain the two values. The
  "parameter changes" check covers each range and each refusal, and
  "the mix follows the document" covers a bus ride stranded by a Resync.

## Consequences

- An interface and an engine from either side of this change refuse each
  other at Hello by name: layout 2 against 3, protocol 3 against 4.
- The Mix window can drop its IN label for post-fader meters, show the bus
  meter, and ride the bus and pan without a document change per step.
- One more per-sample max in the callback per track side. It is bounded and
  allocation-free. Whether it costs anything at 18 tracks at 64 frames is
  for the RTSan and timing runs to show; this ADR does not claim it is free.

## Unverified

1. The per-sample cost at 18 tracks, measured, not argued (the existing
   playback timing harness, before and after).
2. Whether a reader at 60 Hz with a 100 Hz publisher misses a post-fader
   transient. The ring is the same as the input peaks', so it should not,
   but it has not been measured here.
