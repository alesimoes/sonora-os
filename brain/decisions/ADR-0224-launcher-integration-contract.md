# ADR-0224 - Clip launcher integration: voices, controller, clip-to-region resolution

## Summary

Builds on ADR-0164. The launcher's `TransitionList` drives audio through a
`LaunchVoiceSource` (`start(track, clip, offset)` with the clip's loop length,
`stop(track, offset)`), fed by `drive_voices()` from a per-cell `ClipTable`
sized at session build. A cell's `ClipId` is a region's model id, resolved to
a `ResolvedClip` (source offset, loop length, gain) on the control thread and
handed to the audio side before the grid learns the id. The control half is
the in-process `LaunchController` in `engine/control`: assign, trigger, stop,
record-to-cell and bounce-to-arrangement, with held-notice refusals. No wire
change: no protocol number is reserved. `FakeLaunchController` runs the real
state machine against an in-memory document for tests and the UI slice.

## Context

ADR-0164 gave the launcher a quantised state machine whose `advance()` returns
the starts and stops inside each audio block, with their frame offsets, and
left three things to the engine slice of `logic.live_loops`: what plays when
a transition fires, how a cell's `ClipId` becomes audio, and how the operator
records into a cell or writes a performance into the arrangement. The engine
slice and the UI slice start in parallel from this contract, so it must fix
the seams (interfaces, refusals, ordering between threads) before either has
code. The audio thread may not allocate, lock, log or throw (CLAUDE.md); no
protocol version is reserved for this feature, so the API cannot grow a wire
command yet.

## Decision

1. **Transitions drive voices** (`engine/control/include/sonora/launch_voice_source.h`).
   - `LaunchVoiceSource` is the audio side's voice bank, one voice per track
     column (the launcher plays at most one cell per column).
     `start(track, const ResolvedClip&, offset)` starts the clip `offset`
     frames into the block and loops it every `ResolvedClip::loop_frames`;
     `stop(track, offset)` stops the column's voice there. The voice splits
     its block at the offset; the declick ramp at a stop is the voice's.
   - `drive_voices(list, table, voices, counts)` applies one block's
     `TransitionList` in list order. The launcher lists a column's stop
     before its start on the same frame, so a re-trigger is heard as one
     seamless hand-over. It is header-only, allocates nothing, and a test
     checks it with a counting global allocator.
   - `ClipTable` holds one `ResolvedClip` per cell, sized tracks x scenes when
     the session is built, never resized. `find(track, scene, clip)` returns
     the slot only when it holds the very clip the transition names and its
     loop length is positive. Otherwise the start is refused and counted in
     `DriveCounts::unresolved` (reported through the state block, never
     logged); the launcher still shows the cell playing.
   - Order between threads: Session::process() drains the clip ring into the
     table, then calls `Launcher::advance()`, then `drive_voices()`, then
     pushes one `LaunchEvent` per transition (absolute frame) to the event
     ring. A full event ring is an overrun: counted, nothing waits.
2. **Clip-to-region resolution.** A `ClipId` is the model `Id` of a `Region`
   (ADR-0164 1). On `assign_region(track, scene, region)` the controller
   reads the open document on the control thread and refuses a region that
   does not exist (`NoSuchRegion`), lies on another track's playlists
   (`RegionOnOtherTrack`: channel count and input follow the track) or is
   empty (`EmptyRegion`). It snapshots `source_offset`, `length` (the loop
   length) and `gain_db` into a `ResolvedClip`, pushes it to the clip ring,
   and only then calls `Launcher::assign_clip`. Because the clip ring is
   drained before `advance()`, the table is never behind the grid. If the
   launcher's queue refuses the assignment (`QueueFull`), the controller
   pushes the cell's previous clip back on the ring so table and grid agree.
   An edit to an assigned region is not seen by the cell; the controller
   re-resolves and re-assigns on the document's change notification, and
   clears the cell when the region is removed.
3. **Record-to-cell** (`RecordToCellRequest{track, scene, bars}`). Refused
   without a session, outside the grid, on a track not record-armed
   (`NotArmed`), while a linear take is open (`TakeOpen`), or while the
   column already records into a cell (`CellRecording`). Accepted, the take
   starts on the launcher's next boundary on the track's input and ends
   `bars` bars later (0: at the next trigger or stop on the column). The
   recorded region goes into a playlist of the track named "Launcher", never
   the arrangement's active playlist, as one undo step; it is then assigned
   to the cell through section 2 and launched, so it plays from the boundary
   where the take ended. The record path is the existing take machinery
   (ADR-0016); the launcher only chooses the frames.
4. **Bounce-to-arrangement** (`BounceToArrangementRequest{from, to}`). The
   controller keeps a performance log from the event ring (`pump()`). A
   bounce pumps, pairs each start with the next stop or start on its column
   (a span still open runs to `to`), and resolves each span with
   `place_span()`: one pass per loop length counted from the span's real
   start, cut to the window, each pass's source offset moved on when the
   window cuts into it, and `Region::loop` set to the pass index so the
   passes form one clip group as Loop Clip's do (ADR-0052). The passes are
   placed on each track's active playlist, overwriting what was there, as
   one compound and one undo step, "Launcher to Arrangement". Refused with
   no session, while a take is open, on an empty or reversed range
   (`BadRange`), and with nothing played in the range (`NothingToBounce`).
   Transitions lost to a full event ring inside the range are returned in
   `BounceResult::events_lost`, and the UI warns that a span may be missing.
5. **The controller** (`engine/control/include/sonora/launch_controller.h`)
   is in process only: the control server calls it directly, and no wire
   command exists until a protocol number is reserved for one (a later
   amendment to this ADR). Every refusal is a `LaunchRefusal` the UI shows
   as a held notice, because a pressed cell that does nothing must be
   explained. Cell views (`cell()`) come from the events, not from the audio
   thread's grid. `reset()` forgets the log, the views and the recording
   state when a session opens or closes; the engine builds a new launcher and
   table with the session.
6. **The fake** (`fake_launch_controller.h`) runs the real
   `StateMachineLauncher` and the real `drive_voices()` against an in-memory
   region list and a voice bank that records its calls; `run_block()` stands
   in for Session::process(), `finish_recording()` for the record path. Its
   rings are vectors (it allocates; the engine's do not). The contract test
   `engine/control/tests/launch_controller_contract_test.cpp` (label
   `control`) covers every refusal, the frames, a re-trigger, an unresolved
   start, the resolution of spans, bounce, record-to-cell, a full queue, a
   lost event, a reset and the allocation-free drive.
7. **Build.** The headers are header-only on `sonora_launch`; a new module
   edge `engine/control -> common/launch` is listed in
   `tools/module-deps.d/launch.allow`. The test joins through a deferred hook
   in `common/launch/CMakeLists.txt` including `engine/control/launch.cmake`
   while `engine/control/CMakeLists.txt` is held by other features.

## Rationale

The voice bank is the narrowest interface the playback path needs: a frame
and a loop length per column, which is all a transition carries. Matching the
clip id in the table, rather than trusting the slot, turns the one race the
two rings allow (a resolution lost to an overrun) into a counted silent start
rather than a clip played at another region's length. Resolving on the
control thread keeps the model's headers and locks out of the callback, as
ADR-0164 kept the launcher out of the document. Writing the recorded take to
a "Launcher" playlist keeps the arrangement untouched until the operator asks
for it, which is what bounce-to-arrangement is for; computing passes from
the real start keeps the bounce in phase with what was heard even when the
window starts mid-loop. An in-process interface lets the UI slice work
against the fake today and lets the wire mapping wait for a protocol number
without blocking anything.

## Consequences

- The engine slice implements `LaunchVoiceSource` over the disk reader's
  prefetch (one voice per launcher track), owns the clip and event rings and
  wires the order of section 1 into Session::process().
- The engine slice implements `LaunchController` over the document and the
  record path; the UI slice codes against `LaunchController` and tests with
  `FakeLaunchController`.
- A wire command for the launcher needs a protocol number and an amendment
  here; until then the launcher is driven from the in-process server only.
- `DriveCounts` and the event ring's overruns join the state block in the
  engine slice.
- When `engine/control/CMakeLists.txt` and `tools/module-deps.allow` free,
  the hook and the drop-in file move into them.

## Alternatives rejected

- Voices reading the model's `Region` directly at start: the callback would
  include the model's headers and race its edits; resolution belongs to the
  control thread (ADR-0164's split).
- Carrying the resolved clip inside the launcher's command: it would change
  ADR-0164's merged `Launcher::assign_clip(ClipId)` contract and grow every
  queued command for the one kind that needs it.
- A table keyed by `ClipId` with a search: per-cell slots are what the grid
  is, are found in constant time, and make the id check the race guard.
- Recording a cell straight onto the active playlist: every launcher take
  would rewrite the arrangement under the operator's performance and fill the
  undo history with takes they may discard.
- Bounce passes counted from the window's start: they would be out of phase
  with what played whenever a span began before the window.
- A wire command now: no protocol number is reserved; taking one is the
  coordinator's to do, and the UI can work in process against the fake.

## Unverified

Handed to the researcher:

- Whether `common/model` has an operation to clear a range on a playlist
  before placing the bounce's passes (overwrite on place is assumed from
  playlist ordering), and whether a compound can create the "Launcher"
  playlist and its region in one undo step.
- The declick ramp length at a voice stop and whether region fades apply at
  each loop wrap; v1 assumes gain only.
- Where the disk reader prefetches launched clips from: a launch can start any
  cell within one bar, so every assigned clip's head must stay resident.
- How the transport snapshot reaches `advance()` under a tempo change and
  after a locate (carried over from ADR-0164).
- Whether a clip whose region is edited while it plays should change at its
  next loop wrap rather than at the next assignment.
- RTSan has not run on `drive_voices()`; the claim rests on the counting
  allocator test and review.
