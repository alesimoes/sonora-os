# ADR-0148 - Aux solo read once per callback, return lines kept while they hold sound

- Status: Accepted
- Date: 2026-11-12
- Item: `ui.mixer.io` (routing follow-ups)
- Amends: ADR-0024 §4 and §5 (aux solo, later slices) and §6 (the plan, PDC)
- Relates to: ADR-0022 §6 (a send removed at a swap), ADR-0012 §7.3 (PDC)
- No contract, protocol, session format or window format change.

## Summary

*Navigation only; the sections below are the decision.*

- §1: `Session::process()` loads every aux solo flag once per callback
  into a per-bus array sized at session creation; the any-solo test and
  the "feeds a soloed aux" walk both read that copy.
- §2: `compensate()` keeps a silent aux return's delay line, aligned,
  while the pool still holds that line for the return's edge; an edge
  that never had a line still takes none. Silence passes through sums
  and faders only, never through an insert.
- §3: the rule a later UI task follows to show implicit solo on the
  strips.

## Context

ADR-0024 §4 has the audio thread derive implicit solo from the playing
plan. The code loaded each bus's `solo` atomic twice in one callback: once
to decide whether any aux was soloed, and again inside `feeds_solo()` to
mark what feeds the soloed returns. A solo cleared by the control thread
between the two loads left the callback with "an aux is soloed" and
nothing feeding it, so every unsoloed track ramped to silence for a block
and back the next: an audible dip on every track at the moment an aux was
unsoloed.

ADR-0024 §6 left PDC as `compensate()` unchanged, with one shortcut from
the #163 review: an aux return that nothing reaches plays silence, so its
edge into the Master takes no delay line. The swap that removes a bus's
last send then dropped the return's delay at once. The last frames the
send put into the bus, up to the compensation delay, were still in that
line, and were cut instead of played out. ADR-0024 §5 already noted the
same trap for aux inserts (#165 review): an insert's tail plays after
its input stops.

## Decision

### §1 One load of the aux solo flags per callback

Each callback copies every bus's `solo` flag into `aux_solo_` (one byte
per engine bus, allocated with the session). The any-solo test and
`feeds_solo()` read only the copy, so a callback sees one coherent set
of solos: a solo set or cleared mid-callback takes effect in the next
callback, whole. No allocation, lock or extra atomic traffic on the
audio thread. A test reaches the moment between the copy and its use
through a private hook (`solo_hook_`, set only by a test through
`SessionTestAccess`), as `word_hook_` does for the transport.

### §2 Return lines kept while they may hold sound

In `compensate()`:

- A node is silent only if it is an `AuxSum` or `AuxFader` all of whose
  inputs are silent (an `AuxSum` with no input is silent). An
  `InsertChain` is never silent, whatever feeds it: it can play a tail
  with nothing going in, so whatever follows it is aligned like any
  sounding path. This holds for the aux inserts ADR-0024 §5 leaves for a
  later slice, laid out as `AuxSum -> InsertChain -> AuxFader`.
- Every edge's aligned need is computed, silent or not.
- Edges out of a silent node are assigned last, after the strips' own
  paths and the sends, so they never take a line from an edge that
  carries sound. Such an edge keeps its aligned delay and its line when
  the pool still holds a line for its pair from an earlier build: that
  line may hold the frames a removed send left, which play out through it
  as they would have. When the pool holds none, the edge takes no line
  and no delay, as before.
- A kept line stays with its edge from build to build while the bus is
  silent, like any assigned line; a sounding edge that needs it in a full
  pool evicts it first, by the pool's least-recently-used rule.

`DelayLinePool::owner()` is read for the check; no header changes.

### §3 Implicit solo on the strips (for a later UI task)

The engine's derivation is the rule the UI shows. While any track or aux
is soloed, a track that is not soloed itself is shown implicitly soloed
when it feeds a soloed aux by its output or a send, directly or through
other buses; solo-safe and explicitly soloed tracks show their own state.
The UI derives it from the document's routing (outputs and sends), never
from the wire: it is display state, not saved and not undone. The
document and the playing plan can differ for the one swap after an edit;
the outline follows the document, the sound follows the plan.

## Consequences

- Unsoloing an aux no longer dips every track for a block.
- Removing a bus's last send no longer cuts the send's last frames.
- A bus that once carried sound keeps one delay line while it is silent;
  the pool assigns those last, so the mix is never misaligned for them.
- Tests: `routing` (`test_solo_cleared_mid_callback_mutes_nothing`,
  `test_removed_send_plays_out_of_the_return_line`) and `plan_pdc`
  (`test_insert_on_a_bus_into_a_bus`,
  `test_insert_on_a_bus_with_no_input_is_aligned`, on hand-laid plans
  until the builder lays out aux inserts).

## Alternatives rejected

- **Drop the silent shortcut entirely.** Every empty bus would take a
  line ahead of the sends, the pool pressure the #163 review removed.
- **Fade the return at the swap.** It still loses the frames in flight,
  and the session has no previous-plan delay to fade from.
- **A seqlock or a single packed solo word.** One load per flag into a
  per-callback copy is enough: the race was two loads of the same flag,
  not tearing across flags.
