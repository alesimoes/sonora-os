# ADR-0043: Record modes: Auto Input, rolls switch, QuickPunch, TrackPunch

Status: Accepted (recommended defaults), 2026-11-04.

## Summary

Four switches finish `ui.record.punch_loop`. Three are session settings in
`Transport` (saved, not undoable, set at the engine by one command each); one
is per audio track in `TrackMix`. Session format 19 (one bump shared with ADR-0041
and ADR-0042, landed together as the session-model train), protocol 48, undo window
format 16. All default to what the engine did before, so no session changes
behaviour on opening.

| Switch | Field | Default | Meaning |
|---|---|---|---|
| Input Only | `transport.input_only` | off (Auto Input) | on: an armed track that is not monitor-Off is heard from its input at all times, stopped, rolling, in a punch or out of it |
| Pre/Post-Roll | `transport.rolls_on` | on | off: a take or punch starts at the record point and a punch stops the roll at B; `pre_roll`/`post_roll` are kept |
| QuickPunch | `transport.quick_punch` | off | on: the record key punches in and out on the fly; the punch range is not used; each pass is a take of its own |
| TrackPunch | `TrackMix::track_punch` | on | off: the armed track sits a punch take out (no file, playback heard); it records in plain takes as any other |

## Decisions

1. **Real-time.** The control thread publishes `input_only` to the session as
   a relaxed atomic and a track's `track_punch` on its `Track`; the audio
   thread reads them per block in `render_source()` (monitor) and `capture()`
   (which tracks record). Nothing is computed, allocated or locked there. The
   punch points themselves are unchanged: plan data from `PunchRange`.
2. **Rolls off** is applied where the document becomes a `PunchRange` or
   `LoopRange` and where `PlaybackController` reads the rolls, so the audio
   thread never sees the switch: a roll of 0 is the existing case.
3. **QuickPunch** is `document_punch()` returning no range. `transport.punch`
   while rolling then starts a take at the playhead and the next press punches
   out (existing `Session::punch_out()`); each take closes and is placed as a
   take layer, so the take history is kept. A loop take and QuickPunch are
   independent: Loop Record keeps its own range.
4. **TrackPunch** skips the track when the take is a punch take
   (`begin_take()` and `capture()`), and its monitor stays on playback. The
   alternative, a per-track punch range inside a plain take, was rejected for
   this slice: it needs a capture window per track through closing and
   placement.
5. Commands: `transport.toggle_input_only`, `transport.toggle_pre_post_roll`,
   `transport.toggle_quick_punch` (engine), `track.toggle_track_punch`
   (interface, through `SetMix`). Rolls and QuickPunch are refused while a take is open or requested, as the
   punch range is; Input Only is a monitor decision and may change under a
   take.

## Keys

Pro Tools: Input Only Monitoring and Pre/Post-Roll have menu items under
Options and Transport; Pro Tools documents no default key we could confirm, so
the bindings are marked `"verified": false`. QuickPunch is a record mode chosen
in the Transport window. Logic: Autopunch (`transport.punch`'s indicator) has a
key command whose default we could not confirm from the published manual, so
the binding is also `"verified": false`.
