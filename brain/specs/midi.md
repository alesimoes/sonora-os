# MIDI: model, timebase, session format v4, real-time path, take files, SMF

Status: design, written before the code. **Proposed** with
[ADR-0014](../decisions/ADR-0014-midi-model-and-realtime-path.md); nothing
here binds until the owner accepts it. Builds on
[`session-format.md`](session-format.md) (v3 on `main`),
[`timeline-model.md`](timeline-model.md), ADR-0009 (snapshots, operations,
plans), ADR-0012 (plugin hosting) and the code already on `main`:
`common/midi` (events, notes, edits, SMF codec, stream parser, PR #23) and
`sonora::MidiBuffer` in `common/base`.

Sources, all published: *MIDI 1.0 Detailed Specification* and *Standard
MIDI Files 1.0* (MIDI Association); *Universal MIDI Packet and MIDI 2.0
Protocol* (M2-104-UM) only where noted. Workflow references: Avid, *Pro
Tools Reference Guide* (PT-RG, edition not yet pinned), Apple, *Logic Pro
User Guide* (LP-UG 12.3). ADR-0004 holds: behaviour from documents, never
code.

## 1. Units

| Quantity | Unit | Why |
|---|---|---|
| Audio positions, lengths, offsets | frames at the session rate | unchanged (session-format.md, "Numbers") |
| MIDI region positions, lengths, offsets, note and event times | **ticks, 960,000 per quarter note** (`kTicksPerQuarter`) | below |
| MIDI take files, capture and the plan | frames | the audio clock is the one clock |
| SMF interchange | the file's own division | the SMF header decides; converted at the boundary |
| Display of bars\|beats | 960 per beat, as now (`ui/qt` `time_format.h`) | display only, unchanged |

**Why 960,000 per quarter.** It is Pro Tools' published internal MIDI
resolution, with 960 per quarter shown in Bars|Beats ([Avid, Tick-Based
Timing](https://apps.avid.com/proToolsFirstHelp/version12.3/enu/Pro%20Tools%20First%20Help/Arr1.conductors.31.09.html)).
What decides it here is arithmetic: a tick must be shorter than a frame, or
a recorded event cannot come back to the frame it was recorded at. One tick
lasts `60·rate / (bpm·960,000)` frames: 0.025 at 120 bpm and 48 kHz, 0.1 at
120 bpm and 192 kHz. It is under one frame whenever `bpm ≥ 60·rate /
960,000`, which is 3 bpm at 48 kHz and 12 bpm at 192 kHz. Logic's tick,
1/3840 of a whole note (960 per quarter, LP-UG glossary), lasts 25 frames at
120 bpm and 48 kHz: recording at that resolution would move a note by up to
12 frames, which fails the DAW 6 exit criterion ("every note within one
sample of its recorded position").

Range: ticks are `int64`, at most `2^53 − 1` in the file. That is 9.4·10⁹
quarter notes, about 150 years at 120 bpm.

## 2. The musical map: frames ↔ ticks ↔ bars|beats

### 2.1 The tempo map stays anchored in frames in v4

`tempo` keeps its v3 meaning: entries at frames (`at`), `bpm` a float,
`meter`. It is not reinterpreted. ADR-0014 §2 has the reasons and the
DAW 7 consequence. What v4 adds is **one canonical conversion**, in
`common/model`, that every reader uses: the engine's plan builder, the
take resolver, the UI's ruler, counters and grid, the SMF converter.

### 2.2 The conversion, exactly

Notation: `R` the session rate; `Q` = 960,000; for entry *k*, `a_k` its
`at`, and `r_k` the **exact rational value** of its `bpm` float (a binary32
is `m·2^e` with integer `m`, so this is exact). Ticks per frame in entry
*k*: `τ_k = r_k·Q / (60·R)`. `⌊x⌉` is `⌊x + ½⌋` (half rounds up; every
argument is non-negative).

- `T_0 = 0`; `T_{k+1} = T_k + ⌊(a_{k+1} − a_k)·τ_k⌉`. `T_k` is entry *k*'s
  tick. It is derived, never stored.
- **frames → ticks:** `tick(f) = T_k + ⌊(f − a_k)·τ_k⌉`, *k* the last entry
  with `a_k ≤ f`.
- **ticks → frames:** `frame(t) = a_k + ⌊(t − T_k) / τ_k⌉`, *k* the last
  entry with `T_k ≤ t`.
- Each conversion counts from its own entry, so rounding never accumulates
  along the map (the v3 rule, kept).
- **Computed in integers.** `r_k` is expanded from the float's bits and the
  products are formed in 128-bit integers, portably (no `__int128`, which
  MSVC lacks; ADR-0008). No `float` or `double` is in the conversion, so
  GCC and MSVC, with or without FMA contraction, give the same tick.

Properties the M-1 PR tests:

1. `frame(tick(f)) == f` for every frame when `τ_k ≥ 1` (ticks finer than
   frames). Proof sketch: `tick(f) = (f − a_k)·τ_k + e`, `|e| ≤ ½`; dividing
   by `τ_k` gives `(f − a_k) + e/τ_k`, and `|e/τ_k| ≤ ½`. Below 3 bpm at
   48 kHz the identity does not hold, and a recorded event rounds to the
   nearest tick; the take's report says so.
2. Both are monotonic non-decreasing.
3. `tick(a_k) == T_k` and `frame(T_k) == a_k`.

### 2.3 Bars and beats from ticks

The bar rule of session-format.md ("How the map is counted") restated in
ticks: each entry carries, derived like `T_k`, its bar number and the tick
of the bar in progress. A bpm-only entry carries the count on; a meter entry
starts a bar at `T_k`; a bar it cuts short (more than half a display tick
past a bar line) is still a bar. A bar lasts `num · Q · 4 / den` ticks.

**One implementation.** Today the reading lives in `ui/qt` (`time_format.h`,
`musical_time`, `frame_of`, `snap_to_grid`), in `double`. It moves into
`common/model` (`MusicalMap`, built from `SessionSettings`) in the M-1 PR,
and `ui/qt` calls it. Otherwise a note at 5|1|000 in the piano roll and the
grid line at 5|1|000 in the Edit window would be computed by two functions,
and could differ by a frame. The UI tests that pin today's reading
(`ui_transport`, `ui_edit`) must pass unchanged or the PR lists every
difference. *Expected: none past one frame; unverified.*

## 3. The model (`common/model`)

Sketches of shape; rt-engineer settles signatures in the M-1 PR (ADR-0007
§6: `common/model`'s public headers are contract files). New edge:
`common/model` → `common/midi` (both layer 1, both neutral; ADR-0014 §1).

```cpp
namespace sonora::model {

using Ticks = std::int64_t;                 // 960,000 per quarter note
constexpr Ticks kTicksPerQuarter = 960'000;

enum class TrackKind : std::uint8_t { Audio, Midi, Instrument };

// Notes and other events of one region, in data ticks. Immutable, shared
// between regions (split, duplicate, paste) and between snapshots.
struct MidiSequence {
    std::vector<midi::Note> notes;          // sort_notes() order
    std::vector<MidiEvent> events;          // channel messages other than notes, by tick, stable
    std::vector<MidiSysEx> sysex;           // by tick, stable
};

struct MidiRegion {
    Id id;
    std::string name;
    Ticks position;                         // timeline tick of the visible window's start
    Ticks length;                           // > 0
    Ticks offset;                           // data tick shown at `position`; >= 0
    bool mute = false;
    Id source = 0;                          // the take or import it came from; 0 for none
    std::shared_ptr<const MidiSequence> data;
};

struct MidiPlaylist {                       // same rules as Playlist, in ticks; no crossfades
    Id id;
    std::string name;
    std::vector<MidiRegion> regions;        // sorted by position, never overlapping
};

struct MidiRouting {                        // TrackMix: saved, not undoable (like `input`)
    std::string in_port = "all";            // "all", or a port name (§4.3)
    std::uint8_t in_channel = 0;            // 0 all, 1..16
    std::string out_port = "none";          // MIDI tracks only: "none" or a port name
    std::uint8_t out_channel = 0;           // 0 as recorded, 1..16
};

struct MidiSource {                         // SourcePool, append-only
    Id id;
    std::string path;                       // "MIDI Files/<name>.mid"
    Frames length;                          // frames from take start to its end
    std::uint64_t events;
    std::int64_t time_reference;            // samples since midnight of the take's frame 0
    SourceKind kind;                        // Recorded | Imported
    std::int64_t take; Id recorded_on_track; Frames latency_compensation;
};
}
```

- `TrackEdit` gains `kind`, `midi_playlists` (used instead of `playlists`
  when `kind != Audio`) and `std::optional<Insert> instrument` (instrument
  tracks only). `check_invariants` refuses a track whose playlists do not
  match its kind.
- **The window, not the data, is trimmed.** A note is heard if its start
  is in `[offset, offset + length)`. It is ended at the region's end if it
  runs past it (§5.3). Notes outside the window stay in `data`, so a trim
  undone, or dragged back out, brings them back (non-destructive, as both
  vendors' region trims are; *which of them plays a note past the region
  end is unverified*, owner question O4).
- **Copy-on-write sequences.** Split, duplicate and paste make regions
  that share one `data`. Editing notes through one of them gives that
  region a new `data`; the others keep theirs. So copies are independent,
  as Pro Tools' clips are. Aliases (Logic) would be an explicit later
  feature, not an accident of sharing.
- Undo memory is charged as ADR-0009 §1 says: a note edit allocates one new
  `MidiSequence` and the playlist copy; a move or trim allocates no
  sequence at all.

### 3.1 Operations

| Operation | Undoable | Effect |
|---|---|---|
| `AddTrack` | yes | gains `kind`, and for MIDI and instrument tracks a `MidiRouting` in its mix |
| `AddMidiSource{source}` | no (pool) | a MIDI take or an imported SMF |
| `PlaceMidiRegion{playlist, region, split_right_id}` | yes | as `PlaceRegion`, overwrite included (timeline-model.md, "Overwrite"), in ticks |
| `MoveRegion`, `TrimRegion`, `SplitRegion`, `RemoveRegion` | yes | **unchanged ops, now also on MIDI regions**: they name a region by id, and the positions they carry are in that region's timebase (ticks on a MIDI playlist). `SetFade`, `SetCrossfade` on a MIDI region are refused by name |
| `EditMidi{region, remove_notes, add_notes, remove_events, add_events, remove_sysex, add_sysex}` | yes | removes by index into the region's current sequence, adds values, re-sorts; gives the region a new `data`. Every piano-roll and event-list edit, quantize, transpose, velocity, legato (`common/midi` `edit.h`) resolves to it |
| `SetMidiRouting{track, routing}` | no | MIDI input and output; mix state like `input` (owner question O5) |
| `SetInstrument{track, insert, plugin, params, state}`, `RemoveInstrument{track}` | yes | the instrument slot; afterwards it is an `Insert` by id, so `ReplaceInsert`, `SetInsertActive`, `SetInsertBypass`, `SetInsertParams`, `SetInsertState` apply to it unchanged |

- **A take is one compound**, labelled "Record", as ADR-0009's
  2026-10-14 update already requires: per audio track `AddSource` +
  `RecordTake`, per MIDI or instrument track `AddMidiSource` +
  `PlaceMidiRegion` (replace) or `AddMidiSource` + `EditMidi` and
  `TrimRegion` (merge). No `RecordMidiTake` operation: replace is
  `PlaceMidiRegion` exactly, and merge composes existing members.
- **Resolved, so `apply` stays pure.** The engine converts the take's
  frames to ticks with §2.2 before it builds the compound; `apply` never
  reads the tempo map to place a region.
- `kProtocolVersion` 6 → 7 (new operation kinds the engine may send,
  `messages.h` rule); `kWindowFormat` bumped with them; op codec entries
  for each; `FakeEngine` and the conformance suite follow.

### 3.2 What a tempo change does

`SetSettings` with a new tempo map is unchanged and not undoable. Audio
does not move (v3 rule). MIDI regions keep their ticks, so they move in
frames, as tick-based tracks do in both vendors. The plan is rebuilt. A
tempo change while rolling is heard at the next plan swap, with the
note-off rule of §5.3.

## 4. Session format version 4

`sonora_session` becomes **4**. Everything in v3 keeps its meaning.

### 4.1 Folder

```
<Session Name>/
  MIDI Files/        MIDI takes and imported SMFs, one file each (version 4)
```

### 4.2 Top level

`midi_sources` follows `sources`. Same id space.

```json
"midi_sources": [
  {
    "id": 48,
    "path": "MIDI Files/Piano_03.mid",
    "length": 1920000,
    "events": 412,
    "time_reference": 175200000,
    "kind": "recorded",
    "take": 3,
    "recorded_on_track": 9,
    "latency_compensation": 384
  }
]
```

Checked at open against the file (exists, parses with `read_smf`, event
count). A missing or changed file flags the source and tells the operator;
it never changes what plays, because the regions hold their events (§4.4).

### 4.3 Tracks: `kind`, and keys by kind

Every track gains `"kind"` as its second key. The key set depends on it;
the strict parser refuses a key that does not belong to the kind.

| Key | audio | midi | instrument |
|---|---|---|---|
| `id`, `kind`, `name`, `monitor`, `mute`, `solo`, `solo_safe`, `active_playlist`, `playlists` | ✓ | ✓ | ✓ |
| `channels` | 1–2 | — | 1–2, the instrument's outputs |
| `input` | ✓ | — | — |
| `gain_db`, `pan`, `inserts` | ✓ | — | ✓ (inserts after the instrument) |
| `midi_input` | — | ✓ | ✓ |
| `midi_output` | — | ✓ | — |
| `instrument` | — | — | ✓, an insert object without `slot`, or absent |

```json
"midi_input": { "port": "all", "channel": 0 },
"midi_output": { "port": "UMC1820 MIDI 1", "channel": 0 }
```

- `port` is `"all"` (input only: every MIDI input merged, the default for a
  new track; owner question O3), `"none"` (output only), or a **port name**
  as the backend reports it. Names, not indices, unlike audio channels: a
  MIDI port is a device (this keyboard, that synth) that comes and goes
  while the session runs, and an index would point at another device after
  a replug. A named port that is absent at open is shown missing, kept, and
  never dropped (the rule for missing plugins). *Which name PipeWire keeps
  stable across replugs is unverified* (ADR-0014 Unverified 2).
- `channel`: 0 means all (input) or as recorded (output), else 1–16.
- `monitor` on a MIDI or instrument track governs MIDI thru: `"auto"`
  passes input to the track's destination while it is armed, as Pro Tools'
  MIDI Thru does.

### 4.4 MIDI playlists and regions

A MIDI or instrument track's `playlists` hold MIDI regions and have no
`crossfades` key.

```json
{
  "id": 52,
  "name": "Piano_03",
  "position_ticks": 15360000,
  "length_ticks": 30720000,
  "offset_ticks": 0,
  "mute": false,
  "source": 48,
  "notes": [
    [0, 480000, 0, 60, 96, 64],
    [960000, 455000, 0, 64, 88, 64]
  ],
  "events": [
    [0, 176, 64, 127],
    [1920000, 176, 64, 0]
  ],
  "sysex": []
}
```

- Unit names carry `_ticks` so no reader takes them for frames, which every
  other position in the file is.
- `notes`: `[start, length, channel, pitch, velocity, release_velocity]` in
  data ticks; `start ≥ 0`, `length ≥ 0`, channel 0–15, pitch 0–127,
  velocity 1–127 (0 is a note-off on the wire), release 0–127. In
  `sort_notes()` order. Arrays of scalars, so one line per note
  (canonical JSON rule).
- `events`: `[tick, status, data1, data2]`, channel messages other than
  note on and off (`0xA0`–`0xEF`), data bytes 0–127, `data2` 0 for
  two-byte messages. By tick, and at one tick in stored order: that order
  is meaning (a bank select before the program change, `event.h`
  `sort_by_tick`).
- `sysex`: `[tick, "f0…f7"]`, lower-case hex of the whole message, at most
  **4,096 bytes** (`kMaxSysexBytes`, the per-block byte budget of §5.1).
  Larger messages are not stored; import reports each one dropped.
- `source`: optional, the `midi_sources` id the region came from.
- **Invariants checked at load:** regions sorted by `position_ticks` and
  not overlapping; `length_ticks > 0`; `offset_ticks ≥ 0`; every value in
  range; notes and events sorted. A region may hold notes outside its
  window (§3).
- **No meta events.** Tempo, meter and markers belong to the session
  (`tempo`, `markers`); text, lyrics, key signatures from an SMF are
  reported and not kept in v4.

### 4.5 Migration v3 → v4

Every track gains `"kind": "audio"`; the top level gains
`"midi_sources": []`. Nothing else changes. The frozen v3 fixture loads
through it; v4's is `v4-midi.sonora` with one track of each kind. The
pre-migration backup rule applies as for every version.

### 4.6 Size

About 30 bytes and 7 DOM values per note. The v3 parser caps (64 MiB,
4,000,000 values) therefore hold about 570,000 notes and events in one
session. A dense performance with continuous controllers can reach tens of
thousands of events per track. *Unverified whether real sessions stay
inside* (ADR-0014 Unverified 6); the measured remedy, if not, is the
plugin-state pattern: sequences beside the file, content-addressed, a
version bump.

## 5. The real-time path

### 5.1 MIDI ports in the backend contract (`contracts/backend`)

```cpp
struct AudioBlock {
    // ... v1 fields unchanged ...
    const MidiBuffer* const* midi_inputs;  uint32_t midi_input_count;
    MidiBuffer* const*       midi_outputs; uint32_t midi_output_count;
};
struct BackendConfig { /* ... */ uint32_t midi_inputs = 16, midi_outputs = 16; };
// Non-RT: which device each slot carries now, and a generation that moves
// on every change, so the control thread can follow a replug.
std::vector<MidiPortInfo> midi_capture_ports();   // {slot, name, generation}
std::vector<MidiPortInfo> midi_playback_ports();
uint32_t midi_input_latency_frames() const;       // how late an event's offset is, by design
```

- Each slot is a `sonora::MidiBuffer` (common/base, already RT-safe) the
  driver allocates at `open()`. Input messages carry their **frame offset
  in this block**; the callback writes output messages with offsets, and
  the driver drains and clears them after `process()` returns.
- **Fixed slot count, devices come and go.** A device plugged in mid-session
  takes a free slot from a non-RT thread in the driver; nothing reopens the
  backend, nothing stops the audio. More devices than slots: the extra one
  is reported, not connected.
- `drivers/jack`: JACK MIDI ports through PipeWire's JACK API (ADR-0003).
  `drivers/fake`: scripted input at given frames, captured output, both
  deterministic, so the DAW 6 exit test runs on it. A Windows driver
  (ADR-0008) would read MIDI on its own thread and stamp events into these
  buffers itself; `common/midi` `StreamParser` exists for a driver that gets
  raw bytes. The contract does not change for it.
- Classified **additive** under ADR-0007 §6 (new fields every backend fills,
  no change to who calls what or to the RT boundary): rt-engineer and
  driver-engineer review; the conformance suite gains MIDI checks
  (offsets inside the block, order kept, overflow counted, no output left
  uncleared).

### 5.2 What the audio thread does per block, per MIDI or instrument track

Into the track's own `MidiBuffer` (preallocated: 1,024 messages, 16 KiB,
placeholders), in this order of offsets, everything dropped past capacity
counted:

1. **Generated note-offs** first, where needed (§5.3).
2. **Chase**, on the first block after a start or locate (§5.4).
3. **Playback**: while rolling, the plan's events in `[playhead, playhead +
   nframes)`, through a per-track cursor (advanced as it plays, reset by
   binary search after a locate or a new epoch, as the segment cursor is).
   A loop end inside the block is §5.3's boundary at its frame.
4. **Thru**: live input from the slots the track's routing selects,
   filtered by channel, when the monitor rule passes input.
5. **Interface notes** (on-screen keyboard), from `RtCommand`s, at offset 0
   (one block of jitter, stated).

Then the buffer goes to its destination:

- **Instrument track:** to its chain, written into the host region's event
  area with the block's (empty) input (§6).
- **MIDI track:** to its output port's slot, **delayed by the plan's total
  compensation** (the `L` at `HardwareOut`, ADR-0012 §7.3) through a
  fixed-capacity per-port delay queue, so an external synth sounds with the
  delayed audio and not ahead of it. Overflow counted.

And, while the track's capture window is open, step 4's events also go to
capture (§7).

### 5.3 No hanging notes

Per destination, the audio thread keeps which notes sound (16 × 128 bits)
and which channels hold the sustain pedal. It sends a note-off for exactly
the notes still sounding, then CC 64 = 0 where held, at the frame of:
stop; locate; a loop end; a plan swap that changes the track's MIDI
(the plan carries a per-track content generation); mute; a routing change;
the end of a region (a note running past its region ends there, owner
question O4). A plan swap never retriggers a note in progress (no note
chase in v1).

### 5.4 Chase

At play and locate, the control thread (not RT) computes per track the
controller state at the start frame from the plan: bank and program, every
CC's last value, pitch bend, channel pressure, from the regions that cover
the start, in that order. It writes it into one of two preallocated chase
slots per track and names the slot in the play or locate `RtCommand`. The
audio thread emits it at offset 0 of the first block. The plan carries the
chase at the loop start, so a loop wrap needs no command.

### 5.5 The plan

The builder flattens each MIDI or instrument track's active playlist into
frames with §2.2: notes become on and off pairs, a note running past its
region gets its off at the region end, muted regions give nothing.

```cpp
struct PlanMidiEvent { int64_t frame; uint32_t sysex_pos; uint8_t size; uint8_t data[3]; };  // 16 bytes
struct PlanMidiTrack { uint32_t engine_track; uint32_t first_event, event_count;
                       uint32_t generation; uint32_t loop_chase; };
```

- In the plan's one allocation, with a SysEx byte pool. Sorted by frame,
  and at one frame offs before ons, then stored order.
- **No residency gate for MIDI**: the events are in the plan, in memory.
  The audio gate of ADR-0009 §4.4 still governs the swap.
- Fixed maxima at session creation: MIDI events per plan, SysEx bytes per
  plan. The builder refuses past them and names the limit (ADR-0009 §4.5).

## 6. Instruments (amends ADR-0012, Proposed with ADR-0014)

- An instrument track's chain is **one host**: the instrument, then the
  track's inserts, at most 11 instances (inside `kMaxInstances` = 16). The
  instrument has its own slot ahead of A–J (owner question O6).
- The chain takes **0 audio inputs** and gives the track's channels.
  ADR-0012 §7.1's "the chain's channel count equals the track's, in and
  out" becomes "out equals the track's; in equals the track's, or 0 for an
  instrument chain".
- Plan: `TrackSource` of an instrument track renders no audio and hands its
  `MidiBuffer` to the `InsertChain` node, which writes it with the submit.
  The rest of the path, misses, fades and PDC are ADR-0012's, unchanged.
  An instrument's latency (+1 IPC block) is compensated like any chain's.
- `contracts/plugin` **layout 2**: the channel region gains an event area
  the audio thread writes with the input planes, before `submitted_block`
  is published (release), and the host reads after its acquire:

  ```cpp
  struct EventArea { uint32_t count; uint32_t bytes; uint32_t dropped;
                     EventEntry entries[kMaxEventsPerBlock];   // {offset u32, size u16, pad u16, byte_pos u32}
                     uint8_t data[kMaxEventBytesPerBlock]; };  // 1,024 and 16 KiB, placeholders
  ```

  `kLayoutVersion` 1 → 2, size asserts move with it, a mismatched host
  exits 3 (ADR-0012 §5.5). Owners: rt-engineer and plugin-engineer.
- The host delivers the events in the plugin's own form: an LV2
  `atom:Sequence` of `midi:MidiEvent` with frame times; for VST3,
  `Vst::Event` note and pressure events in `IEventList`, and controllers,
  pitch bend and channel pressure through the plugin's `IMidiMapping` as
  parameter changes at their offsets (the VST3 SDK's published model;
  how plugins differ is Unverified 4). Validation refuses a plugin offered
  as an instrument that has no event input.
- Plugin MIDI output (arpeggiators, MIDI effects) is not in layout 2.

## 7. Recording MIDI

### 7.1 Timestamp against the audio clock

An input message at offset `o` in a block whose first frame plays timeline
frame `P` is placed at

```
position = P + o − (L_out + L_plan + L_midi_in) + midi_input_offset
```

- `L_out`: the backend's playback latency; `L_plan`: the plan's total
  compensation (what the player hears is delayed by both); `L_midi_in`:
  `midi_input_latency_frames()`, the backend's own lag by design (JACK
  delivers in this cycle what arrived during the last one: assumed one
  period, Unverified 1); `midi_input_offset`: a session setting, default
  0, for a device's own delay.
- The sum is recorded per take as `latency_compensation`, as audio does,
  so a wrong figure can be corrected later.
- On `drivers/fake` every term is exact, which is what the exit test needs.

### 7.2 Capture, boundaries, files

- The audio thread pushes `{timeline_frame, track, size, bytes[3]}`
  (16 bytes) for every captured message onto an SPSC capture ring per
  session, SysEx through a byte ring beside it. **A full ring is an overrun:
  counted, reported as MIDI lost, never a wait** (CLAUDE.md).
- Capture windows open and close with the in-band boundary records of
  timeline-model.md ("Closing a take while rolling"), with an event count
  instead of a frame count. Punch: MIDI is captured in `[A, B)` exactly,
  without the audio's handle. At a window's close the audio thread pushes
  note-offs for the notes the track's capture still holds, so every take
  file is well-formed.
- **Take files are SMFs whose tick is one frame.** Format 0, one `MTrk`;
  division and tempo chosen so `ppq · 10⁶ / tempo = rate`:

  | Rate | Division (ppq) | Tempo (µs/quarter) |
  |---|---|---|
  | 44,100 / 88,200 / 176,400 | 22,050 | 500,000 / 250,000 / 125,000 |
  | 48,000 / 96,000 / 192,000 | 24,000 | 500,000 / 250,000 / 125,000 |

  Any SMF reader then puts every event at its true time; a text meta event
  at tick 0 carries `sonora take=<n> track=<id> session=<name>
  time_reference=<samples> rate=<R>`, as BWF's `Description` does. So a
  MIDI take is self-describing and recoverable from its own header.
- The disk thread writes the file as it drains, opened ahead by the
  control thread like audio takes. At every checkpoint it patches the
  `MTrk` length to the bytes written and `fdatasync`s, on the audio
  schedule (`sync_seconds`). **Recovery** of an unfinished take appends the
  end-of-track event and fixes the length: the one write to an unfinished
  file the format allows, as for audio (session-format.md, "Crash
  recovery"). Orphans are offered for placement from their text event.
- At close the disk thread reports "MIDI take closed, N events". The
  control thread reads the file back with `read_smf`, converts frames to
  ticks (§2.2) and builds the take compound (§3.1). The region holds what
  is on disk, not what the audio thread meant to write.
- A take with no events adds its source to the pool and places no region.
- **Record modes** (DAW 6 roadmap): *replace* overwrites (the default, owner
  question O1); *merge* adds the take's notes to the regions it
  covers and extends them; *cycle* makes one take per pass on DAW 3's
  playlists and take lanes.

## 8. SMF import and export

In `interchange/` (ADR-0007 §1 already lists SMF there), with
`common/midi`'s codec, run inside `sonora-convert`, because an SMF from
outside is untrusted input and ADR-0007 §3 parses those in their own
process. The engine reads only its own take files (bounded, non-throwing
`read_smf`).

- **Import** copies the file into `MIDI Files/`, adds a `MidiSource`
  (`imported`), and makes one region per `MTrk` that holds channel
  messages, on new MIDI or instrument tracks or on chosen ones. Ticks
  scale from the file's division to 960,000 exactly (960,000 is a multiple
  of every common division: 24, 48, 96, 120, 192, 240, 384, 480, 960,
  1,920, 3,840 and 7,680; a division that does not divide it rounds, and
  the report says so). An SMPTE division converts through seconds to
  frames and then §2.2, and the report states any rounding. The file's
  tempo and meter are offered to replace the session's, never applied
  silently.
- **Export** writes format 1: a conductor `MTrk` (tempo as µs per quarter,
  rounded from `bpm` and reported where not exact; meters; markers), then
  one `MTrk` per track with its name. Division **960** by default, 1–32,767
  by choice (owner question O2). A tick that does not land on the export
  grid rounds to the nearest, and the report gives the count.
- **The round trip, stated honestly.** An SMF's division is at most 32,767
  per quarter (15 bits), so an unquantized performance at frame accuracy
  cannot pass through one unchanged. What can be tested, and what O2
  proposes as the exit criterion: export → import → export is
  byte-identical; every note on the export grid comes back exactly; every
  other note comes back within half an export tick, counted in the report.

## 9. MIDI 2.0 and UMP

v4 is MIDI 1.0 throughout: bytes in `MidiBuffer`, 7-bit values in the
model. Not ruled out by anything here:

- UMP on the wire would be a driver concern; JACK MIDI ports carry MIDI 1.0
  bytes. *Whether PipeWire hands UMP devices to JACK clients as MIDI 1.0
  is unverified* (ADR-0014 Unverified 3).
- Higher-resolution velocity and controllers would be a format version
  whose migration upscales 7-bit values by the published MIDI 2.0
  bit-scaling rule: a filled default, the session-format test of a good
  migration.
- MPE needs nothing new: it is MIDI 1.0 on several channels, which the
  `channel` field already carries.

## 10. Open questions

- Unverified items 1–8 of ADR-0014, each with its measurement and owner.
- The SysEx cap (4 KiB) and the per-block buffer sizes are placeholders
  until a bulk dump and a dense controller stream are measured.
- Whether a MIDI track may drive an instrument track (Pro Tools' common
  layering): a later `midi_output.track` key, a version bump.
- Sample-based MIDI tracks (events fixed in frames when the tempo changes,
  Pro Tools' sample timebase): a later `timebase` key, a version bump.
