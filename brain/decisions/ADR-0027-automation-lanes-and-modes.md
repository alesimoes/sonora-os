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
- §7 (update): Automation Follows Edit, computed by the interface into the
  region edit's `Compound`; no format or protocol change.
- §8 (update, 2026-11-02): the rest of the write slice cut into five PRs:
  writer hardening, plan-move state, per-frame sends and PDC alignment,
  Touch's return, Trim. Only Trim bumps formats. AutoMatch time is an
  engine preference, not a session setting.

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
- *As built (write slice, protocol 33):* `ParameterChange` carries a
  `gesture` (move, touch, release) and drives `ParamId::Pan` as well as
  gain. A pass is one roll, numbered by the audio thread, which publishes
  the last pass ended after its writes; a write is a span `{frame, frames,
  value}` per block, split at the loop's end, and a release is a span of 0
  frames. A later write over the same frames (a loop's next turn) wins.
  Outside what it wrote each lane keeps what it played: its old points in
  the gaps, its old value (or the static fader or pan where it had no lane)
  as a step on each run's edges. The return time is a constant 250 ms (Pro
  Tools' AutoMatch default) until the session carries one. Write plays the
  live values stopped as well. Trim stays refused until its own slice.

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

- *Amended (plugin lanes, slice 1, 2026-11-01): the frame is the block's
  first frame, not that less the chain's latency.* A change tagged with a
  block applies to the input submitted with it (ADR-0012 §5.1), and the
  chain's input in that callback is the timeline at the block's first
  frame; the chain's latency then delays the audio and the change alike.
  Less the latency would land every plugin move that many frames late,
  2048 frames behind a look-ahead limiter. The TrackFader's `L` early
  (§2) is the same rule seen from after the chain. For an insert behind a
  latent one in the same chain the exact frame is that less the earlier
  inserts' latency, which the plan does not yet carry per instance: until
  it does such an insert's lane is that much late, and only there.
- *As built (slice 1, engine/core):* `PlanPluginLane {instance, param,
  first, count}` in `PlaybackPlan::plugin_lanes`, listed by its
  InsertChain node, points in `auto_points`, `param` the dense index the
  builder resolves from the key. Once per callback that runs the chain,
  each lane's value goes to the node as a change at offset 0 when it
  moved, after the changes the control thread posted (all of the node's
  lanes after any posted one, so a knob moved under a lane never wins);
  every lane goes again after a plan swap. Read, Touch, Latch and Trim
  play the lanes; Off and Write play the knob, as a plugin parameter has
  no touch to write from yet. `PlaybackConfig::max_plugin_lanes` bounds a
  plan's lanes; more is a plan out of bounds.
- *Update (plugin lanes in the model, 2026-11-01).* `AutomationParam`
  gains `Plugin`; a plugin lane also carries `insert`, the id of an insert
  on the same track, and `key`, ADR-0012's stable ParamValue key, valid for
  that insert's format (an LV2 port symbol, a VST3 ParamID in decimal).
  Volume and pan lanes carry insert 0 and an empty key. Lanes stay on
  `TrackEdit::automation`, sorted by (param, insert, key), one each, so
  Automation Follows Edit (§7) and every lane edit treat a plugin lane as
  any other. Values are format-native, as ParamValue's: finite, and a VST3
  one from 0 to 1. At most `kMaxInsertParams` lanes an insert.
  `EditAutomation` carries `insert` and `key` beside `param`. The lanes
  follow their insert: `RemoveInsert` and `ReplaceInsert` drop them (a
  replacement is another plugin, whose keys mean other things; undo brings
  them back), `MoveInsert` carries them to the destination track, and
  `DuplicateTrack` with automation and inserts gives the copy's lanes the
  copied inserts' ids, and with automation but no inserts drops them.
  Session format **14** (lane objects of param `"plugin"` carry `insert`
  and `key`; a version-13 file migrates unchanged), window format **11**,
  protocol **36** (a version-35 peer refuses param `"plugin"` and the new
  keys). The plan builder's resolution of key to dense index (slice 1's
  `PlanPluginLane::param`) is a later engine slice; until it lands a plugin
  lane is saved, undone and replicated but not played.
- *As built (key resolution, 2026-11-02):* the builder takes a
  `LaneResolver` beside the `ChainResolver`; `PluginHosting::resolve_lane`
  answers from the running host's own instance table, the one whose store
  the chain node names, and gives the parameter's range, to which the
  points are clamped as a knob's value is. A lane is carried only on a
  chain bound with a store, and only when its key resolves: a key the
  plugin lacks, an insert that is missing, refused or not yet running, is
  left out of the plan, never mapped to another index. More than
  `max_plugin_lanes` is refused at build, naming the limit. Restoring the
  knob when a parameter leaves lane play (mode to Off or Write, a lane
  removed) is not built: the plugin keeps the lane's last value until the
  knob next moves.

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

### §7 Automation Follows Edit (update, 2026-10-31)

- Pro Tools' Options > Automation Follows Edit: a region moved (drag,
  nudge) or cleared (the region, or a range on the track's row) takes the
  automation under it along. The interface computes one `EditAutomation`
  per lane that changes (`follow_move_ops`, `follow_clear_ops` in
  `ui/qt/src/automation_edit.h`) and sends them in one `Compound` with the
  region operation: one undo step, `apply` still only validates and splices.
  No new operation, protocol or session format.
- Moving the span `[s, e)` to `d`: the lane's values on `[s, e)` play on
  `[d, d + e - s)`, the place left runs straight from the value arriving at
  `s` to the value at `e`, and nothing outside both spans changes. Each
  span's ends are steps (two points at one frame, §1): the value arriving
  and the value leaving, collapsed to one point when they agree. A clear is
  the first half alone, on lanes with a point in `[s, e]`.
- The option is the interface's, as the edit mode is: on by default as in
  Pro Tools, not saved in the session, kept when a session opens, toggled by
  the Interface-domain command `automation.toggle_follows_edit`.
- Per track (§1, alternatives): a move on the active playlist moves the
  track's lanes. Cut, Copy, Paste, Duplicate, Shuffle-mode moves, trims and
  MIDI regions do not follow yet (BACKLOG).
- A full lane (update, 2026-11-02). The edge points can take a lane past
  `kMaxAutomationPoints`, and `apply` then refuses its `EditAutomation` and
  the whole `Compound` with it, region edit included. Decided, on the
  recommended default (no owner available): the region edit goes ahead.
  Such a lane first drops the points inside the edit's span that play
  nothing, those inside a flat run (found in one pass, no tolerance; a
  step's two points stay); if it still does not fit it is left out of the
  `Compound`, unchanged, and the interface names it in a held notice
  ("Automation did not follow the Move: ..."), since a lane that no longer
  matches its region must not be said only on the status line. Refusing
  the whole edit was rejected: the operator asked for the region edit, and
  one full lane on a track should not block it.

### §8 Rest of the write slice: slicing plan (update, 2026-11-02)

Taken on recommended defaults by the architect; nobody was available.

**What is on main (8ce028ca).** Touch, Latch and Write (#178, protocol 33);
Release while tracks move (#225); a Load drops the open pass (#178
follow-up, `pass_dropped_`, `forget_latches()`). The **interface** line of
the BACKLOG item is done too, although its box is still unticked. #187
shipped the mode selector (`AutomationModeButton.qml`, strip and Edit
header). It also made `MixModel` send `Touch` at the fader and pan press and
`Release` at release. The coordinator ticks that line. Still missing: Trim,
the return time, per-frame sends, PDC-aligned lanes, and the nine #178/#225
review lines.

**Decisions taken here.**

- AutoMatch time is a **preference, not a session setting**. Pro Tools
  keeps it in Preferences > Mixing. Carrying it in the session would cost a
  format bump for nothing the session needs to replay. It becomes an engine
  start-up option, `--automatch-ms` (default 250, range 10-5000), in the
  same way as `--autosave-seconds`. A UI Preferences entry needs a protocol
  message, so it is a later BACKLOG line.
- Touch with no lane: **the static value wins**. In a writing mode a fader
  ride does not commit `set_gain`/`set_pan`, because the pass lane holds the
  move, as in Pro Tools. On release the audio glides back to the static
  value over the AutoMatch time, which is the return the writer already
  writes. Audio and lane then agree.
- A lane that is still over `kMaxAutomationPoints` after thinning is thinned
  again at twice the tolerance, at most 4 times. If it is still over, only
  that lane's edit is dropped and the client is told by name. The other
  lanes of the pass land. A non-finite live value is never written, which
  matches #243's rule for plugin lanes. NaN pan is dropped, not clamped.
- Trim is one mode, and it is **Pro Tools' Touch/Trim**. The fader writes a
  `VolumeTrim` lane from touch to release, and the Volume lane plays as in
  Read. Latch/Trim and Write/Trim need a mode pair, which is another format
  bump, so that is a BACKLOG line. The trim lane clamps to [-60, +12] dB.
  *Unverified:* Pro Tools' trim range. The researcher checks it against the
  Pro Tools Reference Guide before slice 5 merges.
- `PassWrites::add`'s linear scan stays until tracks pass about 64. That
  line stays in the BACKLOG and is not in any slice.

**In flight (do not touch their files):** Track 2, model automation
(`common/model/*`, presumably also `ui/qt/src/automation_edit.*`). Track 4b,
plugin-lane key-to-index resolution (`engine/control/src/plan_builder.cpp`,
presumably also `engine/core/include/sonora/playback_plan.h`). PR #243
(`engine/core/src/session.cpp` `gather_lanes`,
`insert_chain_runner.h`, `test_plugin_automation.cpp`). PR #244 (keymap,
conformance). Before launching a slice, check its file list against
`gh pr view <n> --json files` of whatever is open at that moment.

| # | Slice (BACKLOG lines) | Files touched | Bump | Test first (fails on main) | Review | Can start |
|---|---|---|---|---|---|---|
| 1 | **Writer hardening**: overflow keeps the other lanes; snap a merged line's end (ulp pair); NaN pan dropped in `follow_automation`; tests for Load and client-leave clearing touched flags, and long-pass growth | `engine/control/src/automation_writer.cpp`, `include/sonora/automation_writer.h`, `src/engine_control_server.cpp` (`follow_automation`, pass apply), `tests/test_automation_writer.cpp`, `tests/test_control.cpp` | none | a pass with lane A at `kMaxAutomationPoints`+1 points after thinning and lane B: B lands, A is reported; a merged line ending one ulp off the next span gives no same-frame pair; a NaN pan write leaves the lane finite | Opus (writes the document: storage) | **now**, in parallel with every in-flight item, if Track 4b's PR does not touch `engine_control_server.cpp` |
| 2 | **Plan-move and Load state** (RT): touched bits carried in `PlanTrackMix`; `auto_latched_` moves with its track on adopt; drop a pass by the plan it started on, not by the pass number | `engine/core/include/sonora/playback_plan.h` (`PlanTrackMix`), `engine/core/{include/sonora/session.h,src/session.cpp}`, `engine/control/src/playback_controller.cpp` (`adopt_track_mix`), `engine/control/src/engine_control_server.cpp` (~1016, ~2647), `engine/core/tests/test_automation.cpp`, `engine/control/tests/test_track_structure.cpp` | none (the plan is in-process; the pass's plan seq is a `Session` accessor, not the state block) | Latch on track 2, reorder while rolling: the latch follows the track; a moving plan's first 2 blocks stamp no write on the arriving track; play then Load back to back: no write lands on the new session | Opus (RT) | after #243, Track 4b and slice 1 merge |
| 3 | **Per-frame sends and PDC-aligned lanes** (RT): post-fader and follow-pan sends take the lane per frame; a track behind an insert chain reads its lane at the frames its output came from, so the loop's first pass is right | `engine/core/src/session.cpp` (send ramp, lane read offset), `engine/core/tests/test_sends.cpp`, `engine/core/tests/test_plan_pdc.cpp` | none | a send on a lane ramping inside one callback matches the fader's per-frame curve; a looped track with `L` > 0 plays the loop start's lane value, not the loop end's, for its first `L` frames | Opus (RT) | after slice 2 (same `session.cpp`) |
| 4 | **Touch's return**: release with no lane glides to the static value; `--automatch-ms` | `engine/core/{include/sonora/session.h,src/session.cpp}` (config field, `lane_param` glide for unautomated params), `engine/app/src/main.cpp`, `ui/qt/src/mix_model.cpp` (no `set_gain`/`set_pan` after a live ride), `engine/core/tests/test_automation.cpp`, `ui/qt/tests/test_mix.cpp` | none (start-up option) | Touch, no lane, ride +6 dB and release: the audio 250 ms later equals the written return lane frame for frame; `--automatch-ms 100` shortens both | Opus (RT) | after slice 3 (same `session.cpp`) |
| 5 | **Trim** (Touch/Trim) | `common/model/{include/sonora/model/model.h,src/model.cpp,src/checks.h,src/op_codec.cpp,src/session_file.cpp}` (`AutomationParam::VolumeTrim`, `SetMix` accepts Trim), `contracts/api/include/sonora/api/messages.h`, `engine/control/src/plan_builder.cpp` (copy the trim lane), `engine/core/include/sonora/playback_plan.h`, `engine/core/src/session.cpp` (gain = Volume dB + Trim dB, write the trim lane), `ui/qt/src/automation_edit.*`, lane menu and QML | **session format 15, window format 12, protocol: the next free number (38 if nothing lands first)**. A v14 file opens unchanged, and a v14 reader refuses `volume_trim`. No state-block bump | model: v14 round-trip, v15 refused by v14 codec, Trim `SetMix` accepted; engine: Volume lane -6 dB with trim -3 dB plays -9 dB, block-split invariant; a Trim pass writes only the trim lane | Opus (contracts, storage, RT) | after Track 2, Track 4b and slice 4 |

*As built (slice 2, recommended defaults, nobody available):* the pass's
plan is carried in each `AutomationWrite` (`epoch`, the plan its pass began
on), not in a `Session` accessor: an accessor names only the current pass,
and the drain can still hold the one before. A Load drops every pass begun
on a plan built before it (`PlanExchange::last_epoch()`). `PlanTrackMix`
carries `touched`, stamped at the publish (not the build, which a held gate
can keep back), and `track`, the document track, in **every** plan: the
audio thread moves `auto_latched_` and last callback's live bits by it, as
a mix serial cannot say where a track came from when a block skipped a
plan or a plan puts the tracks back where the atomics already are.
In-process only: no protocol, layout or format bump.

**Merge order:** 1 (now), then #243 and Track 4b, then 2, 3, 4, 5. Slice 1
is the only one that can run beside the in-flight work, because 2 to 5 all
edit `session.cpp`. Each slice's diff is small enough for one agent session,
so running them one after another costs less than a rebase fight would.

**ui-engineer:** the `mix_model.cpp` half of slice 4 (rt-engineer leads the
PR). Also slice 5's interface: the Volume Trim lane in the lane menu and
`automation_edit`, the Trim entry of `AutomationModeButton` no longer
refused, the fader showing the trim offset while in Trim, and screenshots
of the strip and Edit-window scenes. Slices 1 to 3 have no UI.

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
