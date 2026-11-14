# ADR-0202 - A rerouted strip crossfades at the swap

- Status: Accepted
- Date: 2026-11-14
- Item: `ui.mixer.io` (routing follow-ups)
- Amends: ADR-0024 §6 (a reroute takes effect with the plan that swaps in)
- Relates to: ADR-0148 §2 (return lines kept), ADR-0012 §7.3 (PDC delay
  lines), ADR-0063 (sums stay on the audio thread)
- No contract, protocol, session format or window format change.

## Summary

*Navigation only; the sections below are the decision.*

- §1: when a strip's output edge ends at a new node after a swap, the
  session crossfades: the old edge plays on as a ghost, faded out, and the
  new edge is faded in. Both fades scale the line's input, not its output.
- §2: the fade lasts 10 ms of the session's rate, fixed when the session
  is created: never a count of blocks.
- §3: the new edge starts from a fresh delay line, as any new pair does;
  the ghost keeps the old edge's line until its frames have played out.
- §4: everything is sized at session creation; process() allocates,
  locks and logs nothing for it.

## Context

ADR-0024 §6 makes a strip's output a plan edge: (strip's last node, the
destination's sum). A reroute is a swap to a plan whose edge for that strip
ends elsewhere. The audio thread had no notion of a "previous edge", so at
the swap the old route stopped and the new one started on the same frame:

- undelayed, the output stepped from the old route's level to the new
  one's in one frame, a click whenever the two levels differ;
- with PDC, the old edge's line was dropped with whatever it held (up to
  the compensation delay of the strip's last frames), and the new edge's
  fresh line played silence for its delay: a hole, then a step.

ADR-0148 kept a silent return's line so a removed send plays out; it
rejected "fade the return at the swap" because the session had nothing to
fade from. A strip's reroute is different: the strip itself is still in the
new plan, and so, almost always, is its old destination.

## Decision

### §1 The crossfade

`EdgeRamps` (engine/core, `edge_ramp.h`) records, per strip (engine track
or engine bus), the output edge of the plan last adopted: the stable ids of
its two ends, its delay and its line. At a swap, a strip whose output edge
leaves the same node (same stable id) and ends at a different one starts a
fade:

- the new edge's input is scaled by `a`, rising from `1/R` to 1 over `R`
  frames;
- a ghost edge, from the strip's node in the new plan to its old
  destination found by stable id, carries the strip's output scaled by
  `1 - a`, summed into that node after its own inputs.

The fade is applied before each edge's delay line. What is already in a
line plays out as it was, and two routes that PDC aligns to the same
output frame cross on the same frame: the sum of the two never steps by
more than one increment of the fade times the input.

The swap stays a cut (as before) when the ghost cannot be placed: the old
destination is gone, comes before the strip in the plan's order, or its
line now serves another edge of the new plan. A fade-in alone would only
add a dip to that cut. A strip moved again while it fades drops the
running fade and starts a new one from the destination it had last; the
old ghost's remaining frames are cut. Both are rare, and the second needs
two edits inside 10 ms.

A strip is identified by its last node's stable id, not its position, so a
chain edit (its last node changes id, ADR-0012) is not taken for a reroute.

### §2 Length

`R` is `sample_rate / 1000 * kEdgeRampMs` frames, `kEdgeRampMs = 10`, set
when the session is created. The plan for this item asked for a length in
blocks; a count of blocks would make the output depend on how the backend
cuts its callbacks (`routing`'s `test_block_split_invariance` reroutes a
strip and compares 256-frame and 32-frame splits), so the length is in
frames, fixed for the session as a block count would have been. The fade
advances per frame, after each piece a callback is mixed in.

### §3 Delay lines

The new edge is a new (from, to) pair, so its line starts from silence
(ADR-0012 §7.3: the session forgets a line it finds serving another pair).
Nothing new: the fade-in is written into that line and comes out its
delay later, on the same frame the ghost's fade-out does.

The ghost names the old edge's line and delay. The pool still holds that
line for the old pair (it was used in the last build and is reassigned only
when the pool is full, least recently used first), so the session's
owner check passes and the line continues. The ghost lives `R` plus its
delay frames: its input is silent after `R`, and the rest plays out the
frames the strip sent before the fade ended. An old edge the pool left
undelayed stays undelayed as a ghost.

### §4 Real-time

`EdgeRamps::allocate()` sizes every per-strip array, and the session a
`2 x max_block` buffer for the scaled input, when the session is created.
`adopt()` runs once per swap (O(strips + edges), plus a node and an edge
scan per strip that starts or keeps a ghost); `advance()` once per mixed
piece (O(strips)). Neither allocates, locks, logs or throws. Sums stay on
the audio thread (ADR-0063 §7), so the ghost is summed where its
destination is.

## Consequences

- Moving a track or a bus from one destination to another no longer
  clicks, and a delayed route no longer drops its in-flight frames or
  leaves a hole of its delay.
- For 10 ms after a reroute the strip is heard on both routes; meters on
  the old destination see it fade.
- Tests: `edge_ramp` (`test_undelayed_reroute_ramps`,
  `test_reroute_through_delay_lines_ramps`; both fail on the cut) and
  `routing` (`test_latent_insert_on_an_intermediate_bus`: track -> bus A
  -> bus B -> Master, a latent insert on A, aligned with a dry track
  through process()).

## Alternatives rejected

- **Ramp the new edge only.** The old route still stops on the swap's
  frame: the output falls to the new route's start and climbs back, a
  larger step than the cut.
- **Keep the previous plan and mix both for the fade.** Twice the graph
  for 10 ms, a second set of chain runs, and the plan exchange would have
  to hold two plans on the audio thread.
- **Fade after the line.** The ghost's in-flight frames would be faded
  away instead of played, and with PDC the two fades would sit a delay
  apart: a dip of the delay's length.
- **Fade over a count of blocks.** Output dependent on the block split;
  see §2.
