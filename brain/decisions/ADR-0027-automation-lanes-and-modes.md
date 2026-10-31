# ADR-0027 — Automation: breakpoint lanes in the document, evaluated per frame from the plan

- Status: **Accepted**, 2026-10-31, on the recommended defaults (the
  coordinator, under the owner's standing rule to take recommended
  decisions).
- Date: 2026-10-31
- Catalogue: `automation.lanes`, `ui.mixer.automation`.
- Numbering: ADR-0026 is the playlists PR's (#167); protocol 31 is its too.

## Summary

*Navigation only; the sections below are the decision.*

- §1: a track's automation is a list of **breakpoint lanes** on
  `TrackEdit`, one per parameter, and is **undoable**, as in Pro Tools. One
  operation, `EditAutomation`, replaces a lane's points in a frame range;
  pencil, thin, cut, copy and paste are all that one operation. The
  **mode** (off, read, touch, latch, write, trim) is per track mix state,
  saved and not undoable, as Pro Tools' automation mode is.
- §2: the plan carries each lane as an immutable point array. The audio
  thread evaluates it statelessly at a 32-frame grid anchored to timeline
  frames and interpolates the coefficients between grid points: sample
  placed, zipper-free, identical however the callbacks are cut. A
  discontinuity is crossfaded over one callback from what was last applied.
- §3: live writes (touch, latch, write, trim) never make the audio thread
  allocate: the UI or a control surface sends values to the control thread;
  the audio thread applies a per-track override from atomics and stamps
  what it applied into a fixed ring; the control thread turns a pass into
  one `EditAutomation`, one undo step.
- §4: plan swaps, locate and loop: a swap or a locate is a discontinuity
  (one-callback crossfade); a loop wrap is not (the lane at the loop start
  plays at the wrap, as the audio does). Stopped, the lane's value at the
  playhead applies.
- §5: plugin parameters ride the same lanes, keyed by insert and parameter,
  evaluated once per block into ADR-0012's block-tagged last-value table.
- §6: first slice: read-mode volume and pan on audio tracks. Session
  format 13, window format 10, protocol 32.

## Context

Nothing in the tree automates anything. The Qt mixer's fader *ride*
(`ui/qt/src/mix_model.*`, PR #162) sends live `ParameterChange` steps while
the fader moves and one `SetMix` at release; the engine ramps each mix
value once per callback (ADR-0009 §4.5). ADR-0009 §3 keeps mix state out of
undo. Pro Tools and Logic both treat automation *data* as an edit: drawing,
thinning, pasting and a written pass are undone like a region move, while
the automation *mode* is a strip setting.

## Decision

### §1 The data model

- `AutomationParam` names what a lane drives: `Volume` (dB, in
  `[kMinGainDb, kMaxGainDb]`, converted as the fader is) and `Pan`
  (-1 to +1) now; mute, send level and pan, and plugin parameters
  (insert id plus ParamValue key, §5) are later values of the same enum and
  key.
- `AutomationLane { param, points }`; `AutomationPoint { at (frames), value }`,
  sorted by `at`, at most two points at one frame (a step: the first is the
  value before, the second after). At most `kMaxAutomationPoints` (65,536)
  per lane. `TrackEdit::automation` holds the lanes, sorted by param, each a
  `shared_ptr<const AutomationLane>` so an undo step shares every lane it
  did not change. A lane with no point is removed.
- Between points the value is linear in the lane's unit (dB for volume,
  position for pan); before the first and after the last it holds.
- **Undoable**: `EditAutomation { track, param, from, to, points }` replaces
  the points in `[from, to)` with `points`, all of which lie in it. The
  whole lane is `[0, max)`. Pencil, the grabber's breakpoints, thin, cut,
  copy and paste are computed by the interface into this operation; a paste
  over several lanes is a `Compound`. It is resolved (ADR-0009 §2): `apply`
  only validates and splices.
- **Mode**: `AutomationMode { Off, Read, Touch, Latch, Write, Trim }` on
  `TrackMix::automation`, default Read. Saved, not undoable. Off plays the
  static fader and pan; Read, Touch and Latch play the lanes until a
  parameter is touched (§3). Until §3 lands, `SetMix` refuses Touch, Latch,
  Write and Trim with a reason naming the slice.

### §2 How lanes reach the audio thread

- The builder copies every Volume and Pan lane of a track with audio into
  one pool, `PlaybackPlan::auto_points`, and records `{first, count}` per
  parameter on `PlanTrack`. The plan stays one allocation (ADR-0009 §4);
  `plan_in_bounds()` checks every range.
- The mode reaches the audio thread as every mix value does: a `Track`
  atomic, and `PlanTrackMix::automation` while a track lags its plan
  (ADR-0023 §4). A mode change therefore needs no new plan. A parameter is
  *automated* in a callback when the mode plays lanes and its lane has
  points; the static value of an automated parameter is ignored, as a Pro
  Tools fader follows its automation.
- Evaluation is a pure function of the timeline frame `t`. At grid frames
  (multiples of 32) the lane is found by binary search, volume converted to
  linear and pan through the plan's pan law, giving a coefficient per side;
  frame `t` uses the linear interpolation between the grid frames either
  side of it. No cursor, so a locate needs no reseek, and the result for a
  frame does not depend on where a callback starts: playback is
  block-split invariant in steady state, bit for bit. The grid caps a step's
  slope at 32 frames, which is the declick.
- Mute, solo and the crossfades of §4 multiply on top: a callback that is
  not continuous with the last ramps from the coefficient last applied to
  the evaluated one, `c0 + (open * c(t) - c0) * a`, `a` rising over the
  callback as the fader's ramp does now.
- A track with inserts evaluates its lanes at the TrackFader `L` frames
  early, `L` the chain's compensated latency, folded back into the loop, so
  automation stays aligned with the audio it scales.
- Post-fader sends and follow-pan sends take the lane's value at the
  callback's first frame, through the existing per-callback send ramp. Per
  frame sends are a later refinement.

### §3 Modes and live writes (decided now, built in the write slice)

- A touch from the interface or a control surface goes to the control
  thread (the existing `ParameterChange` path, with touch and release).
  The control thread stores the live value and a touched flag in per-track
  atomics; the audio thread reads them once per block and plays them
  instead of the lane, ramped, in Touch, Latch, Write and Trim.
- Each block in which a written parameter is live, the audio thread pushes
  `{track, param, frame, value}` into a fixed `RingBuffer` sized at session
  construction; a full ring is counted as an overrun, never waited on. The
  control thread drains it into a buffer reserved when the pass starts.
- Touch writes from touch to release, then returns to the lane at the
  session's glide time; Latch from touch until stop; Write from play to
  stop; Trim writes a relative dB lane (`VolumeTrim`) summed in dB with
  Volume, in the mode it is combined with. At the pass's end the control
  thread thins the pass and applies one `EditAutomation` per written lane,
  in a `Compound` labelled "Automation Write": one undo step per pass.
- After a pass Write falls back to Latch, as Pro Tools does by default.

### §4 Plan swaps, locate and loop

- A callback is continuous when it starts at the frame the last one would
  have continued to (looped, or the playhead, stopped), the plan is the
  same, the track's automated set is the same and mute and solo did not
  change. Otherwise it crossfades (§2) over that callback. So an edit under
  the playhead, a locate mid-ramp and a mode switch never click; the next
  callback plays the lane exactly.
- A loop wrap inside a callback is continuous: frames after the wrap take
  the lane at the loop start directly, as the audio jumps without a fade.
- Stopped, every frame of the callback takes the lane's value at the
  playhead, so the fader shows and plays what the next play will start at.

### §5 Plugin parameters

Plugin lanes are keyed by insert id and ADR-0012's parameter key, carried in
the plan beside the mix lanes, and evaluated once per block at the block's
first frame less the chain's latency, into the block-tagged last-value
table that ADR-0012 §5 already gives the host. No new IPC. They come after
the write slice.

### §6 First slice, format, protocol

- Slice 1: read playback of Volume and Pan on audio tracks; `EditAutomation`
  with undo; `TrackMix::automation` with Off and Read.
- Session format **13**: `automation` on every track with audio, a list of
  `{param, points: [[at, value], ...]}`; `automation` (the mode) in each
  track's mix. A version-12 file migrates with no lanes and every mode Read,
  which plays what it played.
- Window format **10**: the undo window's tracks carry their lanes.
- Protocol **32** (31 is PR #167's): the operation codec has
  `EditAutomation`, and `TrackMix` carries `automation`. A version-31 peer
  refuses both.

## Consequences

- Undo history grows with lane edits; lanes are shared between steps, so a
  step costs the lanes it changed.
- The fader path gains a per-frame branch for automated tracks only;
  unautomated tracks run the existing code.
- The send follow is block rate for now (§2), a BACKLOG line.

## Alternatives rejected

- **Automation as mix state, outside undo.** Pro Tools and Logic undo
  automation edits and passes; an operator who thins a lane by mistake
  expects Undo.
- **Lanes per playlist.** Pro Tools keeps automation with each edit
  playlist, so switching playlists switches the mix moves; Logic keeps it
  per track. Per track (Logic) is taken: a comp built across playlists
  (ADR-0026) keeps one mix. Moving the lanes onto `Playlist` later is a
  format bump, not a redesign. Decided (2026-10-31, the owner's question
  answered on the recommended default): per track; the Edit window's lanes
  (`automation.lanes` UI) are disclosed per track.
- **Interpolating on the control thread into a per-block value table.**
  Block-rate values zipper on fast rides and change with the period size.
- **A cursor per lane on the audio thread.** Faster per frame, but every
  locate, loop and swap must reseek it; the stateless search at grid rate
  costs a few compares per 32 frames.
- **Writing passes on the audio thread into the plan.** The plan is
  immutable once published (ADR-0009 §4); the audio thread would have to
  allocate or own a second copy.
