# ADR-0052 — Clip groups, clip loops and sample-level edits: tags on the region, and copies for anything destructive

- Status: Accepted (recommended defaults, taken by the clip-editing agent, 2026-11-04)
- Date: 2026-11-04
- Builds on: ADR-0026 (playlists), ADR-0042 (clip gain), ADR-0031 §6 (Consolidate),
  ADR-0004 (what we reproduce), session-format.md "Versioning"
- Progress items: `ui.editing.clip_groups`, `ui.editing.sample_edit`

## Summary

- §1: a **clip group** is the regions of one playlist that carry the same non-zero
  `clip_group`; the group's id is the id of one of its member regions, so no id is
  reserved. A **loop** is a group whose extra members are repeats: `loop` is 0 for a
  region the operator made and k >= 1 for the k-th repeat. A repeat is an ordinary
  region of the same source; **the engine plays nothing new** (the plan builder and the
  audio thread do not know groups exist).
- §2: two operations. `SetClipGroup{playlist, group, members[{region, loop}]}` tags
  or (with group 0) frees regions, one undo step. `SetRegionSources{playlist, region,
  channels, source_offset, gain_line}` points a region at other files, one undo step.
  **Session format 24, protocol 57, undo window 20**; a v19 file opens unchanged, a
  v23 file holding `clip_group` or `loop` is refused as a reader of 23 would.
- §3: interface: Group Clips (the clips the range touches, two or more, none grouped),
  Ungroup, Loop Clip (N repeats, after the clip or the group; looping again lays the new
  count), Unloop. A group moves as one: the same distance for each member, stopping at
  the session start. Repeats are drawn with a bar along their foot and "loop k" in the
  name. Copy, paste and Repeat make plain clips (they drop the tag).
- §4: **sample edits are made by the engine, into a new file.** `sample.edit` takes
  `VERB REGION FROM TO CHANNEL [PARAM]` and writes the region's span plus a 2 s handle
  either side, with the stretch processed, to `Audio Files/<track>_edit_NN.wav`,
  exclusive create, synced, then applies one Compound (AddSource, SetRegionSources). The
  take is never opened for writing; undo points the region back at it. Verbs: repair
  (cubic Hermite from the neighbours' slopes, at most 2048 samples: a click), draw
  (the Pencil's points), silence, reverse, invert, gain, normalize, fade_in, fade_out.
- §5: a refusal of `sample.edit` is a held notice, not the status line: the operator
  must not believe the samples changed.

## Decision

### 1. Why tags on the region, not a nested clip object

Pro Tools' clip groups nest and have a clip-list entry of their own. That needs a second
kind of object in the playlist, in the plan builder, in every region operation, and in
the session format. The workflow the engineer has in hand is "these clips move together"
and "repeat this clip". Both are met by a tag: a group member is a region, so split,
trim, fades, crossfades, gain and colour keep working on it unchanged, and Edit-window
operations on one member never ask "what is a group". The cost: no nesting, and a
trim of one member does not trim its repeats (Pro Tools' looped clip shares its
content; ours are copies, trimmed one by one). Both are listed as follow-ups.

`SetClipGroup` is refused when the group's name is not among its members, when a
region outside the list already holds the group, when a repeat names the group's own
region, and (group 0) when a member keeps a loop number. A repeat with no group is an
invalid region.

### 2. Loop Clip

Loop N lays N repeats after the clip, or after the span of the group's originals
(`loop == 0` members): repeat k of each original at its position plus k times the span.
Repeats overwrite what is beneath, as a paste does. One Compound labelled "Loop
Clip": remove the repeats an earlier Loop laid, tag the originals, place the copies
(each carrying `clip_group` and `loop`). Unloop removes the repeats, and ungroups a
loop of one clip. The count asked last is kept for the next Loop without an argument
(3 at first).

### 3. Group move

`submit_move` expands a move of a grouped clip into one Compound of MoveRegion, "Move
Group": the distance is the dragged clip's, clamped so no member starts before frame 0;
the members go leading first so one never lands on another that has not moved. A member
that lands inside a clip outside the group splits it (a reserved id, as a single move).
Automation Follows Edit does not follow a group move yet.

### 4. Sample edits and the original take

- The interface has no audio and no session folder (waveforms are synthetic overviews until
  the peak files land); the engine owns the files. So the edit is an engine command,
  built by the interface from the selection (a range inside one clip), run on the
  control thread (it reads and writes files; the audio thread never waits for it), and
  refused while a take is open or closing.
- The copy holds all the region's channels (a mono edit of one channel copies the
  others unchanged), in the session's file format. Int24 sessions clamp to full scale.
  The new source's BWF time reference is the take's plus the copy's first source frame.
- The gain line is in source frames (ADR-0042), so it is rewritten for the copy: points
  before the copy fold into one at frame 0 holding their level there.
- A stretch is at most 16 million frames (held in memory, one channel at a time).
  Normalize uses one gain for every channel edited so a stereo image keeps its balance.
- The copy has a handle of 2 s of source so an existing crossfade or a later trim has
  material; a crossfade that still loses its handle is dropped as after any edit.
- Not done: a pointer-drawn Pencil on the waveform, and a window showing individual
  samples. The overview is still synthetic, so there is nothing true to draw on; Draw
  takes its points as an argument and the Pencil follows when the real waveform lands.

## Consequences

- Looped repeats cost nothing at playback. A loop of a 10 s clip x 100 is 100 regions
  (the Compound member limit, 256, bounds it).
- Sample edits grow the session folder by the region's span each time, never shrink it;
  the file is out of the pool only if the model refused after the write (a notice names
  it). Cleaning up unused edited files belongs to the same job as unused takes.
- Changing a merged contract is an amendment asked of its owner: `SetRegionSources`
  and `SetClipGroup` are additive; Consolidate (ADR-0031) is untouched.
