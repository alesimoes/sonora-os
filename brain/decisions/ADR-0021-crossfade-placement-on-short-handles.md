# ADR-0021 — Crossfade placement, and what the gesture does on short handles

- Status: **Accepted**, 2026-10-26, by the coordinator on the recommended
  default below.
- Date: 2026-10-26
- Proposed by: rt-engineer (model squad), backlog item
  `engine.timeline.crossfades` ("Crossfades").
- Refines: ADR-0009, Rationale, "Fail loudly" ("a crossfade without handle
  material is refused rather than shortened") for the Edit-window gesture only. The model,
  the session format (10) and the control protocol (23) are unchanged.
- Numbering: follows ADR-0020.

## Summary

*Navigation only; the sections below are the decision.*

- §1: a crossfade joins two butted regions; overlapping regions do not
  exist, because placement trims (ADR-0009 §3), and what it trims is the
  handle the crossfade plays.
- §2: the selection picks the placement: ending at the cut is pre, starting
  at it is post, anything else is centred (an odd length puts the extra
  frame after the cut).
- §3: when the handles are too short for what was selected, the gesture
  shortens the crossfade to the audio there is, keeping the placement. A
  centred one with no audio on one side moves to the other side, as pre or
  post. With nothing to draw on, it is refused, naming the region and the
  side that has audio.
- §4: the model still refuses any crossfade past a handle. The shortening is
  done before the operation is built, the selection is redrawn to what was
  made, and the status line says how long it is and which handle limited it.

## Context

A crossfade needs audio past the original cut: the left region's source
after its end, the right region's before its start (`Crossfade`, model.h;
the plan builder extends both segments into those handles,
plan_builder.cpp). Regions that butt with no handle at all are common: two
imported files placed end to end, a take whose recording starts at the cut,
the last region of a file. ADR-0009 ("Fail loudly") made the model refuse a crossfade
past a handle rather than shorten it, so an operation never means something
other than what was sent, and PR #58 carried that refusal up to the gesture
unchanged: select across the cut, press F (Pro Tools, Commands Focus) or
Ctrl+Alt+X (Logic, Apply Default Crossfade), and with a short handle nothing
happens but a message.

Pro Tools does not stop there. Its Fades dialog offers "Adjust Bounds" when
there is not enough audio, which shortens the crossfade to the material
available (Pro Tools Reference Guide, *Fades*; the dialog's exact wording
is unverified here, with no copy of the guide in `brain/research/`). Logic's X-Fade drag mode and its crossfade key create what fits.
Neither makes the operator work out the handle length and select again.

## Decision

### §1 Butted regions only

A playlist's regions never overlap (ADR-0009 §3). Dropping a region over
another trims it, and the trimmed audio stays in the source as the handle a
crossfade at the new cut plays. So "overlapping or abutting" reduces to
abutting, and a crossfade is always named by its two regions.

### §2 Placement from the selection

Unchanged from PR #58: a range that ends at the cut is a pre crossfade, one
that starts at it post, any other range across it centred, of the range's
length. The model has only these three alignments; a lopsided range becomes
a centred crossfade of its length, and the selection is redrawn to show it.

### §3 Short handles: shorten to the audio, keep the placement

`model::fit_crossfade(x, before, after)` takes the handles
(`model::crossfade_handles`: the right region's source before its start,
the left region's after its end, the shortest channel counting) and:

- pre: shortens to the handle before the cut; refused if there is none;
- post: shortens to the handle after the cut; refused if there is none;
- centred: keeps the longest centred length both handles allow
  (`2 × after` if after ≤ before, else `2 × before + 1`); if one handle is
  empty, moves to the other side as pre or post, shortened to that handle;
  refused only when both are empty.

The region-room checks (a crossfade may not reach into the fade or
crossfade at a region's other end) run on the fitted crossfade, as before.

The default shape stays equal power, for uncorrelated material; the X-FADE
shape button gives linear (equal gain) for correlated material, a region
crossfaded against its own continuation, which an equal-power crossfade
lifts by 3 dB at the middle (test_crossfades, test_fade_curve).

### §4 The model is unchanged, and the operator is told

The fitting happens in the gesture, before the `SetCrossfade` is built, so
the model still refuses any crossfade past a handle and an operation still
means exactly what it says (ADR-0009's rule holds for every other path:
replication, a loaded file, a take's punch crossfades). The selection
becomes the crossfade made, so Clear takes it away and the length is on
screen; the status line says "Crossfade shortened to N frames[, before/after
the cut]: <region> has M frames of handle …". A refusal names the region
with no handle and, when the other side has audio, the selection that
would work.

## Consequences

- A crossfade on butted files with a handle on one side now works with one
  key press, where it was refused.
- A crossfade can come out much shorter than selected (a few frames, if the
  handle is that short). The redrawn selection and the status line say so;
  nothing is silent.
- `fit_crossfade` and `crossfade_handles` are public model functions, so a
  future Fades dialog or batch fades use the same rule.

## Alternatives rejected

- **Keep refusing** (PR #58 as it was). Correct but unlike either DAW, and
  the operator has to find the handle length by trial.
- **Shorten in the model** (`apply`). Would make an operation mean something
  other than what was sent, on every path, against ADR-0009's "Fail
  loudly".
- **Choose whichever placement gives the longest crossfade.** Moves the
  crossfade away from where the operator put it even when the asked-for
  placement has audio; only the side with no audio at all moves.
- **Extend past the media with silence.** A crossfade into silence is a
  fade, which the operator can make on purpose.

## Open questions

- A Fades dialog (Pro Tools Cmd+F) with an explicit Adjust Bounds choice
  would let the operator see the limit before committing; not built.
- The status line clears after 2.5 s. The shortened crossfade stays visible
  as the selection, so a held notice was not added; revisit if operators
  miss it.
