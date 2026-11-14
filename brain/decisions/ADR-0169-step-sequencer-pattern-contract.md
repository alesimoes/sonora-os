# ADR-0169 — Step sequencer pattern contract (logic.step_sequencer)

Status: accepted.

## Context

Logic-style step sequencing programs drums and one-shots on a grid of rows
and steps, each step with its own velocity, chance, gate, pitch and nudge.
MIDI and virtual instruments are out of v1, so the sequencer cannot lean on
a MIDI track; it has to trigger the audio the session already holds, and its
playback has to be sample-exact and safe on the audio thread.

## Decision

New platform-neutral module `common/stepseq` (no Qt, no platform header, no
module dependency):

1. `Step` (`step.h`): on, velocity 0..127, probability 0..100 %, gate as a
   fraction of one step, pitch offset ±24 semitones, nudge in ticks of 960
   per beat. `Row` (`row.h`): a `SourceId` naming an existing audio region or
   its sample in the session model (0 = nothing), row gain in dB, pan, mute,
   solo. `Pattern` (`pattern.h`): fixed capacity 16 rows x 64 steps, length
   1..64, steps per beat 1..8, swing 50..75 % on every second step. Setters
   clamp; nudge is clamped to half a step, again whenever the grid changes.
2. `PatternPlayer` (`pattern_player.h`), abstract: `reset(seed)` off the audio
   thread, and `render_events(pattern, block_start_frame, frames,
   TempoSnapshot, EventBlock&) noexcept`, which fills a fixed array of 256
   `TriggerEvent`s (frame offset, gate frames, absolute step index, source,
   gain, pan, row, velocity, pitch) sorted by frame. Overflow is counted in
   `EventBlock::dropped`, never waited for. The pattern is passed in rather
   than held, so the caller acquires one snapshot per block and tests inject
   any pattern.
3. Probability is a counter-based draw, a hash of (seed, absolute step, row)
   with the seed stored in the player: the same seed replays the same
   performance, and seeking or a different block size changes nothing.
4. Editing happens on the UI thread on its own `Pattern` copy.
   `PatternExchange` (`pattern_exchange.h`) holds two preallocated slots and
   two atomic pointers (published, in use). `publish()` copies into the slot
   the audio thread does not hold, or returns false at once so the editor
   retries next tick; `acquire()` on the audio thread takes the published
   slot and records it as in use. No lock, no allocation.
5. `FakePatternPlayer` is the reference implementation used by the contract
   test and as the yardstick for the engine's player.

Build hook: `common/stretch/CMakeLists.txt` carries one guarded
`add_subdirectory(../stepseq)`; the test is labelled `stepseq` inside
`common/stepseq/CMakeLists.txt`, because the root `CMakeLists.txt` and
`tools/module-deps.allow` are owned by other items. Both move to the root
later.

## Rationale

A pattern is its own model, not MIDI: MIDI is out of v1, and per-step
probability, nudge and swing have no clean home in a note list. Triggers
name regions that already exist, so no new audio storage or format is
needed. Fixed capacity makes a snapshot a plain copy the audio thread can
read without allocation. Computing each step's frame from the absolute step
index, not by accumulating time, keeps triggers sample-exact and identical
whatever the block size.

## Consequences

- Nothing plays yet: an engine player that voices `TriggerEvent`s against
  regions, the session-model storage of patterns, and the editor UI are
  later slices of logic.step_sequencer.
- A tempo change lands on the next block boundary (tempo is constant over a
  block in `TempoSnapshot`).
- `PatternExchange::publish()` can refuse; the editor must keep its copy and
  retry, never spin.
- The module is not yet in the dependency allow-list.
- Amended 2026-11-13 (logic.step_sequencer.rest, exchange wiring): the two
  sides of the exchange are named classes in `common/stepseq`.
  `PatternPublisher` is the editor side: `submit()` after each edit, and on
  a refusal it records the edit as pending; `retry()` on the next tick offers
  the *current* editor copy, so edits made in between are not lost. Neither
  call waits. `StepSequencerModel` (ui/qt) gets an exchange constructor
  that owns a publisher and runs a 5 ms retry timer only
  while an edit is pending; the `Publisher` callback constructor stays for
  scenes and tests. `PatternFeed` is the audio side: `render_block()` calls
  `acquire()` once per block and hands that snapshot to the `PatternPlayer`.
  Two atomic operations and the player: no allocation, no lock (checked by
  `stepseq_exchange_wiring`, which also asserts the atomic pointer is always
  lock-free). Block-size independence of the probability draws is
  cross-checked by `stepseq_block_size` (block sizes 1 to 6000, swing,
  nudge, non-zero origin, through the feed). Still open: the engine voice
  that owns a feed per sequencer track and the application object that owns
  the exchange; neither exists yet.

## Alternatives rejected

- Patterns as arrangement regions: no place for per-step parameters
  (probability, nudge, gate, pitch).
- Per-step timers: not sample-exact, and timers are not allowed on the audio
  thread.
- A running PRNG stream for probability: results would depend on block size
  and seek position.
- A mutex-guarded shared pattern: locks on the audio thread.

## Unverified

- How the audio callback gets a `TempoSnapshot` (tempo, origin frame) from
  the transport and tempo map without locks: handed to the researcher.
- Whether 16 x 64 and 256 events per block cover real use.
- CPU cost of `render_events` per block at the engine's smallest buffer.

Handed to the researcher.
