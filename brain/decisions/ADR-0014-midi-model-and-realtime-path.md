# ADR-0014 — MIDI: ticks in the model, frames on the audio thread, MIDI ports in the backend, takes as SMF files

- Status: **Accepted by the owner, 2026-10-19**, with every recommended
  default for O1–O8 ("For the owner"). Not implemented. Accepting it is the
  ADR-0007 §6 amendment for the `contracts/backend` and `contracts/plugin`
  changes (O8).
- Date: 2026-10-19. Revised the same day with the rt-engineer's and
  plugin-engineer's reviews of PR #93 (both "accept with changes"). Every
  required change is folded in; the PR thread maps each review point to
  where it landed.
- Decided by: architect. BACKLOG Track 2 item 4, ROADMAP DAW 6 ("a MIDI
  spec and ADR before the model").
- Spec: [`midi.md`](../specs/midi.md). This ADR is the decision; the spec
  holds the formulas, keys, layouts and capacities.
- Numbering: follows ADR-0013.
- **Version numbers** (session format 4, protocol 7, plugin layout 2, clock
  layout 2, a state layout bump) mean the next free number when each PR
  lands. ADR-0013 is also Proposed and also names numbers; neither may
  claim one before it lands.
- Amends: ADR-0007 §1 (four edges) and §6 (this ADR is the
  amendment for the `contracts/backend` ownership change, O8), ADR-0012 §2
  and §7.1 (instrument chains), `session-format.md` (version 4, tempo
  bounds). Each amendment is named where it is made; none is made quietly.

## Context

DAW 6's exit criterion: *a MIDI performance recorded against the click
plays back through a hosted instrument with every note within one sample
of its recorded position in the rendered output, and survives an SMF
export and re-import unchanged.*

What is already on `main`:

- `common/midi` (PR #23): MIDI 1.0 events in ticks, note pairing, edit
  primitives, an SMF reader and writer with bounds and no throws, a live
  byte-stream parser, a tick-anchored `TempoMap` in µs per quarter.
  Neutral, never on the audio thread.
- `sonora::MidiBuffer` in `common/base`: one block of raw messages at frame
  offsets, fixed capacity, overflow counted. Its `push` is a sorted insert,
  O(1) only in offset order; its `dropped()` is not atomic.
- `common/model` (session format v3): sample-anchored audio, playlists
  that never overlap, an append-only pool, operations and compounds
  (ADR-0009), inserts (ADR-0012). The tempo map is **frame-anchored with a
  float bpm**, checked only for `bpm > 0`, with any meter denominator.
  v3's spec says musical-time anchoring "is a separate field added with a
  version bump, not a reinterpretation of this one".
- Bars|beats are read in `ui/qt` (`time_format.h`) in `double`.
- Plugin chains (ADR-0012): one host per chain, depth 1, the audio thread
  never waits, a skipped submit when the host is busy; audio only, no
  transport.
- The UMC1820 has 5-pin MIDI in and out; its figures wait for OS 1.

What has no answer yet: where MIDI lives in the document, which clock it is
counted in, how it reaches the audio thread and a plugin, how a MIDI take
survives a crash, and how SMF fits a sample-accurate model.

The constraints that settle most of it: the audio thread never allocates,
locks or blocks; a lost take is the worst outcome; the image boots the same
way on hardware we have never seen, where MIDI devices come and go.

## Decision

### 1. MIDI regions live in `common/model`, in ticks, with their events inline

- A track has a **kind**: audio, MIDI (to an external port) or instrument
  (to a hosted instrument). Pro Tools has MIDI and Instrument tracks, Logic
  external MIDI and software instrument tracks; three kinds cover both.
- MIDI and instrument tracks hold **MIDI playlists** of **MIDI regions**.
  ADR-0009 §3's playlist rules hold in ticks: sorted, **never
  overlapping**, overwrite on placement, no crossfades.
- A region is a **window onto an immutable, shared `MidiSequence`** (notes
  as start and length, other channel messages, SysEx) in data ticks, with
  `position`, `length` and `offset` like an audio region's
  `source_offset`. Trims are non-destructive. Split, duplicate and paste
  share the sequence; editing through one region copies it for that region
  alone.
- **Events are stored in the session file, not in the pool.** Notes are
  edited constantly; audio never is. A pool entry per note edit would grow
  without bound outside undo. Inline, every MIDI edit is an ordinary
  snapshot step.
- Operations (spec §3.1): the region operations by id (`MoveRegion`,
  `TrimRegion`, `SplitRegion`, `RemoveRegion`) apply to MIDI regions too,
  and **each that carries a position gains a timebase tag that `apply`
  checks against the playlist**, so a sender that sent frames to a tick
  playlist is refused instead of mis-scaling by 25 times. New:
  `PlaceMidiRegion`, `EditMidi`, `AddMidiSource`, `SetMidiRouting`,
  `SetInstrument`, `RemoveInstrument`. Protocol and undo-window versions
  move with them.
- **Amends ADR-0007 §1, four edges, all to `common/midi`** (layer 1,
  neutral, links only `common/base`; no cycle): from `common/model` (notes
  and edits), `interchange` (SMF codec), `engine/core` (the disk thread's
  streaming take writer, which produces bytes and does no I/O) and
  `engine/control` (reading a closed take back). Additive under §6:
  rt-engineer and ui-engineer review.

### 2. The timebase: 960,000 ticks per quarter, against the frame-anchored tempo map

- Every MIDI position in the model and the file is an **integer tick at
  960,000 per quarter note**, Pro Tools' published internal resolution.
  The reason is arithmetic: a tick shorter than a frame is what lets a
  recorded event return to its frame. At 960,000 that holds above 3 bpm at
  48 kHz and 12 bpm at 192 kHz. Logic's 960 per quarter would move a
  recorded note by up to 12 frames at 120 bpm.
- **The tempo map keeps its v3 meaning**: entries at frames, float bpm. One
  canonical conversion, frames ↔ ticks ↔ bars|beats (`MusicalMap` in
  `common/model`), is defined exactly in spec §2.2: each entry's tick
  derived by rounding once from the entry before, each conversion counting
  from its own entry, rounding by floor division so negative arguments are
  defined, **128-bit integer arithmetic on the float's exact value**, so
  GCC and MSVC give the same tick with or without FMA.
- **Bounds that make the arithmetic true** (spec §2.4): bpm in [1, 1000],
  meter denominators powers of two up to 64, rates up to 384 kHz. With
  them every numerator is under 2^107 and every denominator under 2^53.
  Every 128-to-64-bit result is checked and an out-of-range one refused by
  name, never wrapped. New edits outside the bounds are refused; a v3 file
  is migrated into them with each change reported (O7).
- **Exact by construction, not only by the identity.** `frame(tick(f)) ==
  f` holds when ticks are finer than frames. The resolver and the builder
  keep it meaningful through tempo changes by the two rules of spec §3.1
  and §5.5: a take's data ticks are `tick(f_event) − position`, and a
  note's off is `frame(position + start + length − offset)`, never an on
  frame plus a duration.
- **Cost is the control thread's, and is paid for in M-1.** The audio
  thread never calls `MusicalMap`. A portable 128/64 division costs 30 to
  150 ns, so 500,000 events would cost 30 to 150 ms. M-1 ships a batch API
  for sorted input, a 64-bit fast path behind an overflow check, and a
  benchmark on GCC and MSVC; M-3 designs a per-region flatten cache into
  the plan builder from the start.
- The bars|beats reading in `ui/qt` moves onto `MusicalMap` in M-1, so the
  grid, the counters and a note at 5|1|000 are one function.
- **Audio stays in frames; MIDI regions move in frames when the tempo
  changes**, as tick-based tracks do in both vendors.
- **DAW 7's choice is kept open, not made.** Tempo entries anchored in
  ticks (a change at bar 17 stays at bar 17) are DAW 7's ADR. If it takes
  that route, its migration fills each entry's tick from the derived `T_k`.

### 3. Session format version 4

Spec §4: a `MIDI Files/` folder; `midi_sources[]`; every track gains
`kind`, with the key set strict per kind; MIDI regions with
`position_ticks`, `length_ticks`, `offset_ticks`, `notes` as one-line
arrays, `events`, `sysex` up to 4 KiB; positions and note starts never
negative. MIDI ports are named by **port name**, not index, with `"all"`
the default input, because a MIDI port is a device that is plugged and
unplugged. Migration v3 → v4 fills `"kind": "audio"` and
`"midi_sources": []` and brings the tempo map inside §2's bounds with a
report. **Amends `session-format.md`**: "every position is an integer count
of samples" gains "or, for MIDI, of ticks at 960,000 per quarter, in keys
ending `_ticks`", and the tempo map gains its bounds.

### 4. MIDI reaches the audio thread from the backend, per block, at frame offsets

- **`contracts/backend` gains MIDI ports**: `AudioBlock` carries
  `midi_inputs` (driver-written, callback-read) and `midi_outputs`
  (callback-written, driver-drained and cleared after `process()`), fixed
  arrays of `MidiBuffer` slots allocated at `open()`. `nullptr`/0 means no
  MIDI, and the new virtuals have default bodies. Ownership and thread are
  stated per field in the header. That is a change in memory ownership,
  which ADR-0007 §6 classes as breaking: **this ADR is the §6 amendment
  for it** (O8).
- **Slots are fixed ports; a replug changes connections only.** The JACK
  driver registers every slot's port at `open()` and follows devices with
  connect and disconnect from its non-RT thread. A slot a departed device
  held is reassigned only after the engine releases it, once a plan and
  routing that no longer name it are published and the previous plan
  retired, as ADR-0012's regions are never reused early.
- The backend reports `midi_input_latency_frames()` and
  `midi_output_latency_frames()`. A Windows driver (ASIO carries no MIDI)
  pushes from its MIDI thread to a driver-internal SPSC ring and fills the
  slots on the audio thread at callback start; nothing but the audio
  thread writes a `MidiBuffer`. `drivers/fake` scripts MIDI at exact frames.
- **On the audio thread** (spec §5.2), per track, five sorted sources
  (pending and generated endings, chase, plan playback, thru, on-screen
  keyboard) are **merged by offset**, so every push is an O(1) append.
  Capacity per block is **4,096 messages with 2,064 reserved for endings**
  (16 × 128 note-offs + 16 × CC 64), the one rule taken for both reviews'
  capacity point: **a generated note-off is never dropped**; one that does
  not fit is carried to the next block. Starts and controllers past the
  rest are dropped and counted. Chase that does not fit is carried, never
  lost.
- **Capture is not monitor-gated**: the routed, filtered input goes to
  capture before the monitor rule decides thru.
- MIDI tracks write their port through a fixed delay queue of
  `max(0, L_plan + L_out − L_midi_out)` frames, so an external synth sounds
  with the delayed audio.
- **The plan carries MIDI as frames**, offs then non-note messages then ons
  at one frame, SysEx lengths in the plan's byte pool. No residency gate:
  it is in memory.
- **No hanging notes.** Trackers sit after the delay queue (a note sounds
  once it was written, or delivered to a plugin). Playback and thru are
  tracked apart, so stop and locate end playback's notes and not the key
  being held; mute and routing changes end both. A swap that changes a
  track's MIDI, including a change of `L_plan`, ends its playback notes.
  Trackers, delay queues, pending endings and chase slots belong to the
  session, sized at construction, never to the plan.
- **Chase** is computed on the control thread into one of two slots per
  track, freed only when the epoch that named it has retired; if both are
  busy the locate carries none, counted, and the control thread never
  waits.
- **Every loss is named and counted** in engine atomics published through
  a state layout bump and `EngineEvent`s (spec §5.6): MIDI lost (capture),
  dropped to a plugin, dropped to a port (including the driver's ENOBUFS),
  chase skipped or late, plan over maxima.

### 5. Instruments are plugin chains with an event input (amends ADR-0012)

- The instrument is **its own slot** ahead of inserts A–J (O6); afterwards
  it is an `Insert` by id. **Only it receives events**; a plugin with an
  event input and no audio input is refused in A–J.
- **Amends ADR-0012 §2:** an instrument track's chain is one host, the
  instrument and then the inserts (at most 11 instances). **Amends §7.1:**
  the chain's inputs equal the track's channels, or 0 for an instrument
  chain.
- **Events are never lost on a skipped submit.** A block not submitted
  because the host was busy leaves its events in an engine-private carry
  buffer, delivered first in the next submit at offset 0. Overflow drops
  note-ons first; if an ending still cannot be kept, the next submit
  starts with All Notes Off and CC 64 = 0 on the channels the chain used.
- **`contracts/plugin` layout 2**: separate input and output channel
  counts; an event area whose offset and capacity are in the header and
  checked by `header_valid`, written before `submitted_block` is
  published; no host-written counter the engine reads. The host copies the
  area once, validates every count, offset and byte range, and exits
  faulted on a violation. Delivery per format (LV2 atom sequence; VST3
  `IEventList` for notes and poly pressure, `IMidiMapping` for controllers,
  bend, channel pressure and program change, straight to the processor and
  never through the parameter ring; fresh note IDs) is spec §6.4.
- **Transport** reaches plugins for the first time: tempo, meter, playing
  flag, position in frames and quarters, written per block by the audio
  thread into the clock region (next clock layout) from a tempo table the
  builder computes. LV2 `time:Position`, VST3 `ProcessContext`.
- Only the main output bus, 1 or 2 channels, is used; auxiliary outputs
  are off in v1. After a host restart, replacement or reactivation the
  instrument's trackers are cleared and the chase re-sent; bypass sends
  endings first. The catalog gains the fields validation and the picker
  need, and its version moves.
- Misses, fades and PDC are ADR-0012's. Live playing through an instrument
  on an armed track is heard at the **plan's total compensation**, stated
  rather than hidden; hearing less is DAW 4's low-latency monitoring item.

### 6. Recording: stamped against the audio clock, written as an SMF whose tick is a frame

- An input event is placed at the frame that was **audible to the player**
  when it reached the engine: block frame + offset − (playback latency +
  plan compensation + MIDI input lag) + a session offset (spec §7.1),
  recorded per take as `latency_compensation`. A position before the
  timeline start is placed at 0 and counted; the file keeps the true frame.
- Captured events go to the disk thread on an SPSC ring with reserved room
  for boundary records and closing endings, SysEx on both rings or neither,
  sized from a stated worst case. **A full ring is an overrun: counted and
  reported as MIDI lost.**
- **Each MIDI take is written to disk while it is recorded**, never kept
  only in memory until a save. The file is a format 0 **SMF whose division
  and tempo make one tick one frame**, without running status, opened by a
  header text event with a defined grammar (take, track, rate, origin,
  start frame, time reference, compensation, percent-encoded session name)
  that the disk thread writes when the take opens. Empty text events
  bridge silences longer than a delta time can hold. Checkpoints write,
  then `fdatasync`, then patch the `MTrk` length to an event boundary, so a
  durable length never points past durable bytes. Recovery cuts the file
  back to its last complete event, ends open notes and appends the end of
  track before anything reads it: the one write to an unfinished file the
  format allows, as for audio.
- At close the control thread reads the file back, converts to ticks and
  applies the take compound with the audio tracks' members ("Record", one
  undo step). Undo removes the region, never the file or the pool entry.
  The engine reading its own take files is not an interchange parse.

### 7. SMF import and export in `interchange/`, in `sonora-convert`

As ADR-0007 §1 and §3 place them: an SMF from outside is untrusted and
parsed in its own process with `common/midi`'s bounded codec. Import
behaviour is listed in spec §8 (running status, velocity-0 offs, FIFO
pairing, unterminated notes, format 2 one pattern per region, joined SysEx
packets, RMID unwrapped, exact tick scaling where the division divides
960,000) and reported. Export writes format 1 with `0x8n` note-offs that
keep release velocity, at 960 per quarter by default (O2).

### 8. MIDI 1.0 now; MIDI 2.0, UMP and MPE not foreclosed

The model and the RT buffers hold MIDI 1.0. Higher resolution later is a
format version whose migration upscales by the published MIDI 2.0 rule;
UMP on the wire is a driver concern. MPE needs nothing new for external
ports and LV2; for VST3 it needs a later translation to note expression,
which the note IDs keep possible.

### 9. Phases

Each is its own PR or series, in this order; each leaves the product
runnable.

| Phase | Scope | Gate |
|---|---|---|
| M-0 | this ADR and `midi.md` | owner accepts, answers O1–O8 |
| M-1 | `common/model`: `MusicalMap` with batch API and fast path, bounds (§2), `ui/qt` onto it, track kinds, MIDI playlists and regions, timebase tags, `midi_sources`, operations and codec, protocol, window format, format v4 with fixtures. The engine plays nothing on MIDI tracks yet and says so | rt-engineer and ui-engineer (ADR-0007 §6); `MusicalMap` benchmark on GCC and MSVC; Unverified 6 |
| M-2 | `contracts/backend` MIDI ports, `drivers/fake` MIDI, `drivers/jack` JACK MIDI (fixed ports, slot release), conformance | rt-engineer, driver-engineer, **rt-safety-auditor** (the driver's RT copy-in and drain); researcher's Unverified 1–3 on the laptop |
| M-3 | engine playback to external ports: plan MIDI with the flatten cache, merge by offset, trackers, endings, chase slots, delay queue, port and plugin loss counters (state layout bump) | rt-safety-auditor, including that `MusicalMap` is unreachable from `process()`; regression tests listed first: stop, locate, loop wrap, plan swap, mute and routing change with held notes and held sustain; the 2,064-ending worst case; delay-queue flush; Unverified 7 |
| M-4 | recording: capture ring with reservations, SMF take writer, header, checkpoints, recovery, capture counter; replace, then merge; cycle with DAW 3's take lanes | a MIDI take survives `kill -9`; a silent take longer than `max_vlq`; a full ring at window close; recovery of a torn tail |
| M-5 | instruments: `contracts/plugin` layout 2, carry buffer, LV2 then VST3 delivery, transport (clock layout), catalog fields, instrument tracks, PDC; **exit criterion part 1** as a ctest on fake | rt-engineer, plugin-engineer, **rt-safety-auditor** (event-area writes before the release); a host stalled 3 blocks over a note-off leaves no hanging note; Unverified 4 |
| M-6 | SMF import and export in `interchange/` through `sonora-convert`; **exit criterion part 2** as O2 states it | |
| M-7 | the Qt UI: MIDI and instrument tracks in Edit and Mix, piano roll, event list, MIDI operations, step input, on-screen keyboard, each with its own design note and screenshots | ui-engineer |

Later, each with its own ADR or amendment: MIDI tracks driving instrument
tracks, sample-based MIDI tracks, plugin MIDI output and MIDI effects,
instrument auxiliary outputs, VST3 note expression and MPE, MIDI learn
(with DAW 10), score and notation, MIDI 2.0.

## Rationale

- **A lost take decides recording.** A MIDI take kept in memory until a
  save would be lost by a crash, a power cut or an engine restart, exactly
  the cases audio is protected against. Written as it is played,
  checkpointed in an order that never claims bytes the disk does not hold,
  and recoverable from its own header, it has the same loss window as
  audio. An SMF with tick = frame, rather than a private journal, opens in
  any DAW at the right time when nothing else survives.
- **A lost note-off is a hanging note,** the MIDI equivalent of a click
  that never stops. So endings have reserved capacity on every path
  (track buffer, carry buffer, event area, capture ring) and are carried
  rather than dropped; only starts and controllers are ever dropped.
- **One clock.** MIDI is stamped by the backend at a frame offset in the
  same callback as the audio. A separate reader thread stamping
  `CLOCK_MONOTONIC` would need a second clock correlated on the audio
  thread, with its error landing on note timing.
- **Fail loudly, never block.** Every buffer is fixed at construction;
  every overflow is a counted drop with a name the operator sees; a plan
  over its maxima is refused at build; a missing port is shown missing; an
  out-of-range conversion is refused, never wrapped.
- **Ticks for the model, frames for the audio thread.** Ticks are what
  editing needs; frames are what the callback can use without arithmetic
  it should not do. The builder converts once, off the audio thread, with
  an exact function every reader shares.
- **Hardware we have never seen.** Port names rather than indices, `"all"`
  as the default input, fixed slots whose connections follow devices: a
  session opens and plays on a machine whose MIDI devices are different,
  and says what is missing.
- **What it costs later is stated.** DAW 7 may write a tempo migration; the
  derived `T_k` makes it mechanical. Inline events bound the session by the
  parser caps; the measurement and the remedy are named.

## Consequences

- Three contracts change, each in its own PR with its owners' reviews and a
  `CONTRACTS.lock` regeneration: `common/model` (format 4, operations,
  protocol), `contracts/backend` (MIDI ports; breaking, amended here),
  `contracts/plugin` (layout 2 and clock layout 2). The state block gains
  MIDI loss counters (a state layout bump).
- `ui/qt`'s bars|beats code moves into `common/model`; its tests are the
  check that nothing the operator sees moved.
- v3 sessions with a tempo outside [1, 1000] bpm or a meter denominator
  that is not a power of two up to 64 open changed and say so.
- The plan grows by 16 bytes per MIDI event; the flatten cache keeps a note
  edit from re-flattening the session (Unverified 7).
- The disk thread writes a second kind of take file. The `fdatasync`
  schedule and the recovery scan cover `MIDI Files/` too.
- Per track and per chain, about 4,096 × 12 bytes of buffer plus 32 KiB of
  bytes, several times over (track buffer, carry buffer, event area, delay
  queue): roughly 200 KiB per MIDI or instrument track, preallocated and
  locked.
- An instrument chain is one host process per instrument track (ADR-0012
  Unverified 3 applies).
- Interface notes have one block of jitter; device MIDI has the backend's.
  Events carried past a busy host arrive one block late, at offset 0.
- `common/midi`'s `StreamParser` comment describes the rejected reader-
  thread design for JACK; it stays right for a driver that reads raw bytes,
  and is corrected in M-2.

## Unverified

Each names the measurement that settles it and who runs it.

1. **PipeWire's MIDI timing to a JACK client.** Whether an offset reflects
   arrival time or the cycle start, whether input is one period late (the
   assumed `L_midi_in`), the output side's `L_midi_out`, and jitter.
   Laptop proxy: `snd-virmidi`, notes sent at known `CLOCK_MONOTONIC`
   times, offsets read in a JACK client at quantum 128 and 1024, error in
   frames. Real figure in OS 1: UMC1820 MIDI out cabled to MIDI in.
   Researcher, before M-2 merges.
2. **Port names across replug.** Which name PipeWire gives a USB MIDI
   device's port, and whether it survives unplug, replug, a second
   identical device and a reboot. Researcher, with 1.
3. **UMP devices.** Whether PipeWire presents MIDI 2.0 devices to JACK
   clients as MIDI 1.0. Researcher, before M-2.
4. **Instrument delivery** across free-licensed LV2 and VST3 instruments
   and at least one yabridge-bridged VST3 instrument: notes at their
   offsets, controllers through `IMidiMapping`, what plugins without it do,
   transport read correctly by a tempo-synced instrument, and the bridged
   one's miss count at quantum 128. plugin-engineer, in M-5.
5. **Vendor behaviour** (researcher, with the PT-RG pinning in BACKLOG):
   whether Pro Tools and Logic play a note past its region's end, their
   default record mode over existing MIDI, a MIDI split's effect on a
   crossing note, their chase defaults. O1 and O4 rest on it.
6. **Session size with dense MIDI.** 18 instrument tracks of a 5-minute
   piece with continuous controllers: file bytes, DOM values against the
   4,000,000 cap, parse time, and undo-window bytes after 200 note drags in
   a region with 50,000 controller events (copy-on-write copies the whole
   sequence). rt-engineer, in M-1. Over the caps means sequences beside the
   file (a version bump), decided then.
7. **Plan build with MIDI.** 500,000 events on 18 tracks, with and without
   the flatten cache, against the 10 ms control-thread budget.
   rt-engineer, in M-3.
8. **Windows MIDI stamping under ASIO**, only when a Windows ADR opens
   (ADR-0008 §6).

Capacities (4,096 messages, 32 KiB, 2,064 reserved, 4 KiB SysEx, 10,000
messages a second per USB port for the capture ring) are placeholders
until a dense controller stream, a stop with every key held and a bulk
dump are measured in M-3 and M-4.

## For the owner

**Ruled by the owner, 2026-10-19: every recommended default below is
accepted (O1–O8).**

- **O1. Default record mode over existing MIDI.** *Recommended: replace*
  (the take overwrites the range, as audio does), with a Merge toggle in
  the Transport window. Vendor defaults are Unverified 5.
- **O2. The SMF half of the exit criterion.** An SMF division is at most
  32,767 per quarter, so an unquantized performance at frame accuracy
  cannot pass through one unchanged. *Recommended:* restate it as "export
  → import → export is byte-identical; notes on the export grid return
  exactly; every other note returns within half an export tick, and the
  report counts them", with export at 960 per quarter by default, up to
  32,767 by choice.
- **O3. Default MIDI input of a new track.** *Recommended: all inputs,
  merged* (Pro Tools' "All"). The alternative is none until chosen.
- **O4. A note that runs past its region's end.** *Recommended: it ends at
  the region's end*, which keeps one region sounding per track and bounds
  hanging notes. The alternative plays it to full length.
- **O5. MIDI routing and undo.** *Recommended: not undoable*, as a track's
  audio input is not.
- **O6. Where the instrument sits.** *Recommended: its own slot ahead of
  A–J* (Logic's layout), drawn as the top insert row so Pro Tools users
  find it where they expect; instruments are refused in A–J. The
  alternative is Pro Tools' instrument in any insert slot.
- **O7. Tempo map bounds** (new, from the rt-engineer's review).
  *Recommended:* bpm 1 to 1000, meter denominators powers of two up to 64,
  rates up to 384 kHz; edits outside are refused; a v3 session outside is
  opened with the tempo clamped and the denominator rounded to the nearest
  power of two, each change reported, the original kept in the
  pre-migration backup. The alternative refuses to open such a session,
  which breaks "backward compatibility: always" for values nobody plays.
- **O8. This ADR as the ADR-0007 §6 amendment** (new, from the
  rt-engineer's review). The backend's MIDI slots change memory ownership
  (driver-written inputs, callback-written outputs the driver drains),
  which §6 classes as breaking and which needs an amendment before the
  code. *Recommended: accepting ADR-0014 is that amendment*, for
  `contracts/backend` and for `contracts/plugin` layout 2, so M-2 and M-5
  need no separate ADR. The alternative is a short ADR-0007 amendment of
  its own before M-2.

## Alternatives rejected

- **MIDI sequences in the append-only pool, regions as windows onto them**
  (the audio shape). Every note edit would add a pool entry undo cannot
  remove; the pool keeps the takes instead (§6).
- **Sequences beside the session file, content-addressed** (the
  plugin-state shape). Smaller file and snapshots, but every edit becomes
  a file at save and a replica must read the session folder. The named
  remedy if Unverified 6 fails.
- **960 ticks per quarter** (Logic's). Recording would quantize to 25
  frames at 120 bpm and fail the exit criterion.
- **MIDI positions in frames only.** Quantize, grid edits and tempo changes
  need musical time; sample-based MIDI tracks stay possible later.
- **Anchoring the tempo map in ticks now.** It reinterprets a v3 field v3
  promised to keep and settles DAW 7's tempo editing before that ADR
  exists.
- **Float or double tick conversion.** Two compilers, or FMA contraction,
  can round a note to different frames; engine and UI would disagree.
- **Leaving bpm and meters unbounded.** A binary32 bpm can carry an
  exponent of +104, and then no fixed-width integer holds the product; a
  denominator of 3 makes a bar a fraction of a tick. The bounds cost
  nothing anyone plays.
- **A MIDI reader thread outside the backend.** A second clock to correlate
  on the audio thread, its error on note timing, and a Linux API in a path
  ADR-0008 keeps behind `drivers/*`. A driver may still do it internally
  through its own SPSC ring, as a Windows one would.
- **Registering and unregistering JACK ports per device.** Simpler
  bookkeeping, but the callback would see port objects appear and vanish,
  and a reassigned slot would play another device for a block.
- **Holding a MIDI take in memory until it closes or the session saves.**
  A crash during a long take loses all of it.
- **A private binary journal for MIDI takes.** An SMF with tick = frame is
  as easy to append to and every DAW reads it.
- **Patching the `MTrk` length before the sync.** Simpler, but a power cut
  between the two leaves a length that points past durable bytes.
- **MIDI ports by index**, as audio channels are. An index would point at
  another device after a replug, without a word.
- **Separate `MoveMidiRegion`, `TrimMidiRegion`, … operations.** Units in
  the type, but a doubled vocabulary and codec, and two cases in every
  compound sender. The timebase tag gives the safety with one vocabulary.
- **Dropping events when a host is busy, as audio is.** A dropped note-off
  hangs inside the plugin, where the engine's tracker cannot see it.
- **Dropping overflow in arrival order.** A dense controller stream would
  fill the buffer and the generated note-offs would be what is lost.
- **Pre-reading playback MIDI by the instrument's latency** instead of
  delaying the other paths. One node would need two latencies; ADR-0012's
  single PDC mechanism already aligns it.
- **A `RecordMidiTake` operation.** Replace is `PlaceMidiRegion` exactly
  and merge composes `EditMidi` and `TrimRegion`.
