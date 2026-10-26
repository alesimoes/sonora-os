# ADR-0020 — The click and the count-off: beats in the plan, synthesised on the audio thread

- Status: **Proposed**, 2026-10-25, with the recommended defaults below,
  for the coordinator to accept or amend at review.
- Date: 2026-10-25
- Proposed by: rt-engineer (Track 1), backlog item `engine.core.click`
  ("Click generated in the engine from the tempo map, routed to the monitor
  bus, count-in").
- Amends: `brain/specs/midi.md` §2.5 (what the audio thread may compute from
  the tempo map), `brain/specs/session-format.md` (`transport`, version 8).
  Changes the state block (layout 4) and the control protocol (15).
- Numbering: follows ADR-0019.

## Summary

*Navigation only; the sections below are the decision.*

- §1: engine/control turns the tempo map into one `ClickSegment` per entry
  and puts them in the playback plan. The audio thread finds beat j of a
  segment with the map's own rounding, in 64-bit steps, so every downbeat is
  on the frame the rulers and the grid show, and it never calls
  `MusicalMap`.
- §2: two sounds, synthesised once when the session is made, accented on
  beat 1 of the bar. Added to the plan's click outputs after the bus gain,
  which in Phase 2 are the monitor bus's outputs. Never to an input, so no
  take can capture it.
- §3: count-off. A take started with the transport stopped, while count-off
  is on, first plays `transport.count_off_bars` bars of click at the tempo
  and meter where it starts. The playhead stands still and nothing is
  recorded until the last frame of the count-off; the take then starts as
  it would have. Default 2 bars, saved in the session (format 8).
- §4: the click and count-off switches are engine state, toggled by
  `transport.toggle_click` and `transport.toggle_count_off` and shown in the
  state block's `transport_flags`. Not saved yet.

## Context

The click must land where the interface draws its beats. `MusicalMap` is the
one conversion every reader shares (`midi.md` §2), but it allocates and
divides in 128 bits, and §2.5 keeps it off the audio thread. The plan
carries frames. A table of every beat frame is unbounded (an hour at 1000
bpm in 64ths is millions of beats), and a horizon refreshed by the control
thread would race every locate and loop.

The count-off has to work from frame 0, so it cannot be a pre-roll that
locates earlier on the timeline; and it must end on the sample where the
take begins, or the performer's first downbeat is late by a control-thread
turn.

## Decision

### §1 Beats in the plan

`click_segments()` (engine/control, `plan_builder.h`) builds a
`ClickSegment` for each reachable entry k of `MusicalMap`: its frame a_k,
its exact ticks per frame num/den, the beat length in ticks, the beats to
the bar, and its first beat: the first tick at or after T_k on the grid of
the bar in progress (a tempo change carries the bar on; a meter change
starts one at its tick), with that beat's place in its bar. Beat j's frame
is

    a_k + round((first_tick + j · beat_ticks) · den / num), halves up,

which is `MusicalMap::frame_at()` for that tick in entry k, since every
beat of the segment is before T_{k+1}. `click_scale()` forms the 128-bit
product one bit at a time, as a quotient and a remainder under num, so no
step overflows 64 bits: 64 iterations of adds and compares, no division
past the first. The audio thread keeps a cursor on the next beat and steps
it; after a locate, a loop wrap or a new plan it finds the next beat by a
binary search over the segment. No allocation, no lock, bounded work.

The plan builder copies the segments into the plan's single allocation,
so a tempo change reaches the click with the plan swap, at a block
boundary, like any other edit. A map that does not build gives no
segments: the click is silent rather than wrong.

This amends `midi.md` §2.5: the audio thread still never calls
`MusicalMap`, but it does evaluate the map's tick-to-frame formula for one
entry, from integers the control thread copied out of it.

### §2 The sound and where it goes

Two sounds, 30 ms each, built in the `Click` constructor: a 1500 Hz
accent at -6 dBFS for beat 1 of each bar, a 1000 Hz beat at -10 dBFS for
the others, an exponential decay tapered to zero. A new beat restarts the
one voice at its sample. The click is added to the plan's `click_out`
outputs after the mix, after the bus gain and outside the bus meter, so
pulling the monitor fader down does not silence it. In Phase 2 the
builder sets `click_out` to the monitor bus outputs; a separate click
output (a cue mix) is a Phase 3 routing decision and needs only a
different index here. Takes capture the inputs, which the click never
reaches.

The click sounds while the transport rolls, plays and takes alike, and not
while stopped. Pro Tools' "only during record" and Logic's per-mode
switches are later options.

### §3 Count-off

`transport.count_off_bars` (session format 8, default 2, 1 to 99, Pro
Tools' default and range) is the length. With count-off on, a take started
with the transport stopped (a plain take, a punch from its pre-roll start,
a loop take from its start) posts its take number to the audio thread
ahead of the take's transport store. While that take's count-off runs, the
block is treated as stopped: no roll, no capture, the playhead still. Its
length is `bars` bars of the meter at the take's start, at the tempo there,
whatever the map does later: the count-off is not on the timeline. It
starts `lead` frames into its first block so that it ends on a block
boundary, and the take's first frame then follows the last count-off beat
by exactly one beat. If the period changes during a count-off, it ends at
the end of the block it would have ended in. The count-off sounds whether
the click is on or not, as in both applications. A stop during it ends it;
nothing was captured, and the take closes empty like one stopped before
its first block.

A take started rolling never counts off. The pre-roll, when there is one,
plays after the count-off: the count-off holds the take where it starts,
and that is the pre-roll's start.

### §4 Switches and the wire

The two switches are engine state on `Session::click()`, toggled by
`transport.toggle_click` and `transport.toggle_count_off` (Pro Tools'
keypad 7 and 8, Logic's K and Shift-K, both unverified), and published as
`kTransportClick`, `kTransportCountOff` and `kTransportCountingOff` in the
state block's new `transport_flags` (layout 4). They start off. The bars
travel in `SetSettings` (protocol 15: a version-14 decoder refuses the key).

## Consequences

- One more segment per tempo entry in every plan; nothing else grows.
- The click's beats and the rulers' cannot drift apart: the same integers,
  the same rounding (engine/control's `test_click_control` compares them
  against `MusicalMap` across tempo and meter changes and hours in).
- Saving the switches with the session, a click level and a click output
  of its own, and Pro Tools' "only during record" are follow-ups.
