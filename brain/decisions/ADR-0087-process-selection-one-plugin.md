# ADR-0087 — Process Selection through one chosen plug-in

- Status: Accepted (recommended defaults, taken by the implementer with no
  owner available, 2026-11-08)
- Date: 2026-11-08
- Progress item: `plugins.offline` (Pro Tools: AudioSuite; Logic Pro:
  selection-based processing)
- Depends on: ADR-0004 (plug-ins out of process), ADR-0012 (hosting; any
  miss fails a render), ADR-0051 (render in place), ADR-0011 §1.4 (native
  processors stay out of the engine path)
- Amends: ADR-0051 §6, "Process Selection applies the whole chain of the
  track"

## Summary

*Navigation only; the sections below are the decision.*

- §1: `plugins.offline` is delivered as the AudioSuite half ADR-0051 §6
  left open: Process Selection through **one** insert of the track, rendered
  in real time on the live chain exactly as ADR-0051 §2. Faster-than-real-
  time rendering through hosted plug-ins stays not built.
- §2: wire: `RenderOptions::only_insert`, the word `insert=<id>` for
  `track.process_selection` only, capability `track.render.insert`. No
  protocol bump.
- §3: model: `RenderPlacement::only_insert`; Process makes only that insert
  inactive; `render_insert_valid()` refuses a foreign, inactive or bypassed
  insert.
- §4: engine: the other inserts are silenced by a render-scoped override in
  `PluginHosting`, never by an edit, and restored on every exit; a render
  whose override was never confirmed fails and keeps no file.

## Context

ADR-0051 built Freeze, Commit and Process Selection on one path: the live
session plays the range, the audio thread pushes the chain's output into a
`TrackTap`, a worker writes the file. Process Selection prints the *whole*
active chain; to apply one plug-in of several the operator bypasses the
others first, then un-bypasses them. That is the AudioSuite workflow
(select, pick one plug-in, Render) with three extra steps and two chances
to forget one. The item `plugins.offline` names AudioSuite and Logic's
selection-based processing; both are one processor on a selection.

AudioSuite renders faster than real time. Our plug-ins are third-party code
in processes of their own (ADR-0004) and the hosts run only at the audio
callback (ADR-0012); nothing has measured whether a host can be driven from
a worker at more than 1x without breaking either.

## Decision

### 1. One insert, in real time

Process Selection takes an optional insert: the selection plays through the
track's chain with every other active insert silenced for the duration of
the render, so the file is that one plug-in's output. It runs in real time
on the live hosts, through the same `TrackTap`, worker, basis check and
failure rules as ADR-0051 §§2–4. A 30 s selection takes 30 s.

The boundary: **offline (faster than real time) rendering through hosted
plug-ins is not built.** It stays the open question of ADR-0051 §6, and the
measurement that would reopen it is under Unverified.

### 2. Wire (`contracts/api/include/sonora/api/render.h`)

- `RenderOptions` gains `uint64_t only_insert` (0 = the whole active chain,
  the ADR-0051 behaviour).
- The argument word `insert=<id>`, formatted after `tail=` and before
  `name=`. `parse_render_argument(command, arg, ...)` reads it for
  `track.process_selection` (`kCommandProcessSelection`) and refuses it for
  every other command with "insert= applies only to Process Selection;
  freeze and commit print the whole chain". `insert=0` is refused: it names
  nothing.
- The command-blind `parse_render_argument(arg, ...)` refuses the word for
  every command. It is what an engine without the feature reads, so such an
  engine refuses rather than printing more of the chain than was asked.
- Capability `kCapabilityRenderInsert` = `"track.render.insert"` in
  `Hello::capabilities`.
- No protocol bump: an old server refuses the unknown word with a reason,
  and a new client checks the capability before offering the choice.
- `FakeEngine` stands in for both engines: configured with the capability
  it advertises it in Hello and reads the word as above; by default it is
  the engine as it is, without the capability, and refuses the word. The
  default stays the engine's because `engine/control`'s test requires the
  fake's default capabilities to equal the real engine's; the engine task
  adds `track.render.insert` to both in one change.

### 3. Model (`common/model/.../render_in_place.h`)

- `RenderPlacement` gains `Id only_insert` (0 = whole chain).
- `render_insert_valid(snapshot, track, insert, why)`: true only for an
  active insert of that track that is not bypassed in the mix. A bypassed
  insert would print the dry signal and call it processed; the operator is
  told to un-bypass it.
- `render_members()` with `only_insert` and `RenderKind::Process` makes
  **only** that insert inactive (its effect is in the audio now); the other
  inserts stay active, their bypass untouched. Freeze and Commit with
  `only_insert` are refused: they print the whole chain.
- Undo is the same one Compound step, labelled "Process Selection".

### 4. Engine semantics (binding on the engine task)

- The other active inserts are silenced for the render by a render-scoped
  override posted to their hosts through `PluginHosting`. The document is
  not edited: bypass is mix state (ADR-0012 §6.1), and an edit would be
  seen by every client, race the operator's own bypass clicks and, as a
  Compound, leave undo noise.
- The override is lifted on **every** exit — Done, Failed, Cancelled and
  server teardown — by the existing forced republish of the document's
  bypass (`PluginHosting::post_values(..., force = true)`), so the hosts end
  where the document says, whatever the override did.
- The render starts only once the hosts have confirmed the override (what
  the running host reads for its bypass). A render whose hosts never
  confirmed it fails and keeps no file (ADR-0012 §3, fail loudly): a file
  that might hold the whole chain is worse than none.
- The basis check of ADR-0051 §4 compares the document's bypass, which the
  override does not touch; an operator who bypasses or un-bypasses an
  insert during the render still fails it.
- The file is the chain's output minus the silenced inserts,
  latency-compensated as ADR-0051 §3. A silenced insert is bypassed, not
  removed, so it still delays by its latency (ADR-0012: a toggle never
  shifts timing) and the tap's reported latency covers it; the file lands
  where the live track played.

## Rationale

- The workflow gain is the whole point of AudioSuite and costs one word on
  the wire, one field in the model and an override the hosts already
  support in shape (bypass).
- Real time keeps the audio identical to what the operator auditioned and
  needs no second host driver, the part nobody has measured.
- Silencing by an override rather than an edit keeps the document honest:
  what is saved is what the operator set, and a crash mid-render leaves no
  stray bypass in the session file.
- Refusing a bypassed target and an unconfirmed override follows the house
  rule: fail clearly rather than produce a plausible wrong file.

## Consequences

- An engine task implements §4 and advertises `track.render.insert`; until
  it lands the engine reads the command-blind parse and refuses the word.
- A UI task offers "Process with…" listing the track's active, unbypassed
  inserts, only when the capability is present.
- Processing several times through different inserts is several renders,
  each in real time; each makes its own insert inactive, so the chain left
  on the track is what has not been printed yet.
- `CONTRACTS.lock` changes with `render.h` and `render_in_place.h` as the
  contract-hash tool requires.

## Alternatives rejected

- **Offline render through a second host driver.** Faster than real time,
  but it rests on an unmeasured assumption: that a host can be run from a
  worker thread at more than 1x without breaking ADR-0004's isolation or
  ADR-0012's callback-shaped boundary. Rendering native stock processors
  in process instead is blocked by ADR-0011 §1.4.
- **Toggling bypass in the document for the render.** Undo noise (or a
  bypass outside undo that a crash leaves behind), visible to every client,
  and it races the operator's own bypass while the render runs.
- **Render the whole chain and ask the user to bypass the others first.**
  The ADR-0051 status quo: three manual steps per process and a silent
  wrong print when one is forgotten.

## Unverified

- **Offline host measurement (for the researcher):** can a host process run
  a plug-in chain driven from a worker thread at more than 1x real time
  without breaking ADR-0004 (isolation, the host stays out of process) and
  ADR-0012 (the lock-free, deadline-bound handoff; any miss fails a
  render)? Measure on the target machine with real LV2 and VST3 hosts:
  throughput, misses, and whether the live callback's hosts are disturbed.
  A yes reopens faster-than-real-time Process; a no closes it.
- The override path on a real out-of-process host: that a silenced insert
  confirms within the pre-roll, and that the print of one insert nulls
  against the live output with the others bypassed by hand.
- That plug-ins whose bypass is not a true bypass (a "soft" bypass with
  colouration) do not leak into the print; the override uses the host's
  bypass, so it inherits that behaviour.
