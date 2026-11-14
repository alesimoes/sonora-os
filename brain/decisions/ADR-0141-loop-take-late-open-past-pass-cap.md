# ADR-0141 - Loop take opened after the pass cap

- Status: Proposed
- Date: 2026-11-09
- Item: `ui.comping.loop_takes` (BACKLOG: "Loop record: a late open past `loop_pass_cap` labels its tail pass P(cap+1); drop passes past the cap using `wraps_past_cap`")
- Relates to: ADR-0016 (loop record, wrap list), `brain/specs/session-format.md` ("Loop takes keep their wraps beside them")
- Numbering: 141 reserved by the coordinator for this item

## Context

A loop take is one file per track through every pass. `Config::loop_pass_cap`
(default 99) bounds how many passes go on playlists. The disk thread keeps a
per-file wrap list in memory cut at that cap (`wrap_cap_`) and appends every
wrap to the on-disk `.wraps` list regardless (`DiskThread::wrap`); the count
past the cap comes back as `ClosedFile::wraps_past_cap`.

A track's file can open late, after the take has already wrapped. The wraps
before the file are held as `WrapSidecar::kBeforeFile` places
(`DiskThread::hold_places`) and counted in `wraps_before_file`, so the file's
passes keep the take's numbers. If the file opens after the cap, its in-memory
list is cut at the cap and holds only before-file places, so counted from the
list its passes would start at the cap and its first kept pass would be
labelled P(cap+1), a pass the operator never made on the other tracks.

## What the code does today

The code already holds, checked on `feat/ui.comping.loop_takes`:

- `loop_file()` in `engine/control/src/closed_take.cpp` rebuilds `known` from
  `wraps_before_file` when it exceeds the cut list, so the places the cap cut
  are put back. It computes passes from `min(total, cap)` wraps, where
  `total = max(take.loop_wraps, wraps.size() + wraps_past_cap)`, then erases
  every pass whose own number is greater than `cap`. It labels by `pass.pass`,
  the take's number, never by index.
- `all_passes` is counted past the cap too, so the notice can say how many
  were recorded.
- `test_a_late_window_past_the_cap_places_no_pass`
  (`engine/control/tests/test_loop_take.cpp`): cap 2, file opened in pass 4,
  `wraps_before_file == 3`, list `[before, before]`. The track gets no pass
  on a playlist (not one labelled P3), its audio is in the source pool, and
  the notice reads "take 1 recorded 5 passes; the first 2 are on playlists".
- `test_a_late_window_keeps_the_take_s_pass_numbers` (same file) and
  `test_a_late_track_numbers_its_passes_from_the_take`
  (`engine/core/tests/test_loop_record.cpp`) cover a late open under the cap.

So the BACKLOG line's behaviour is implemented and tested; this ADR records
it as the contract. It does not change code.

## Decision

1. A loop take file opened after `loop_pass_cap` wraps keeps only the passes
   whose take pass number is `<= cap`.
2. Each kept pass is labelled by the take's pass number (`.P<n>`), never
   by its index in the file. The first pass kept from a late file is never
   P(cap+1).
3. Passes numbered past the cap are not put on playlists. They stay
   recoverable: every wrap is on disk in the `.wraps` list (`wraps_past_cap`
   counts those the in-memory list dropped), and the audio is in the source.
4. A track left with no pass keeps its audio as an unplaced source, and the
   take notice names the loss: it says the take recorded N passes and only the
   first `cap` are on playlists.
5. A wrap the list never got or the boundary ring lost is placed by loop
   length, and the track is reported as such.

The cap is clamped to 1..`model::kMaxLoopTakePasses`, as `add_closed_take`
already does.

## Rationale

- Comping lanes line up the same pass under the same name on every track
  (PR #117 review, N1). A late file labelled by index breaks that.
- Dropping passes from playlists bounds the model (an operation carries at
  most `kMaxLoopTakePasses`), while the disk keeps all, so nothing recorded is
  lost: a take must not be lost.
- The audio thread never holds the list past the cap, so no growth in the
  callback; the disk thread writes the rest to the sidecar.

## Consequences

- Wrap list version 2 is required for `kBeforeFile`. An engine that reads
  wrap lists only up to version 1 refuses a version 2 list
  (`WrapSidecar` reads `1..kVersion`); its recovery then places that take by
  loop length. `session-format.md` says so.
- A loop take's passes are placed by loop length wherever the list is missing
  or incomplete; that stays an open BACKLOG line and is noted in
  `session-format.md`.
- A take that overshoots the cap shows fewer playlists than wraps; the
  operator reads the count in the notice, which is held, not the status line.
- No protocol or session version change.

## Alternatives rejected

- **Label by index (P1.. for the file's own passes).** Lanes stop lining up
  across tracks.
- **Raise the cap per late file.** The model's per-operation limit is
  shared; a late track would need more than the others.
- **Drop the late file's audio from the pool.** The take would be lost
  beyond the cap.
- **Keep the whole wrap list in memory.** It grows in the audio-thread-reachable
  path or needs an allocation there.
- **Refuse the take.** An operator who loops 100+ times has recorded real audio.

## Unverified

- The notice's wording for a track with several dropped passes is tested for
  one case (cap 2, 5 passes), not for a cap of 99 or `kMaxLoopTakePasses`.
- A session saved by an engine with wrap list v2 and opened by a v1 engine
  has not been run; the refusal is from the reader's version check.
- Recovery of a late file's passes past the cap after a crash is not tested
  here (recovery reads the list from disk; the late-file recovery tests use
  caps not exceeded).
- Nothing was built or run for this ADR; the tests named are read, not run.
