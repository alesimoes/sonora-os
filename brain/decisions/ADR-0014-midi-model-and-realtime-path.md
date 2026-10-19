# ADR-0014 — MIDI: ticks in the model, frames on the audio thread, MIDI ports in the backend, takes as SMF files

- Status: **Proposed**, 2026-10-19. Not implemented. The owner approves
  each ADR; six questions for the owner are under "For the owner", each
  with the default this ADR builds on until the owner rules.
- Date: 2026-10-19
- Decided by: architect. BACKLOG Track 2 item 4, ROADMAP DAW 6 ("a MIDI
  spec and ADR before the model").
- Spec: [`midi.md`](../specs/midi.md). This ADR is the decision; the spec
  holds the formulas, keys and layouts.
- Numbering: follows ADR-0013.
- Amends, if accepted: ADR-0007 §1 (two edges), ADR-0012 §2 and §7.1
  (instrument chains), `session-format.md` (version 4). Each amendment is
  named where it is made below; none is made quietly.

## Context

DAW 6's exit criterion: *a MIDI performance recorded against the click
plays back through a hosted instrument with every note within one sample
of its recorded position in the rendered output, and survives an SMF
export and re-import unchanged.*

What is already on `main`:

- `common/midi` (PR #23, ADR-0007's `common/midi` update): MIDI 1.0
  events in ticks, note pairing, edit primitives (quantize, transpose,
  velocity, legato, split, merge), an SMF reader and writer with bounds and
  no throws, a live byte-stream parser, and a tick-anchored `TempoMap` in
  µs per quarter. Neutral, never on the audio thread.
- `sonora::MidiBuffer` in `common/base`: one block of raw messages at frame
  offsets, fixed capacity, a push that does not fit is dropped and counted.
- `common/model` (session format v3): sample-anchored audio, playlists
  that never overlap, an append-only source pool, operations and compounds
  (ADR-0009), inserts (ADR-0012). The tempo map is **frame-anchored with a
  float bpm**; v3's spec says musical-time anchoring "is a separate field
  added with a version bump, not a reinterpretation of this one".
- Bars|beats are read in `ui/qt` (`time_format.h`) in `double`.
- Plugin chains (ADR-0012): one host per chain, depth 1, the audio thread
  never waits; `InsertChain` nodes; PDC on edges. Audio in and out only.
- The UMC1820 has 5-pin MIDI in and out; its figures wait for OS 1.

What has no answer yet: where MIDI lives in the document, which clock it
is counted in, how it reaches the audio thread and a plugin, how a MIDI
take survives a crash, and how SMF fits a sample-accurate model.

The constraints that settle most of it: the audio thread never allocates,
locks or blocks; a lost take is the worst outcome; the image boots the same
way on hardware we have never seen, where MIDI devices come and go.

## Decision

### 1. MIDI regions live in `common/model`, in ticks, with their events inline

- A track has a **kind**: audio, MIDI (to an external port) or instrument
  (to a hosted instrument plugin). Pro Tools has MIDI and Instrument
  tracks, Logic external MIDI and software instrument tracks; the three
  kinds cover both.
- MIDI and instrument tracks hold **MIDI playlists** of **MIDI regions**.
  The playlist rules of ADR-0009 §3 hold in ticks: sorted, **never
  overlapping**, overwrite on placement. No crossfades.
- A region is a **window onto an immutable, shared `MidiSequence`** (notes
  as start and length, other channel messages, SysEx), in data ticks:
  `position`, `length`, `offset`, like an audio region's
  `source_offset`. Trims are non-destructive. Split, duplicate and paste
  share the sequence; editing notes through one region copies it for that
  region alone (independent copies; Logic-style aliases would be explicit
  and later).
- **Events are stored in the session file, not in the pool.** Notes are
  edited constantly; audio never is. An append-only pool entry per note
  edit would grow without bound and sit outside undo. With events inline,
  every MIDI edit is an ordinary snapshot step (ADR-0009 §1).
- Operations: the region-level operations by id (`MoveRegion`,
  `TrimRegion`, `SplitRegion`, `RemoveRegion`) apply to MIDI regions too,
  with positions in the region's timebase; new are `PlaceMidiRegion`,
  `EditMidi` (remove by index, add by value: every piano-roll, event-list
  and `common/midi` edit resolves to it), `AddMidiSource`,
  `SetMidiRouting`, `SetInstrument`, `RemoveInstrument` (spec §3.1).
  `kProtocolVersion` 6 → 7 and the undo window format move with them.
- **Amends ADR-0007 §1:** two new edges, `common/model` → `common/midi`
  (both layer 1, both neutral, `common/midi` links only `common/base`, no
  cycle) and `interchange` → `common/midi` (for the SMF codec, §7). Additive under §6: reviews by rt-engineer and ui-engineer.
  `common/midi`'s `TempoMap` stays the SMF-domain map for the codec; the
  session's map is the model's (§2).

### 2. The timebase: 960,000 ticks per quarter, against the frame-anchored tempo map

- Every MIDI position in the model and the file is an **integer tick at
  960,000 per quarter note**: Pro Tools' published internal resolution.
  The reason is arithmetic, not imitation. A tick must be shorter than a
  frame, or a recorded event cannot return to its frame. At 960,000 that
  holds for every tempo above 3 bpm at 48 kHz and 12 bpm at 192 kHz.
  Logic's 960 per quarter would move a recorded note by up to 12 frames at
  120 bpm and fail the exit criterion.
- **The tempo map keeps its v3 meaning in v4**: entries anchored at frames,
  float bpm. It is not reinterpreted, as v3's spec promised. v4 adds one
  canonical conversion, frames ↔ ticks ↔ bars|beats (`MusicalMap` in
  `common/model`), defined exactly in spec §2.2: each entry's tick is
  derived by rounding once from the entry before, each conversion counts
  from its own entry, and the arithmetic is **128-bit integer on the
  float's exact value**, so GCC and MSVC, with or without FMA contraction,
  give the same tick. `frame(tick(f)) == f` whenever ticks are finer than
  frames; that identity is what lets a take recorded in frames be stored in
  ticks and played back on the frame it was recorded at.
- The bars|beats reading in `ui/qt` moves onto `MusicalMap` in the same
  PR, so the grid, the counters and a note at 5|1|000 are one function.
- **Audio stays in frames; MIDI regions move in frames when the tempo
  changes**, as tick-based tracks do in both vendors. A tempo change
  rebuilds the plan.
- **DAW 7's choice is kept open, not made.** Tempo editing as musicians
  expect it (a tempo change at bar 17 stays at bar 17 when an earlier
  tempo moves) needs entries anchored in ticks. That is DAW 7's ADR. If it
  takes that route, its migration fills each entry's tick from this ADR's
  derived `T_k`: a filled default, the session format's own test of a good
  migration.

### 3. Session format version 4

Spec §4. In short: `sonora_session` 4; a `MIDI Files/` folder;
`midi_sources[]` beside `sources[]`; every track gains `kind`, with the key
set strict per kind; MIDI regions with `position_ticks`, `length_ticks`,
`offset_ticks` (named so that no reader takes them for frames), `notes` as
one-line arrays `[start, length, channel, pitch, velocity, release]`,
`events` `[tick, status, d1, d2]`, `sysex` `[tick, hex]` up to 4 KiB.
MIDI ports are named by **port name**, not index, with `"all"` the default
input, because a MIDI port is a device that is plugged and unplugged, and an
index would silently point at another one. A named port that is absent is
kept and shown missing. Migration v3 → v4 fills `"kind": "audio"` and
`"midi_sources": []`, nothing else. **Amends `session-format.md`**: its
"every position is an integer count of samples" gains "or, for MIDI,
of ticks at 960,000 per quarter, in keys ending `_ticks`".

### 4. MIDI reaches the audio thread from the backend, per block, at frame offsets

- **`contracts/backend` gains MIDI ports** (ROADMAP DAW 6's first item):
  `AudioBlock` carries `midi_inputs` and `midi_outputs`, each a fixed array
  of `MidiBuffer` slots the driver allocates at `open()`, messages stamped
  with their frame offset in the block. A non-RT query names each slot's
  device with a generation counter; devices take and free slots without
  reopening the backend. Additive under ADR-0007 §6 (rt-engineer and
  driver-engineer); the conformance suite gains MIDI checks.
- `drivers/jack` uses JACK MIDI ports through PipeWire's JACK API, whose
  events carry a sample offset in the cycle by design ([PipeWire,
  MIDI](https://docs.pipewire.org/page_midi.html)). `drivers/fake` takes
  scripted MIDI at exact frames and captures MIDI out, so the exit test is
  deterministic. A Windows driver (ADR-0008; ASIO carries no MIDI) stamps
  events into the same buffers from its own MIDI thread. The contract does
  not change for it.
- **On the audio thread** (spec §5), per MIDI or instrument track, into a
  preallocated `MidiBuffer`: generated note-offs, chase, plan playback
  through a cursor, thru from the routed inputs, on-screen keyboard notes
  from `RtCommand`s. Every overflow is dropped and counted, never waited
  on. Instrument tracks hand the buffer to their chain. MIDI tracks write
  their output port **delayed by the plan's total compensation**, through a
  fixed per-port queue, so an external synth sounds with the delayed audio.
- **The plan carries MIDI as frames.** The builder flattens each active
  playlist to `{frame, bytes}` with `MusicalMap`, notes to on and off pairs,
  in the plan's one allocation, with fixed maxima named when exceeded.
  MIDI needs no residency gate: it is already in memory.
- **No hanging notes.** The audio thread tracks sounding notes and held
  sustain per destination and ends exactly those on stop, locate, loop
  end, mute, routing change, region end and any plan swap that changes the
  track's MIDI. **Chase** (program, bank, controllers, bend, pressure) is
  computed on the control thread into one of two preallocated slots per
  track and named by the play or locate `RtCommand`; the loop-start chase
  is in the plan.

### 5. Instruments are plugin chains with an event input (amends ADR-0012)

- The instrument is **its own slot** ahead of inserts A–J (owner question
  O6); afterwards it is an `Insert` by id, so active, bypass, parameters,
  state and `ReplaceInsert` work on it unchanged.
- **Amends ADR-0012 §2:** an instrument track's chain is one host running
  the instrument and then the track's inserts (at most 11 instances, inside
  `kMaxInstances` = 16). **Amends §7.1:** the chain's inputs equal the
  track's channels, or 0 for an instrument chain.
- **`contracts/plugin` layout 2:** the channel region gains a fixed event
  area that the audio thread writes with the block's input before
  publishing `submitted_block`. The host gives the plugin its own form (LV2
  atom sequence; VST3 events plus `IMidiMapping` for controllers). Depth 1,
  misses, fades and PDC are ADR-0012's, unchanged: an instrument's latency
  plus its IPC block is compensated like any chain's. Live playing through
  an instrument hears that latency; that is the price of never waiting
  (ADR-0012 §1.5), stated rather than hidden.

### 6. Recording: stamped against the audio clock, written as an SMF whose tick is a frame

- An input event's timeline position is the frame that was **audible to the
  player** when it reached the engine: block frame + offset − (playback
  latency + plan compensation + the backend's MIDI input lag) + a session
  offset (spec §7.1). The sum is recorded per take as
  `latency_compensation`, as audio does.
- Captured events go to the disk thread on an SPSC ring, framed by the
  in-band boundary records audio takes already use. **A full ring is an
  overrun: counted and reported as MIDI lost.** At a window's close the
  audio thread ends the notes still held, so the file is well-formed.
- **Each MIDI take is written to disk while it is recorded**, as audio is,
  never kept only in memory until a save. The file is a format 0 **SMF
  whose division and tempo make one tick one frame** (24,000 ppq at
  500,000 µs per quarter for 48 kHz; spec §7.2 lists every rate), with a
  text event carrying take, track, session and time reference, as BWF's
  `Description` does. Any SMF reader places it in true time; it is
  self-describing for recovery. Checkpoints patch the `MTrk` length and
  `fdatasync` on the audio schedule. Recovery appends the end of track:
  the one write the format allows to an unfinished file, as for audio.
- At close the control thread reads the file back, converts to ticks and
  applies the take compound with the audio tracks' members ("Record", one
  undo step). Undo removes the region, never the file or the pool entry.

### 7. SMF import and export in `interchange/`, in `sonora-convert`

As ADR-0007 §1 and §3 already place them: an SMF from outside is untrusted
and is parsed in its own short-lived process, with `common/midi`'s bounded
codec. Import copies the file in, makes regions per `MTrk`, scales ticks
exactly where the division divides 960,000 and reports any rounding, and
offers the file's tempo map without applying it. Export writes format 1, a
conductor track and one track per Sonora track, at 960 per quarter by
default (O2).

### 8. MIDI 1.0 now; MIDI 2.0 and UMP not foreclosed

The model and the RT buffers hold MIDI 1.0. Higher resolution later is a
format version whose migration upscales by the published MIDI 2.0 rule; UMP
on the wire is a driver concern. MPE needs nothing new.

### 9. Phases

Each is its own PR or PR series, in this order; each leaves the product
runnable.

| Phase | Scope | Gate |
|---|---|---|
| M-0 | this ADR and `midi.md` | owner accepts, answers O1–O6 |
| M-1 | `common/model`: `MusicalMap` (and `ui/qt` onto it), track kinds, MIDI playlists and regions, `midi_sources`, operations and codec, protocol 7, window format, format v4 with fixtures. The engine plays nothing on MIDI tracks yet and says so as a Notice | rt-engineer and ui-engineer reviews (ADR-0007 §6); Unverified 6 measured |
| M-2 | `contracts/backend` MIDI ports, `drivers/fake` MIDI, `drivers/jack` JACK MIDI, conformance | researcher's Unverified 1 and 2 on the laptop (proxies) |
| M-3 | engine playback to external ports: plan MIDI, cursors, note-off bookkeeping, chase, compensation delay; ctests on fake | rt-safety-auditor; Unverified 7 |
| M-4 | MIDI recording: capture ring, SMF take files, checkpoints, recovery, replace mode, then merge; cycle with DAW 3's take lanes | a MIDI take survives `kill -9` (ctest, as `test_take_kill`) |
| M-5 | instruments: `contracts/plugin` layout 2, LV2 then VST3 event delivery, instrument tracks, PDC; **exit criterion part 1** as a ctest on fake | rt-engineer + plugin-engineer; Unverified 4 |
| M-6 | SMF import and export in `interchange/` through `sonora-convert`; **exit criterion part 2** as O2 states it | |
| M-7 | the Qt UI: MIDI and instrument tracks in Edit and Mix, piano roll, event list, MIDI operations, step input, on-screen keyboard. Each with its own design note and screenshots | ui-engineer |

Later, each with its own ADR or amendment: MIDI tracks driving instrument
tracks, sample-based MIDI tracks, plugin MIDI output and MIDI effects, MIDI
learn (with DAW 10), score and notation, MIDI 2.0.

## Rationale

- **A lost take decides recording.** A MIDI take that lived only in memory
  until a save would be lost by a crash, a power cut or an engine restart,
  exactly the cases audio is already protected against. Writing it to disk
  as it is played, checkpointed and recoverable from its own header, gives
  MIDI the same loss window as audio. Using an SMF with tick = frame, rather
  than a private journal, means the file opens in any DAW at the right time
  when nothing else survives.
- **One clock.** MIDI is stamped by the backend at a frame offset in the
  same callback as the audio, so the audio clock is the only clock. A
  separate MIDI reader thread stamping `CLOCK_MONOTONIC` would need a
  second clock correlated to the first, on the audio thread, and the
  correlation's error lands directly on note timing.
- **Fail loudly, never block.** Every buffer is fixed at creation; every
  overflow is a counted drop with a name (MIDI lost, MIDI dropped to a
  plugin, SysEx too large); a plan over its maxima is refused at build; a
  missing port is shown missing.
- **Ticks for the model, frames for the audio thread.** Ticks are what
  editing needs (a quantize grid, a tempo change that moves the music);
  frames are what the callback can use without arithmetic it should not
  do. The builder converts once, off the audio thread, with an exact
  function every reader shares.
- **Hardware we have never seen.** Port names rather than indices, `"all"`
  as the input default, devices that come and go without reopening the
  backend: a session opens and plays on a machine whose MIDI devices are
  different, and says what is missing.
- **What it costs later is stated.** The frame-anchored tempo map means
  DAW 7 has a migration to write if it anchors entries in ticks; the
  derived `T_k` makes it mechanical. Inline events bound the session by the
  parser caps; the measurement (Unverified 6) and the remedy (sequences
  beside the file, like plugin state) are named now.

## Consequences

- Three contracts change: `common/model` (format 4, new operations,
  protocol 7), `contracts/backend` (MIDI ports), `contracts/plugin`
  (layout 2). Each needs its owners' reviews and a `CONTRACTS.lock`
  regeneration in its own PR.
- `ui/qt`'s bars|beats code moves into `common/model`. Its tests are the
  check that nothing the operator sees moved.
- The plan grows by 16 bytes per MIDI event. 500,000 events is 8 MB per
  plan, and a note edit or tempo change rebuilds it (Unverified 7).
- The disk thread writes a second kind of take file. The `fdatasync`
  schedule and the recovery scan cover `MIDI Files/` too.
- An instrument chain takes one host process per instrument track, as
  insert chains do (ADR-0012 Unverified 3, locked memory per host, applies).
- Live MIDI through an instrument has at least one block plus the plugin's
  latency. Low-latency monitoring through instruments is the same open
  item as through inserts (ADR-0012 §1.5).
- Interface notes (on-screen keyboard) have one block of jitter; MIDI from
  a device has the backend's.
- `common/midi`'s `StreamParser` comment ("the port reader ... hands short
  messages to the audio thread through a ring") describes the rejected
  design for JACK; it remains right for a driver that reads raw bytes, and
  its comment is corrected in M-2.

## Unverified

Each names the measurement that settles it and who runs it.

1. **PipeWire's MIDI timing to a JACK client.** Whether an event's offset
   reflects its arrival time or is put at the cycle start, whether events
   are one period late (the assumed `L_midi_in`), and the jitter.
   Laptop proxy: `snd-virmidi`, send notes at known `CLOCK_MONOTONIC`
   times, read offsets in a JACK client at quantum 128 and 1024,
   distribution of error in frames. Real figure in OS 1: UMC1820 MIDI out
   cabled to MIDI in. Researcher, before M-2 merges.
2. **Port names across replug.** Which name PipeWire gives a USB MIDI
   device's JACK port, and whether it is the same after unplug, replug, a
   second identical device and a reboot. Researcher, with 1.
3. **UMP devices.** Whether PipeWire (1.4 and trixie's version) presents
   MIDI 2.0 devices to JACK clients as MIDI 1.0. Researcher, before M-2.
4. **Instrument event delivery.** LV2 atom MIDI and VST3 events plus
   `IMidiMapping` across a set of free-licensed instruments: notes at their
   offsets, controllers and bend arriving, what plugins without
   `IMidiMapping` do with CC. plugin-engineer, in M-5.
5. **Vendor behaviour this ADR assumes** (researcher, with the PT-RG
   pinning already in BACKLOG): whether Pro Tools and Logic play a note
   past its region's end; their default record mode over existing MIDI;
   what a MIDI region split does to a crossing note; their chase defaults.
   O1 and O4 rest on it.
6. **Session size with dense MIDI.** 18 instrument tracks of a 5-minute
   piece with continuous controllers: file bytes, DOM values against the
   4,000,000 cap, parse time, undo window bytes over the socket.
   rt-engineer, in M-1. Over the caps means sequences beside the file
   (version bump), decided then.
7. **Plan build with MIDI.** 500,000 events on 18 tracks, against the
   10 ms control-thread budget (ADR-0009, 2026-10-13 update). rt-engineer,
   in M-3. Over it means MIDI segments flattened per region and cached
   across builds.
8. **Windows MIDI stamping under ASIO**, only when a Windows ADR opens
   (ADR-0008 §6).

## For the owner

Each has a recommended default that the spec builds on until you rule.

- **O1. Default record mode over existing MIDI.** *Recommended: replace*
  (the new take overwrites the range, as audio does), with a Merge toggle
  in the Transport window. Merge as the default is the alternative. Vendor
  defaults are Unverified 5.
- **O2. The SMF half of the exit criterion.** An SMF division is at most
  32,767 per quarter, so an unquantized performance at frame accuracy
  cannot pass through one unchanged. *Recommended:* restate it as
  "export → import → export is byte-identical; notes on the export grid
  return exactly; every other note returns within half an export tick, and
  the report counts them", with export at 960 per quarter by default and
  up to 32,767 by choice. The alternative, a private meta event carrying
  each note's remainder, makes only Sonora round-trip and needs a
  manufacturer ID we do not have.
- **O3. Default MIDI input of a new track.** *Recommended: all inputs,
  merged* (Pro Tools' "All"), so a keyboard plugged into an unfamiliar
  machine plays at once. The alternative is none until chosen.
- **O4. A note that runs past its region's end.** *Recommended: it ends
  at the region's end.* It keeps "at most one region sounds per track"
  true and bounds hanging notes. The alternative plays it to its full
  length. Unverified 5 says which vendor does which.
- **O5. MIDI routing and undo.** *Recommended: not undoable,* as a
  track's audio input is not. The alternative puts it in edit undo.
- **O6. Where the instrument sits.** *Recommended: its own slot ahead of
  A–J* (Logic's layout), drawn as the top insert row in the Mix window so
  Pro Tools users find it where they expect. The alternative is Pro Tools'
  model, an instrument in any insert slot, where effects above it receive
  silence and the chain's channel rule needs a case per slot.

## Alternatives rejected

- **MIDI sequences in the append-only pool, regions as windows onto
  them** (the audio shape). Rejected: every note edit would add a pool
  entry that undo cannot remove, so the pool, the file and the socket's
  snapshots grow with editing, not with recording. The pool keeps the
  takes (§6), which is what it is for.
- **Sequences beside the session file, content-addressed** (the
  plugin-state shape). Smaller file and snapshots. Rejected for now: every
  edit becomes a file to write at save, a replica must read files from
  the session folder, and the measured need is not shown. It is the named
  remedy if Unverified 6 fails.
- **960 ticks per quarter** (Logic's, the display resolution). Rejected:
  recording would quantize to 25 frames at 120 bpm and 48 kHz and fail the
  exit criterion.
- **MIDI positions in frames** (sample-based MIDI only). Exact and simple.
  Rejected: quantize, grid edits and every tempo change need musical time,
  and both vendors make MIDI tick-based by default. Sample-based MIDI
  tracks stay possible later as a track timebase.
- **Anchoring the tempo map in ticks now.** It is where DAW 7 probably
  goes. Rejected for DAW 6: it reinterprets a v3 field v3 promised to keep,
  rewrites the counters and grid now, and settles DAW 7's tempo editing
  semantics before that ADR exists. The derived `T_k` keeps the later
  migration mechanical.
- **Float or double tick conversion.** Shorter code. Rejected: two
  compilers, or one with FMA contraction, can round a note to different
  frames, and the engine and the UI would then disagree about where it is.
- **A MIDI reader thread outside the backend** (ALSA sequencer or rawmidi,
  `CLOCK_MONOTONIC` stamps, a ring to the audio thread). Independent of
  JACK. Rejected: a second clock to correlate with the audio clock, on the
  audio thread, with its error landing on note timing; and a Linux API in
  a path ADR-0008 wants behind `drivers/*`. A driver may still do this
  internally, as a Windows one would.
- **Holding a MIDI take in memory until the take closes or the session
  saves.** No file format to write. Rejected: a crash during a long take
  loses all of it, the worst outcome.
- **A private binary journal for MIDI takes.** Simpler to append to.
  Rejected: an SMF with tick = frame is as easy to append to with a
  patched length, and every DAW reads it when the session does not survive.
- **MIDI ports by index**, as audio channels are. Rejected: MIDI devices
  are plugged and unplugged, and an index would point at another device
  without a word.
- **Separate `MoveMidiRegion`, `TrimMidiRegion`, … operations.** Units
  explicit in the type. Rejected: it doubles the vocabulary and the codec,
  and every compound sender (cut, paste, Shuffle) would need two cases. The
  existing operations name regions by id; the unit follows the playlist.
- **Pre-reading playback MIDI by the instrument's latency** instead of
  delaying the other paths. It would keep an instrument's latency out of
  the mix's total. Rejected for now: one node would need two latencies (its
  playback and its live input), and ADR-0012's single PDC mechanism already
  aligns it. Reconsidered if an instrument's latency is measured as a
  problem.
- **A `RecordMidiTake` operation.** Rejected: replace is `PlaceMidiRegion`
  exactly and merge composes `EditMidi` and `TrimRegion`; the take is the
  compound ADR-0009 already defines.
