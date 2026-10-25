# MIDI: model, timebase, session format v4, real-time path, take files, SMF

Status: design, written before the code. **Accepted** with
[ADR-0014](../decisions/ADR-0014-midi-model-and-realtime-path.md) by the
owner, 2026-10-19, O1–O8 as recommended. Revised 2026-10-19 with the
rt-engineer's and plugin-engineer's reviews of PR #93 (each point is mapped
in the PR thread). Builds on [`session-format.md`](session-format.md) (v3 on
`main`), [`timeline-model.md`](timeline-model.md), ADR-0009 (snapshots,
operations, plans), ADR-0012 (plugin hosting) and the code already on
`main`: `common/midi` (events, notes, edits, SMF codec, stream parser, PR
#23) and `sonora::MidiBuffer` in `common/base`.

Sources, all published: *MIDI 1.0 Detailed Specification* and *Standard
MIDI Files 1.0* (MIDI Association); *Universal MIDI Packet and MIDI 2.0
Protocol* (M2-104-UM) only where noted; the LV2 specifications (atom, midi,
urid, time) and the VST3 SDK documentation for §6. Workflow references:
Avid, *Pro Tools Reference Guide* (PT-RG, edition not yet pinned), Apple,
*Logic Pro User Guide* (LP-UG 12.3). ADR-0004 holds: behaviour from
documents, never code.

**Version numbers.** "Format 4", "protocol 7", "plugin layout 2", "clock
layout 2" and "state layout N+1" below mean **the next free number when the
PR lands**. ADR-0013 (Proposed) also names version numbers, and two
Proposed ADRs must not both claim one. The implementing PR writes the
number it took into this spec.

Taken so far: **session format 4** by M-1's timebase PR, for the tempo
bounds of §2.4 and the v3 → v4 migration that brings a map inside them
(§4.5). The MIDI keys of §4 (track kinds, `midi_sources`, MIDI regions)
therefore land in the next free number after it, not in 4: **session
format 5**, with **control protocol 7** and **undo window format 5**, taken
by the rest of M-1 (track kinds, MIDI regions, the timebase tag, the MIDI
operations). §4 below says "version 4" for what is format 5 on disk.
`SetInstrument`, `RemoveInstrument` and an instrument track's `instrument`
key are not in format 5: they come with hosting an instrument, in M-5, at
the next free number then. Until then an instrument track is its MIDI,
its channels and its inserts, and plays silence.

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
960,000`: 3 bpm at 48 kHz, 12 bpm at 192 kHz. Logic's tick, 1/3840 of a
whole note (960 per quarter, LP-UG glossary), lasts 25 frames at 120 bpm
and 48 kHz: recording at that resolution would move a note by up to 12
frames and fail the DAW 6 exit criterion.

Range: ticks in the file are `0 ≤ t ≤ 2^53 − 1`, about 150 years at
120 bpm.

## 2. The musical map: frames ↔ ticks ↔ bars|beats

### 2.1 The tempo map stays anchored in frames in v4

`tempo` keeps its v3 meaning: entries at frames (`at`), `bpm` a float,
`meter`. It is not reinterpreted. ADR-0014 §2 has the reasons and the
DAW 7 consequence. v4 adds two bounds (§2.4) and **one canonical
conversion**, in `common/model`, that every reader uses: the engine's plan
builder, the take resolver, the UI's ruler, counters and grid, the SMF
converter. Never the audio thread (§2.5).

### 2.2 The conversion, exactly

Notation: `R` the session rate; `Q` = 960,000; for entry *k*, `a_k` its
`at`, and `r_k` the **exact rational value** of its `bpm` float. Ticks per
frame in entry *k*: `τ_k = r_k·Q / (60·R)`. `⌊x⌉` is `⌊x + ½⌋` computed
with **floor division**, so it is defined for negative arguments too (half
rounds towards +∞).

- `T_0 = 0`; `T_{k+1} = T_k + ⌊(a_{k+1} − a_k)·τ_k⌉`. `T_k` is entry *k*'s
  tick. Derived, never stored.
- **frames → ticks:** `tick(f) = T_k + ⌊(f − a_k)·τ_k⌉`, *k* the last entry
  with `a_k ≤ f`, and entry 0 for `f < 0` (it extends backwards).
- **ticks → frames:** `frame(t) = a_k + ⌊(t − T_k) / τ_k⌉`, *k* the last
  entry with `T_k ≤ t`, entry 0 for `t < 0`.
- Each conversion counts from its own entry, so rounding never accumulates
  along the map (the v3 rule, kept).
- **Computed in integers.** `r_k = M_k / 2^{s_k}` is expanded from the
  float's bits, and the products are formed in 128-bit integers written
  portably (no `__int128`, which MSVC lacks; ADR-0008). No `float` or
  `double` is in the conversion, so GCC and MSVC, with or without FMA
  contraction, give the same tick.
- **Bounds that make 128 bits enough** (rt-engineer review, A1). With
  `1 ≤ bpm ≤ 1000` (§2.4), a binary32 in that range has `s ≤ 23` and
  `M < 1000·2^23 < 2^33`; with `|f| < 2^53` and `R ≤ 384,000`:
  - frames → ticks: numerator `|f − a_k|·M·Q < 2^54·2^33·2^20 = 2^107`,
    denominator `2^s·60·R < 2^23·2^25 = 2^48`;
  - ticks → frames: numerator `|t − T_k|·2^s·60·R < 2^54·2^48 = 2^102`,
    denominator `M·Q < 2^53`.

  Every numerator fits a signed 128-bit value and every denominator a
  64-bit one, so the division is a 128-by-64 division.
- **Narrowing is checked, never wrapped** (A2). A result outside
  `[−2^53, 2^53 − 1]` is an error the caller names: a high tempo at a low
  rate can map a frame below 2^53 to a tick above it. An operation that
  would produce one is refused; a recorded event that would is dropped and
  counted in the take report.
- **Cost** (A6). The audio thread never calls it (§2.5). On the control
  thread, 500,000 events are about a million conversions, and a portable
  128/64 division is roughly 30 to 150 ns: 30 to 150 ms, over the 10 ms
  budget. So M-1 ships:
  - a **batch API** for sorted input (`frames_to_ticks(span)`,
    `ticks_to_frames(span)`), which finds each entry once and walks;
  - a **64-bit fast path** taken when an overflow check on the reduced
    fraction shows the product fits (it covers integer and half-integer
    bpm);
  - a **benchmark on GCC and MSVC**, reported in the M-1 PR.

  M-3 designs the plan builder's **per-region flatten cache** from the
  start (§5.5), not as a fallback.

Properties the M-1 PR tests:

1. `frame(tick(f)) == f` for every frame when `τ_k ≥ 1` (ticks finer than
   frames), across entry boundaries too: `tick(a_{k+1} − 1) < T_{k+1}` when
   `τ_k ≥ 1`. Below 3 bpm at 48 kHz the identity does not hold and a
   recorded event rounds to the nearest tick; the take report says so.
2. Both are monotonic non-decreasing.
3. `tick(a_k) == T_k` and `frame(T_k) == a_k`.

### 2.3 Bars and beats from ticks

The bar rule of session-format.md ("How the map is counted") restated in
ticks: each entry carries, derived like `T_k`, its bar number and the tick
of the bar in progress. A bpm-only entry carries the count on; a meter entry
starts a bar at `T_k`; a bar it cuts short (more than half a display tick
past a bar line) is still a bar. A bar lasts `num · 3,840,000 / den` ticks,
an integer for every denominator §2.4 allows.

**One implementation.** Today the reading lives in `ui/qt` (`time_format.h`,
`musical_time`, `frame_of`, `snap_to_grid`), in `double`. It moves into
`common/model` (`MusicalMap`, built from `SessionSettings`) in the M-1 PR,
and `ui/qt` calls it. Otherwise a note at 5|1|000 in the piano roll and the
grid line at 5|1|000 in the Edit window would be two functions, and could
differ by a frame. The UI tests that pin today's reading (`ui_transport`,
`ui_edit`) pass unchanged or the PR lists every difference. *Expected: none
past one frame; unverified.*

### 2.4 Bounds on the tempo map (v4)

- `bpm` in **[1, 1000]** (A1). Below about 3 bpm a tick is longer than a
  frame at 48 kHz (§1); above 1000 the 128-bit bound of §2.2 is spent.
- Meter denominator a **power of two from 1 to 64** (A4): what an SMF
  time-signature event can express, and what makes a bar an integer number
  of ticks.
- `sample_rate` at most **384,000** (the bound §2.2's proof uses).
- `apply` (`SetSettings`) and the loader refuse a v4 value outside these,
  by key path. A v3 file is never refused for them: the v3 → v4 migration
  clamps `bpm` into the range and rounds a denominator to the nearest
  allowed power of two, reports each change at open with the old and new
  value, and the pre-migration backup keeps the original (owner question
  O7). So this migration is not "defaults only", and says so.

### 2.5 Not on the audio thread

`MusicalMap` allocates and divides in 128 bits; it is called from the
control thread, the plan builder and the UI. The plan carries frames
(§5.5). The rt-safety-auditor checks in M-3 that nothing reachable from
`Session::process()` calls it.

## 3. The model (`common/model`)

Sketches of shape; rt-engineer settles signatures in the M-1 PR (ADR-0007
§6: `common/model`'s public headers are contract files). New edge:
`common/model` → `common/midi` (ADR-0014 §1).

```cpp
namespace sonora::model {

using Ticks = std::int64_t;                 // 960,000 per quarter note
constexpr Ticks kTicksPerQuarter = 960'000;

enum class TrackKind : std::uint8_t { Audio, Midi, Instrument };
enum class Timebase : std::uint8_t { Frames, Ticks };   // a playlist's, checked by apply

// Notes and other events of one region, in data ticks. Immutable, shared
// between regions (split, duplicate, paste) and between snapshots. Since
// M-1c PR 2 a spine of canonical chunks (ADR-0015 §1; §4.4.2 below), read
// through notes(), events(), sysex(), which index and iterate in order.
struct MidiLists {                          // what a sequence is built from
    std::vector<midi::Note> notes;          // sort_notes() order
    std::vector<MidiEvent> events;          // channel messages other than notes, by tick, stable
    std::vector<MidiSysEx> sysex;           // by tick, stable
};
class MidiSequence;                         // model.h

struct MidiRegion {
    Id id;
    std::string name;
    Ticks position;                         // timeline tick of the visible window's start; >= 0
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
    std::int64_t time_reference;            // samples since midnight of the take's tick 0
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
  is in `[offset, offset + length)`; it ends at the region's end if it runs
  past it (§5.3, owner question O4). Notes outside the window stay in
  `data`, so a trim undone or dragged back out brings them back.
- **Copy-on-write sequences.** Split, duplicate and paste make regions
  that share one `data`. Editing notes through one of them gives that
  region a new `data`; the others keep theirs: independent copies, as Pro
  Tools' clips are. Aliases (Logic) would be an explicit later feature.
- Undo memory is charged as ADR-0009 §1 says. Since M-1c PR 2 a note edit
  copies only the chunks it touches and the spine above them (§4.4.2); the
  rest is shared with the step before.

### 3.1 Operations

| Operation | Undoable | Effect |
|---|---|---|
| `AddTrack` | yes | gains `kind`, and for MIDI and instrument tracks a `MidiRouting` in its mix |
| `AddMidiSource{source}` | no (pool) | a MIDI take or an imported SMF |
| `PlaceMidiRegion{playlist, region, split_right_id}` | yes | as `PlaceRegion`, overwrite included (timeline-model.md, "Overwrite"), in ticks |
| `MoveRegion`, `TrimRegion`, `SplitRegion`, `RemoveRegion` | yes | **the existing ops, now also on MIDI regions**, by region id. Each that carries a position gains a **`timebase` tag** (`frames` or `ticks`, one byte in the codec) that `apply` checks against the playlist's timebase and refuses on mismatch by name (A26): `Frames` and `Ticks` are both `int64_t`, and a sender that got it wrong would otherwise mis-scale a region by about 25 times without a word. `SetFade`, `SetCrossfade` on a MIDI region are refused by name |
| `EditMidi{region, remove_notes, add_notes, remove_events, add_events, remove_sysex, add_sysex}` | yes | removes by index into the region's current sequence, adds values, re-sorts; gives the region a new `data`. Every piano-roll and event-list edit, quantize, transpose, velocity, legato (`common/midi` `edit.h`) resolves to it |
| `SetMidiRouting{track, routing}` | no | MIDI input and output; mix state like `input` (owner question O5) |
| `SetInstrument{track, insert, plugin, params, state}`, `RemoveInstrument{track}` | yes | the instrument slot; afterwards it is an `Insert` by id, so `ReplaceInsert`, `SetInsertActive`, `SetInsertBypass`, `SetInsertParams`, `SetInsertState` apply to it unchanged. `SetInstrument` sets the track's `channels` from the instrument's main output (§6.4) |

- **A take is one compound**, labelled "Record", as ADR-0009's
  2026-10-14 update already requires: per audio track `AddSource` +
  `RecordTake`; per MIDI or instrument track `AddMidiSource` +
  `PlaceMidiRegion` (replace) or `AddMidiSource` + `EditMidi` and
  `TrimRegion` (merge). No `RecordMidiTake` operation.
- **How the resolver forms ticks** (A5). A take's region starts at
  `position = tick(f_start)`, and each event's data tick is
  **`tick(f_event) − position`**, never `tick(f_event − f_start)`: the
  second form is wrong as soon as a tempo change lies inside the take.
  With `offset = 0`, `frame(position + data_tick) == f_event` then follows
  from §2.2 property 1.
- **Resolved, so `apply` stays pure.** The engine converts before it builds
  the compound; `apply` never reads the tempo map to place a region.
- Protocol 7 (new operation kinds the engine may send, `messages.h` rule),
  the undo window format, op codec entries; `FakeEngine` and the
  conformance suite follow.
- **Settled in M-1** (`operations.h`): `SplitRegion` carries the timebase
  tag too, its `at` being a position. `EditMidi` names its playlist as
  every other region operation does. A MIDI track has `channels` 0, no
  input and no inserts, and its mix's `gain_db` and `pan` stay 0 (the file
  has no key for them). `MidiRouting` lives in `TrackMix`, so `SetMix`
  carries it as well as `SetMidiRouting`, under the same check. An
  `EditMidi` that removes and adds nothing is refused rather than made an
  empty undo step. The sequence's lists are kept by `stable_sort` after
  the removals: what was there stays before what is added at the same
  tick.
- **Settled in the PR #100 review, inside protocol 7.** Each `EditMidi`
  removal carries the value it expects with its index
  (`[index, start, length, channel, pitch, velocity, release]`, and the same
  for event and SysEx rows); a mismatch is refused by name ("note 14 is not
  the note this edit removes; the region changed under it"), so a request
  resolved against a stale replica never removes the wrong note.
  `PlaceMidiRegion` has an optional `data_from` (`{playlist, region}`): the
  new region shares that region's sequence, for duplicate, repeat, paste
  and a move to another track; inline stays for recording and import.
- The engine arms, monitors and records no audio on the engine track under
  a MIDI or instrument track (`may_arm_track`, `apply_monitoring`), and a
  closed take leaves such a file out with a notice; a member the model
  refuses costs only its own track's placement.

### 3.2 What a tempo change does

`SetSettings` with a new tempo map is unchanged and not undoable. Audio
does not move (v3 rule). MIDI regions keep their ticks, so they move in
frames, as tick-based tracks do in both vendors. The plan is rebuilt; the
swap is a "MIDI changed" swap for every MIDI and instrument track (§5.3).

## 4. Session format version 4

`sonora_session` becomes the next free number at landing: 4 is taken by
the bounds of §2.4 alone (see "Version numbers"), so "v4" below means the
format that adds the MIDI keys. Everything in v3 keeps its meaning, except
the bounds of §2.4.

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

Checked at open against the file (exists, parses, event count). A missing
or changed file flags the source and tells the operator; it never changes
what plays, because the regions hold their events (§4.4).

### 4.3 Tracks: `kind`, and keys by kind

Every track gains `"kind"` as its second key. The key set depends on it;
the strict parser refuses a key that does not belong to the kind.

| Key | audio | midi | instrument |
|---|---|---|---|
| `id`, `kind`, `name`, `monitor`, `mute`, `solo`, `solo_safe`, `active_playlist`, `playlists` | ✓ | ✓ | ✓ |
| `channels` | 1–2 | — | 1–2, the instrument's main outputs |
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
  as the backend reports it. Names, not indices: a MIDI port is a device
  that comes and goes while the session runs, and an index would point at
  another device after a replug. A named port absent at open is shown
  missing, kept, never dropped. *Which name PipeWire keeps stable across
  replugs is unverified* (ADR-0014 Unverified 2).
- `channel`: 0 means all (input) or as recorded (output), else 1–16.
- `monitor` on a MIDI or instrument track governs MIDI thru: `"auto"`
  passes input to the track's destination while it is armed, as Pro Tools'
  MIDI Thru does. It does not govern capture (§5.2).

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
  "data": {
    "file": "MIDI Sequences/9b2e…41.msq",
    "sha256": "9b2e…41",
    "bytes": 54,
    "notes": 2,
    "events": 2,
    "sysex": 0
  }
}
```

- **Session format 6 (ADR-0015 §3): `data` names the region's sequence
  file** in `MIDI Sequences/`, by the SHA-256 of its bytes: `file` is
  exactly `"MIDI Sequences/<sha256>.msq"`, `sha256` 64 lower-case hex
  digits, `bytes` its size and `notes`, `events`, `sysex` its counts, all
  checked at open (§4.4.1). Regions that share a sequence (split,
  duplicate, paste) share the file. Version 5 held the three lists inline
  in the region, in the row forms below; v5 → v6 moves them to a file at
  the first save (`session-format.md`, "Versioning").
- Unit names carry `_ticks` so no reader takes them for frames, which every
  other position in the file is.
- `notes`: `[start, length, channel, pitch, velocity, release_velocity]` in
  data ticks; channel 0–15, pitch 0–127, velocity 1–127 (0 is a note-off on
  the wire), release 0–127. In `sort_notes()` order. One line per note.
- `events`: `[tick, status, data1, data2]`, channel messages other than
  note on and off (`0xA0`–`0xEF`), data bytes 0–127, `data2` 0 for
  two-byte messages. By tick, and at one tick in stored order: that order
  is meaning (a bank select before the program change).
- `sysex`: `[tick, "f0…f7"]`, lower-case hex of the whole message, at most
  **4,096 bytes** (`kMaxSysexBytes`). Larger messages are not stored;
  import reports each one, never truncates it.
- `source`: optional, the `midi_sources` id the region came from.
- **Invariants checked at load and by `apply`:** regions sorted by
  `position_ticks` and not overlapping; `position_ticks ≥ 0` (A3, as audio
  refuses a negative position); `length_ticks > 0`; `offset_ticks ≥ 0`;
  every note's `start ≥ 0` and `length ≥ 0`; every value in range; notes
  and events sorted; every tick ≤ 2^53 − 1. A region may hold notes outside
  its window (§3).
- **No meta events.** Tempo, meter and markers belong to the session;
  text, lyrics and key signatures from an SMF are reported and not kept.

### 4.4.1 The sequence file, `.msq` (session format 6, ADR-0015 §3)

Ours, little-endian, versioned and **canonical**: one sequence has exactly
one encoding, so its hash, which names the file, is the same on every save
and every machine. `common/model`'s `encode_sequence` / `decode_sequence`
(`sequence_file.h`).

| Bytes | What |
|---|---|
| 4 | magic `SMSQ` (`53 4d 53 51`) |
| 1 | version, 1 |
| LEB128 × 3 | the note, event and SysEx counts |
| per note | start − the previous note's start (the first from 0), length, as LEB128; then channel, pitch, velocity, release velocity, one byte each |
| per event | tick − the previous event's tick, as LEB128; then status, data1, data2 (0 for the two-byte messages) |
| per SysEx | tick − the previous message's tick, and the message's size, as LEB128; then its bytes, `F0` to `F7` |

Nothing follows the last SysEx message. Every LEB128 is unsigned and
minimal, at most 2^53 − 1; each section is in the order §4.4 fixes, which
is non-decreasing in its first key, so a delta is never negative. The
decoder is bounded and never throws: it refuses a wrong magic or version, a
non-minimal or oversized LEB128, counts the remaining bytes cannot hold
(checked before anything is reserved), a tick past 2^53 − 1, a SysEx
message over 4,096 bytes, trailing bytes, and any sequence §4.4's
invariants refuse; what it accepts re-encodes to exactly the bytes it read
(a fuzz sweep holds it to that). A controller event is about 5 bytes, a
note about 10.

The example sequence of `common/model/tests/test_sequence_file.cpp` (two
notes, a bank select, a program change, a pitch bend and one SysEx) is
these 50 bytes:

```
534d5351 01 02 03 01
00 80a61d 00 24 60 40          note 0, 480000 long, ch 0, pitch 36, vel 96, rel 64
80cc3a d8e21b 01 2b 58 00      note +960000, 455000 long, ch 1, pitch 43, vel 88, rel 0
00 b0 00 01 | 00 c0 05 00 | 80ee05 e0 00 48
80a61d 06 f07e7f0901f7
```

At open each file is read once, however many regions share it, and checked
against the region's `data`: size, SHA-256, the strict decode, the counts.
One that is not there or does not verify does not stop the open (owner
decision O2): its regions keep their windows, play nothing and cannot be
edited (`EditMidi` is refused by name; moving, trimming and splitting the
window are not), a copy is placed only by reference (`data_from`), and the
next save writes the same reference back. The undo window (format 6)
carries the reference, so a replica holds the same silent regions.

### 4.4.2 Chunks, the spine and the window's chunk table (ADR-0015 §1, §2, §4)

`common/model/model.h`, M-1c PR 2.

- **A chunk** holds the notes, events and SysEx whose data tick (a note's
  start) is in `[k · S, (k + 1) · S)`, `S = kMidiChunkTicks` = 3,840,000
  (four quarters), in §4.4's orders. Empty chunks are absent. The chunking
  is a function of the contents alone: `MidiSequence(MidiLists)` and any
  chain of `EditMidi` reaching the same lists build equal spines.
- **The spine has two levels.** A leaf holds the chunks whose index is in
  `[g · 64, (g + 1) · 64)` (`kMidiLeafChunks`), with each chunk's note,
  event and SysEx counts before it in the leaf; the top holds the leaves
  with the counts before each. An index maps to its element by two binary
  searches. One level would copy 40 bytes a chunk on every drag: 72 KiB
  for a 60-minute region, past ADR-0015 Unverified 1's 64 KiB.
- **An `EditMidi`** keeps its global indices. It rebuilds the chunks its
  removals and additions fall in, the leaves those are in and the top;
  every other chunk and leaf is shared with the sequence before.
- **Undo and the budget count per chunk.** A step is charged, from fixed
  per-entry sizes (note 24 bytes, event 16, SysEx 32 and its bytes, chunk
  80, spine entry 40, leaf 48, sequence 64, each node 32 more), the chunks,
  leaves and spine it holds and the step before does not. `midi_max_entries`
  counts the entries of every distinct chunk the retained steps hold, so a
  pasted region edited in one chunk costs that chunk, not a second sequence.
- **Lists from outside** (an operation, a format-5 file, a `.msq`) are
  checked as lists before they are chunked: chunking places each element
  by its tick, which would hide an order broken across chunks.
- **Undo window format 7** carries `"chunks"` (the chunk table's size),
  `"leaves"` (each a list of chunk-table indices) and `"sequences"` (each a
  list of leaf indices, and `"missing"` as in format 6). The chunk table
  itself travels apart, in parts.
- **A part of the chunk table**: `SWCP`, version 1, then as LEB128 the
  table index of its first chunk and its chunk count, then per chunk its
  index, the size of its `.msq` encoding (§4.4.1) and that encoding. Whole
  chunks only, at most 16 MiB (`kMaxChunkPartBytes`); a chunk too large for
  a part alone is refused by name. Each part is decoded strictly on
  arrival: every chunk non-empty and inside its index's span.
- **Protocol 10** (9 is loop record's): a Snapshot is `SnapshotHeader`
  (`part_count`, `part_bytes`), the parts as `SnapshotPart`, the window as
  `Snapshot`; one with no chunk comes alone. The replica refuses a header
  over its own cap (`--max-snapshot-mib`, default 512), keeps its state
  until the window decodes, and abandons the attach at a bad part, asking
  for one Resync. A Load is `LoadPart` requests (whole `.msq` files, at
  most 16 MiB each, `SNRLPRT` v1) ahead of a `Load` carrying the bare
  session file; the engine decodes each part as it arrives, bounded by
  `midi_max_entries`, and a refused part refuses the Load with its reason.
- **On the engine** the window is encoded on a worker from a copy of the
  history taken on the document thread; what else that client is sent
  waits behind the Snapshot, and parts go out as its socket drains, two
  parts ahead at most.

### 4.5 Migration v3 → v4

Every track gains `"kind": "audio"`; the top level gains
`"midi_sources": []`; the tempo map is brought inside §2.4's bounds, each
change reported at open. The tempo part has landed as format 3 → 4
(`session-format.md`, "Versioning"; fixtures `v3-tempo-out-of-bounds.sonora`
and `v4-tempo-bounds.sonora`); the rest is the step after it. Nothing else changes. The frozen v3 fixture loads
through it, and a second v3 fixture with an out-of-range bpm and a meter of
x/3 checks the report; v4's is `v4-midi.sonora` with one track of each kind.

### 4.6 Size

About 30 bytes and 7 DOM values per note. The v3 parser caps (64 MiB,
4,000,000 values) hold about 570,000 notes and events in one session. A
dense performance with continuous controllers can reach tens of thousands
of events per track. *Unverified whether real sessions stay inside*
(ADR-0014 Unverified 6); the remedy, if not, is the plugin-state pattern:
sequences beside the file, content-addressed, a version bump.

**Measured in M-1** (`common/model/tests/measure_midi_session`, Release,
the build container on the owner's laptop, 2026-10-20). 18 instrument
tracks, 5 minutes at 120 bpm, 8 notes a second per track:

| Controller events per track | Events and notes | File | DOM values | Opens? |
|---|---|---|---|---|
| 30 a second | 162,000 + 43,200 | 8.5 MiB | 1.1 M | yes, 101 ms |
| 100 a second | 540,000 + 43,200 | 23.2 MiB | 3.0 M | **no**: the 256 MiB DOM-memory cap binds first, at about 89 bytes a value |

So the values cap is not the one that binds: the DOM-memory cap is, at
about 3 M values, and a session dense enough to reach it **saves but does
not open**. 200 note drags in one region of 50,000 controller events
charge the whole 64 MiB undo budget (each drag copies the sequence, about
800 KiB), which keeps 79 of the 202 steps, and the undo window is then
132 MiB, twice the 64 MiB a Snapshot update may carry: a replica could not
attach. Each drag costs 1.5 ms at most on the control thread.

Both are over the caps, which ADR-0014 Unverified 6 says decides
sequences beside the file. ADR-0015 decided it (accepted 2026-10-20), and
M-1c PR 1 moved the sequences beside the file as format 6.

**Measured in M-1c PR 1** (the same tool, Release, the build container on
the owner's laptop, an i7-11800H, 2026-10-20; every track's sequence its
own, so no file is shared):

| Case | Session file | Sequence files | Opens |
|---|---|---|---|
| 18 tracks, 5 min, 30 controller events a second | 19.7 KiB, 667 DOM values | 1.3 MiB in 18 | yes, 21 ms |
| same, 100 a second | 19.7 KiB, 667 DOM values | 3.5 MiB in 18 | yes, 60 ms (was: does not open) |

At the 2^24-entry design target (ADR-0015 Unverified 2; 16.78 M notes and
events as 64 tracks of 40 minutes, or 18 of 144, at 8 notes and 100
controller events a second): **100.8 MiB of sequence files** and a 69 KiB
session file. First save: encode 129 ms, SHA-256 368 ms (274 MiB/s, the
portable code, no SHA extensions), the 64 files through tmp, `fdatasync`
and rename plus one folder `fsync` 1,053 ms (18 files: 312 ms) on the
container's overlay disk, not the stick. Open, apart, best of three with
the page cache warm: read 63 ms, hash 384 ms, **decode 931 ms** (18 files:
803 ms), inside the 1 s assumed; the whole open 1.97 s, the rest being the
invariant check over every entry and building the document. Peak resident
memory 657 MiB with the saved document and the opened one both held.
`sha256_hex` alone: 292 MiB/s on 100 MiB. The undo window is unchanged by
format 6 (132 MiB for the 200 drags): the chunks of ADR-0015 §1, in M-1c
PR 2, change it.

**Measured in M-1c PR 2** (the same tool, Release, the build container on
the owner's laptop, i7-11800H, 2026-10-25; §4.4.2's chunks):

| ADR-0015 | Case | Result |
|---|---|---|
| Unverified 1 | 200 drags, region of 50,101 controller events | 9.0 KiB a drag (was about 800 KiB); 2.6 MiB of the 64 MiB budget, all 203 steps kept; window 0.2 MiB and a chunk table of 0.6 MiB; slowest drag 0.05 ms (was 1.5 ms) |
| Unverified 1 | one drag in a 60-minute region | 1,800 chunks in 29 leaves; 10.0 KiB (one level: a 70.3 KiB spine alone) |
| Unverified 7 | 2^24 entries (64 tracks) and 200 retained drags, 329 steps | document thread held 0.002 ms to copy the history; `encode_window` 206 ms on the worker; chunk table 102.3 MiB in 7 parts, window 0.92 MiB |
| Unverified 3, engine | the same, one encoded window held | peak RSS 420 MiB (the document alone 288 MiB) |
| Unverified 3, replica | the 7 parts read and decoded one at a time, then the window, in a process of its own | 1,957 ms (slowest part 176 ms; the rest the window, where `History::restore` recounts each step's charge); peak RSS 303 MiB |

Not measured here: the attach over the M7 socket (Unverified 3's time).

## 5. The real-time path

### 5.1 MIDI ports in the backend contract (`contracts/backend`)

```cpp
struct AudioBlock {
    // ... v1 fields unchanged ...
    // Written by the driver before process(), read by the callback during
    // it, valid only for that call. nullptr with count 0 means "no MIDI".
    const MidiBuffer* const* midi_inputs;  uint32_t midi_input_count;
    // Empty when process() is called; written by the callback in offset
    // order; drained and cleared by the driver after process() returns,
    // on the same thread. nullptr with count 0 means "no MIDI".
    MidiBuffer* const* midi_outputs;       uint32_t midi_output_count;
};
struct BackendConfig { /* ... */ uint32_t midi_inputs = 16, midi_outputs = 16; };

// Non-RT, control thread. Default bodies return "no MIDI", so a backend
// without MIDI compiles unchanged.
virtual std::vector<MidiPortInfo> midi_capture_ports();   // {slot, name, generation}
virtual std::vector<MidiPortInfo> midi_playback_ports();
virtual void release_midi_slot(Direction, uint32_t slot); // the engine no longer names it
virtual uint32_t midi_input_latency_frames() const;       // how late an input offset is, by design
virtual uint32_t midi_output_latency_frames() const;      // how late an output leaves, by design
```

- **Ownership, stated in the header** (A7). Input slots: the driver owns
  and writes them, the callback only reads, within `process()`. Output
  slots: the driver owns them; the callback writes them during
  `process()`; the driver drains and clears them after it returns. Both
  arrays and every buffer are allocated at `open()` and never reallocated.
  The engine treats `nullptr`/0 as a backend without MIDI. Because this is
  a change in memory ownership, ADR-0007 §6 classes it as **breaking**:
  ADR-0014 is the amendment it requires before the code (owner question
  O8). The default bodies make it source-compatible as well.
- **Slots are fixed ports; replug changes connections only** (A8).
  `drivers/jack` registers `midi_inputs + midi_outputs` JACK MIDI ports at
  `open()` and follows devices with `jack_connect` and `jack_disconnect`
  from its non-RT thread, so the callback never sees a port appear or
  disappear. A device that leaves frees its slot's connection but **not the
  slot**: the slot is reassigned only after the engine calls
  `release_midi_slot`, which it does once a plan and routing that no longer
  name the slot have been published and the previous one retired (ADR-0009
  §4.1), the never-reused rule of ADR-0012 §5.3. Without that, a stale slot
  index would play another device for a block; the generation counter
  alone does not close that window. More devices than free slots: the
  extra one is reported, not connected.
- `drivers/fake`: scripted input at given frames, captured output, both
  deterministic, so the DAW 6 exit test runs on it.
- **Windows** (ADR-0008, ASIO carries no MIDI; A10): a driver's MIDI thread
  pushes stamped messages to a **driver-internal SPSC ring**, and the driver
  fills the input slots from it on the audio thread at the start of the
  callback. No thread but the audio thread ever writes a `MidiBuffer`.
  `common/midi` `StreamParser` exists for a driver that gets raw bytes.
- The conformance suite gains MIDI checks: offsets inside the block, order
  kept, overflow counted, outputs cleared, `nullptr`/0 handled, a replug
  that never reuses an unreleased slot.

### 5.2 What the audio thread does per block, per MIDI or instrument track

**Capacity** (one rule for both reviews' note-off point, C11 and
plugin-engineer 2). Each track's `MidiBuffer`, each chain's carry buffer
(§6.2) and each `EventArea` hold **4,096 messages and 32 KiB**, of which
**2,064 entries are reserved for endings**: 16 × 128 note-offs plus 16 ×
CC 64 = 0. Starts and controllers are dropped (and counted) past the
unreserved 2,032; **a generated ending is never dropped**: if one does not
fit, the tracker keeps it pending and sends it first in the next block, at
offset 0. Chase that does not fit (16 channels of every CC, bank, program,
bend and pressure is about 2,100) is likewise carried into the next block
ahead of that block's events, counted, never lost. Sizes are placeholders
until measured (§10).

**Order, so that every push is an append** (C12). `MidiBuffer::push` is a
sorted insert, O(n) out of order. The five sources below are each sorted by
offset, and the audio thread **merges them by offset** (at equal offsets
in this order) into the buffer, so every push appends in O(1):

1. pending endings (§5.3), then generated endings;
2. chase, on the first blocks after a start or locate (§5.4);
3. playback: while rolling, the plan's events in `[playhead, playhead +
   nframes)`, through a per-track cursor (reset by binary search after a
   locate or a new epoch, as the segment cursor is). A loop end inside the
   block is §5.3's boundary at its frame;
4. thru: live input from the slots the track's routing selects, filtered
   by channel, **when the monitor rule passes input**;
5. interface notes (on-screen keyboard), from `RtCommand`s, at offset 0
   (one block of jitter, stated).

**Capture is not monitor-gated** (C15): the routed, channel-filtered input
of step 4 goes to capture (§7) **before** the monitor rule is applied,
whenever the track's capture window is open.

Then the buffer goes to its destination:

- **Instrument track:** to its chain (§6).
- **MIDI track:** to its output slot through a fixed-capacity per-port
  **delay queue** of `max(0, L_plan + L_out − L_midi_out)` frames (C9):
  `L_plan` the plan's total compensation (ADR-0012 §7.3), `L_out` the
  backend's audio playback latency, `L_midi_out` its
  `midi_output_latency_frames()`. An external synth then sounds with the
  delayed audio, neither early nor late. Queue overflow drops starts,
  counted; endings are kept.

### 5.3 No hanging notes

- **What "sounding" means** (C13). The tracker sits **after** the delay
  queue: for a MIDI track a note sounds once it has been written to the
  port, not when it entered the queue. For an instrument track it sounds
  once it has been delivered in a submit (§6.2).
- **Two trackers per destination** (C14): one for playback, one for thru
  (and interface notes). Each holds which notes sound (16 × 128 bits) and
  which channels hold CC 64.
- **Endings.** A note-off for exactly the notes still sounding, then
  CC 64 = 0 where held, at the frame of:
  - stop, locate, loop end: the **playback** tracker only, so the key the
    player is holding keeps sounding; queued playback starts in the delay
    queue are discarded first, so none goes out after its ending;
  - a plan swap that changes the track's MIDI, including a change of
    `L_plan` (a "MIDI changed" swap; the plan carries a per-track content
    generation): playback tracker;
  - the end of a region (owner question O4): playback tracker;
  - mute, a routing change, the track removed: **both** trackers;
  - a host restart, `ReplaceInsert` or deactivation of the instrument, or
    its bypass (§6.4).
- A plan swap never retriggers a note in progress (no note chase in v1).
- **The state is the session's, not the plan's** (C13). Trackers, sustain
  bits, pending endings, delay queues and chase slots live in
  `engine/core`, indexed by engine track or port slot, sized at session
  construction. A swap therefore never forgets which notes sound.

### 5.4 Chase

At play and locate, the control thread (not RT) computes per track the
controller state at the start frame from the plan: bank and program, every
CC's last value, pitch bend, channel pressure, from the regions that cover
the start, in that order. It writes it into a free chase slot (two per
track) and names the slot in the play or locate `RtCommand`.

- **Slots are retired by epoch** (C17): a slot is free again only after
  the epoch of the command that named it has been retired (ADR-0009 §4.1).
  If both slots are busy, the command carries no chase, and that is
  counted. The control thread never waits.
- The audio thread emits the chase at offset 0 of the first block (carried
  over if it does not fit, §5.2).
- The plan carries the chase at the loop start, so a loop wrap needs no
  command.
- After a host restart the current chase is re-sent with the first submit
  (§6.4).

### 5.5 The plan

The builder flattens each MIDI or instrument track's active playlist into
frames with §2.2's batch API. Muted regions give nothing.

- **How the builder forms frames** (A5): a note's on is at
  `frame(position + start − offset)` and its off at
  **`frame(position + start + length − offset)`**, clipped to
  `frame(position + length)` at the region's end; never `frame(on) +
  duration`, which drifts as soon as a tempo change lies under the note.
- **Same-frame order** (C16): note-offs, then non-note channel messages in
  stored order, then note-ons. A note-on never precedes the program change
  or bank select at its tick.

```cpp
struct PlanMidiEvent { int64_t frame; uint32_t sysex_pos; uint8_t size; uint8_t data[3]; };  // 16 bytes
// size 1..3: a short message in data. size == kPlanSysEx (0xFF): a SysEx
// whose length (u16) and bytes are at sysex_pos in the plan's byte pool (C18).
struct PlanMidiTrack { uint32_t engine_track; uint32_t first_event, event_count;
                       uint32_t generation; uint32_t loop_chase; };
```

- In the plan's one allocation, with the SysEx byte pool. Sorted by frame,
  then by the same-frame order.
- **Per-region flatten cache** (A6): the builder keeps each region's
  flattened events keyed by (region id, sequence, tempo map revision) on
  the control thread, so a note edit re-flattens one region, not the
  session. Designed in M-3 from the start.
- **No residency gate for MIDI**: the events are in the plan, in memory.
  The audio gate of ADR-0009 §4.4 still governs the swap.
- Fixed maxima at session creation: MIDI events per plan, SysEx bytes per
  plan. The builder refuses past them and names the limit (ADR-0009 §4.5).

### 5.6 Counting what was lost (D19)

`MidiBuffer::dropped()` is not atomic and stays private to its thread. The
audio thread keeps its own atomic counters per track and per port slot,
published by the state publisher. Each kind is named: **MIDI lost** (a
capture ring overrun: part of a take), **dropped to a plugin** (event area
or carry buffer), **dropped to a port** (track buffer or delay queue full,
or the driver's write refused, as `jack_midi_event_write` returns ENOBUFS),
**chase skipped or late**, **plan over maxima** (control thread). They
reach clients as new `TrackState` fields, which is a **state layout bump**
(the next free), plus `EngineEvent` kinds for the one-off ones (plan over
maxima, take report). M-3 adds the port and plugin counters, M-4 the
capture one.

## 6. Instruments (amends ADR-0012, Proposed with ADR-0014)

### 6.1 The chain

- An instrument track's chain is **one host**: the instrument, then the
  track's inserts, at most 11 instances (inside `kMaxInstances` = 16). The
  instrument has its own slot ahead of A–J (owner question O6).
- **Events go to the instrument only** (plugin-engineer 4). Inserts A–J get
  an empty `atom:Sequence` or `IEventList`. A plugin with an event input
  and no audio input is refused in A–J, so an instrument cannot sit in an
  insert slot with silence above it by a side door. MIDI-controlled effects
  with their own event input are a later amendment.
- The chain takes **0 audio inputs** and gives the track's channels.
  ADR-0012 §7.1's "the chain's channel count equals the track's, in and
  out" becomes "out equals the track's; in equals the track's, or 0 for an
  instrument chain".
- Plan: `TrackSource` of an instrument track renders no audio and hands its
  `MidiBuffer` to the `InsertChain` node. Misses, fades and PDC are
  ADR-0012's, unchanged. Live input through an instrument on an armed
  track is heard at the **plan's total compensation** (every path is
  aligned on the slowest), not only at the instrument's own latency plus
  the IPC block. Hearing less needs DAW 4's low-latency monitoring item
  (ADR-0012 §1.5), which excludes a monitoring track from alignment.

### 6.2 Events are never lost on a skipped submit (plugin-engineer 1)

Under ADR-0012 §7.2 step 2, when the host is still busy the submit is
skipped and nothing is written. For audio that is a miss; for events it
would lose the block's MIDI, and a lost note-off hangs inside the plugin,
where §5.3 cannot see it. So, as parameters already are held:

- Each chain has an engine-private **carry buffer** (capacity as §5.2). A
  block's events that were not submitted go there, and the next submit
  delivers them first, **at offset 0**: late, never lost.
- Carry overflow drops note-ons first, and keeps note-offs and CC 64 = 0.
  If an ending still cannot be kept, the chain is marked, and its next
  submit begins with CC 123 (All Notes Off) and CC 64 = 0 on every channel
  it has sounded since the last clean state.
- The instrument's tracker (§5.3) counts a note as sounding only once it
  was delivered in a submit.
- M-5 regression test: a host stalled for 3 blocks with a note-off inside
  the stall produces no hanging note.

### 6.3 `contracts/plugin` layout 2 (plugin-engineer 3)

- **Separate channel counts.** `make_layout` takes `in_channels` and
  `out_channels` (0 and 1–2 for an instrument chain) instead of one
  `channels`.
- **An event area** per channel region, written by the audio thread with
  the input planes before `submitted_block` is published (release), read by
  the host after its acquire:

  ```cpp
  struct EventEntry { uint32_t offset; uint16_t size; uint16_t pad; uint32_t byte_pos; };
  struct EventArea  { uint32_t count; uint32_t bytes;
                      EventEntry entries[kMaxEventsPerBlock];    // 4,096
                      uint8_t data[kMaxEventBytesPerBlock]; };   // 32 KiB
  ```

  Its offset and capacity are in `ChannelHeader` and checked by
  `header_valid`; the engine still uses its own private `Layout` and never
  reads an address or a count back from the region. No `dropped` field:
  the region is untrusted, so the engine keeps its own count (§5.6).
- **Order:** entries by offset; at one offset, note-offs, then other
  channel messages, then note-ons (§5.5's rule). `atom:Sequence` requires
  non-decreasing times and VST3 plugins assume it.
- **The host trusts nothing in it.** It copies `count`, `bytes` and every
  entry into private memory once, then checks `count ≤ kMaxEventsPerBlock`,
  `offset < submitted_frames`, offsets non-decreasing, and `byte_pos + size
  ≤ bytes ≤ kMaxEventBytesPerBlock`. A violation is a fault: the host exits
  `kHostExitFaulted`.
- `kLayoutVersion` and the size asserts move together; a mismatched host
  exits 3 (ADR-0012 §5.5). Owners: rt-engineer and plugin-engineer.
- **Transport** (plugin-engineer 7). No transport reaches plugins today.
  Instruments with tempo-synced LFOs or arpeggiators, and VST3 plugins that
  read `ProcessData::processContext`, need tempo, time signature, the
  playing flag and the position in frames and in quarters. The audio
  thread writes them into the **clock region** (`kClockLayoutVersion` →
  next) once per block, from a per-plan tempo table the builder computes
  with `MusicalMap` (per entry: start frame, start quarter, bpm, meter,
  quarters per frame in 64-bit fixed point). That is a lookup by cursor, a
  multiply and an add: no 128-bit division on the audio thread (§2.5).
  Display-grade precision is enough there; the events themselves are
  placed by the plan's exact frames. LV2 gets it as
  `time:Position` on the same atom port, VST3 as `ProcessContext`. In
  M-5's scope.

### 6.4 Delivery in each format, channels, restarts

- **LV2** (plugin-engineer 5): the input port is an `atom:AtomPort` with
  `atom:bufferType atom:Sequence` and `atom:supports midi:MidiEvent`,
  times in frames (`atom:frameTime`). The buffer is max(`rsz:minimumSize`,
  the event area's size); the sequence header is reset before every
  `run()`; URIDs through `urid:map`, a required feature for instruments.
  LV2 ports cannot stay unconnected, so an instrument's audio *inputs*, if
  any, are connected to a zero buffer. SysEx passes unchanged as
  `midi:MidiEvent`.
- **VST3** (plugin-engineer 6): note-on, note-off and **poly** pressure go
  in `IEventList`. CC, pitch bend (`kPitchBend`), **channel** pressure
  (`kAfterTouch`) and program change (`kCtrlProgramChange`) go through the
  plugin's `IMidiMapping` as `IParameterChanges` points at their offsets,
  straight to the processor: never through ADR-0012's dense parameter ring,
  last-value table or automation. Without `IMidiMapping` a plugin's CCs are
  dropped and counted, and the catalog shows it. A note-on with velocity 0
  becomes a note-off; velocity is scaled to v/127; release velocity goes in
  `NoteOffEvent::velocity`. SysEx goes as a `DataEvent` (`kMidiSysEx`).
  **Note IDs:** the host assigns a fresh `noteId` per (channel, pitch) on
  note-on from a counter in a private 16 × 128 table and repeats it on the
  matching off, never −1, which keeps note expression open without a
  layout change. Validation requires `getBusCount(kEvent, kInput) > 0`,
  activates that bus and the main audio output bus only.
- **yabridge** (plugin-engineer 8): no new mechanism; bridged VST3
  instruments get the same `IEventList` and `IMidiMapping` in the same
  `process()`. They pay a Wine round trip on top, so they are the likeliest
  to miss at quantum 128: Unverified 4 includes one, its miss count the
  figure. Nothing touches licence checks (ADR-0004).
- **Channels.** Only the main output bus is used, 1 or 2 channels; for
  VST3 `setBusArrangements` is tried for the track's layout first. An
  instrument whose main output is not 1 or 2 after negotiation is refused
  at validation, saying why. Auxiliary outputs are deactivated in v1 and
  the scan records them; routing them to tracks is a later amendment with
  its own node shape. Declared outputs are checked against actual, as for
  every chain. `SetInstrument` sets the track's `channels`.
- **Restarts.** A host restart (new region, ADR-0012 §5.3), `ReplaceInsert`
  on the instrument, or a deactivate and reactivate means the plugin has
  lost its controller state and its sounding notes: §5.3's trackers for
  that destination are cleared and the current chase is re-sent with the
  first submit. **Bypassing the instrument** (0 inputs, so bypass is
  silence) sends the endings first.
- **Catalog** (plugin-engineer, catalog): each entry gains `category`
  (Instrument or Effect, from `lv2:InstrumentPlugin` or the VST3
  `PClassInfo2` subcategory; display only, never trusted),
  `has_event_input` (what validation checks), `main_out_channels`,
  `aux_out_buses`, `has_midi_mapping` (VST3) and `wants_transport`. The
  instrument picker filters on `has_event_input` and 1–2 main outputs. The
  scan record changes, so the catalog version moves.
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
  period, Unverified 1); `midi_input_offset`: a session setting, default 0,
  for a device's own delay.
- The sum is recorded per take as `latency_compensation`, as audio does,
  so a wrong figure can be corrected later.
- **Before the timeline start** (A3): a position below 0 (a note played in
  the first milliseconds of a take recorded from 0, or during pre-roll from
  0) is placed at 0, and the take report counts it. The take file keeps the
  true frame (§7.2), so nothing recorded is lost.
- On `drivers/fake` every term is exact, which is what the exit test needs.

### 7.2 Capture, boundaries, files

- **The ring.** The audio thread pushes `{timeline_frame, track, size,
  bytes[3]}` (16 bytes) per captured message onto one SPSC capture ring per
  session, SysEx bytes through a byte ring beside it. A SysEx goes onto
  **both rings or neither**: the audio thread checks free space on both
  first (E20). **A full ring is an overrun: counted as MIDI lost, never a
  wait** (CLAUDE.md).
- **Reserved space** (E20). The ring keeps room, never used by ordinary
  events, for every armed track's boundary records and its closing endings
  (16 × 128 + 16 per track), so a full ring cannot eat a window's close.
- **Sizing** (E20) from a stated worst case, as the audio ring is: 16 ports
  × the fastest input rate × the disk-stall allowance of the audio capture
  ring. DIN MIDI is at most about 1,000 three-byte messages a second;
  USB MIDI has no such limit, and 10,000 a second per port is the
  placeholder until measured.
- **Windows of capture** open and close with the in-band boundary records
  of timeline-model.md ("Closing a take while rolling"), with an event
  count instead of a frame count. Punch: MIDI is captured in `[A, B)`
  exactly, without the audio's handle. At a window's close the audio
  thread pushes endings for the notes the track's capture still holds; and
  the **disk thread** also ends any note still open when it writes the end
  of track, because those endings can themselves have been dropped.
- **Take files are SMFs whose tick is one frame.** Format 0, one `MTrk`, no
  running status (E22); division and tempo chosen so `ppq · 10⁶ / tempo =
  rate`:

  | Rate | Division (ppq) | Tempo (µs/quarter) |
  |---|---|---|
  | 44,100 / 88,200 / 176,400 | 22,050 | 500,000 / 250,000 / 125,000 |
  | 48,000 / 96,000 / 192,000 | 24,000 | 500,000 / 250,000 / 125,000 |

  Any SMF reader puts every event at its true time.
- **The header event** (E23): a text meta event (`FF 01`) at tick 0, which
  the **disk thread** writes when the opening boundary record arrives (the
  start frame is not known when the control thread opens the file):

  ```
  sonora-take v=1 take=<n> track=<id> rate=<R> origin=<samples>
    start=<timeline frame of tick 0> time_reference=<samples since midnight>
    latency_compensation=<frames> session=<name, percent-encoded UTF-8>
  ```

  One line, single spaces, keys in this order, integers in decimal,
  `session` last and percent-encoded (`%20` for a space, `%25` for `%`) so
  no name can break the grammar. Event ticks are compensated timeline
  frames minus `start`, so the file places the take with nothing else, as a
  BWF's `bext` does; `latency_compensation` lets a wrong figure be undone.
- **Long silences** (E21). A delta time is at most `max_vlq` = 2^28 − 1
  ticks: 93 min at 48 kHz, 23 min at 192 kHz. The disk thread writes an
  empty text event (`FF 01 00`) whenever the next delta would exceed
  `max_vlq`, so a silent armed track in a long live take never fails with
  `DeltaTooLarge`.
- **Checkpoints, in this order** (E22): write the events; `fdatasync`;
  then patch the `MTrk` length to an **event boundary** at or before what
  was synced; the patched length becomes durable at the next sync. A
  durable length never points past durable bytes. The schedule is the
  audio one (`sync_seconds`).
- **Recovery** of an unfinished take (E22, plugin-engineer): parse from the
  start to the last **complete** event, truncate any partial tail (a
  half-written delta or event), end the open notes, append the end of
  track, fix the length. It runs before anything calls `read_smf` (which
  refuses a file without an end of track). That is the one write to an
  unfinished file the format allows, as for audio. Orphans are offered for
  placement from their header event.
- **The writer** is a streaming SMF writer in `common/midi` that produces
  bytes and does no I/O (ADR-0008); `engine/core`'s disk thread writes them
  through `common/platform`. At close the disk thread reports "MIDI take
  closed, N events", and `engine/control` reads the file back with
  `read_smf` and builds the take compound (§3.1). Hence two edges,
  `engine/core` → `common/midi` and `engine/control` → `common/midi`
  (ADR-0014 §1). The engine reading **its own** take files with the
  bounded, non-throwing codec is not an ADR-0007 §3 interchange parse; an
  SMF from outside is (§8).
- The region holds what is on disk, not what the audio thread meant to
  write.
- A take with no events adds its source to the pool and places no region.
- **Record modes** (DAW 6 roadmap): *replace* overwrites (the default,
  owner question O1); *merge* adds the take's notes to the regions it
  covers and extends them; *cycle* makes one take per pass on DAW 3's
  playlists and take lanes.

## 8. SMF import and export

In `interchange/` (ADR-0007 §1 lists SMF there), with `common/midi`'s
codec, run inside `sonora-convert`, because an SMF from outside is
untrusted input and ADR-0007 §3 parses those in their own process.

**Import** copies the file into `MIDI Files/`, adds a `MidiSource`
(`imported`), and makes one region per `MTrk` that holds channel messages,
on new MIDI or instrument tracks or on chosen ones. The report states, and
the tests cover:

- running status expanded; a note-on with velocity 0 is a note-off;
- overlapping notes of one pitch and channel paired first-in, first-out,
  as `common/midi` does; a note unterminated at its track's end ends there
  and is reported;
- unknown chunks skipped, as the SMF spec requires; variable-length
  quantities limited to 4 bytes;
- **format 2** imported one pattern (`MTrk`) per region, placed end to
  end, and the report says so;
- SysEx: `F0` and `F7` continuation packets joined; a message over 4 KiB
  reported and left out, never truncated;
- an **RMID** (RIFF-wrapped) file unwrapped, and the report says so;
- ticks scaled from the file's division to 960,000 exactly where the
  division divides it (24, 48, 96, 120, 192, 240, 384, 480, 960, 1,920,
  3,840 and 7,680 all do); otherwise rounded, and the report counts the
  moved events. An SMPTE division converts through seconds to frames and
  then §2.2, with any rounding stated;
- the file's tempo and meter are offered to replace the session's, never
  applied silently, and brought inside §2.4's bounds with a report.

**Export** writes format 1: a conductor `MTrk` (tempo as µs per quarter,
rounded from `bpm` and reported where not exact; meters; markers), then one
`MTrk` per track, named with `FF 03`. Notes end with **`0x8n` note-offs
carrying the release velocity**, not note-on with velocity 0, so release
velocity survives the round trip. Division **960** by default, 1–32,767 by
choice (owner question O2). A tick off the export grid rounds to the
nearest, and the report gives the count.

**The round trip, stated honestly.** An SMF's division is at most 32,767
per quarter, so an unquantized performance at frame accuracy cannot pass
through one unchanged. What can be tested, and what O2 proposes as the exit
criterion: export → import → export is byte-identical; every note on the
export grid comes back exactly; every other note comes back within half an
export tick, counted in the report.

## 9. MIDI 2.0, UMP and MPE

v4 is MIDI 1.0 throughout: bytes in `MidiBuffer`, 7-bit values in the
model. Not ruled out by anything here:

- UMP on the wire would be a driver concern; JACK MIDI ports carry MIDI 1.0
  bytes. *Whether PipeWire hands UMP devices to JACK clients as MIDI 1.0
  is unverified* (ADR-0014 Unverified 3).
- Higher-resolution velocity and controllers would be a format version
  whose migration upscales 7-bit values by the published MIDI 2.0
  bit-scaling rule: a filled default.
- **MPE** is MIDI 1.0 on several channels, which the `channel` field
  carries, so it needs nothing new for **external ports and LV2**. It is
  not free for **VST3** (plugin-engineer 6): there per-channel bend and
  pressure reach a plugin only if it maps all 16 channels in
  `IMidiMapping`, and real MPE means translating to
  `INoteExpressionController`. That translation is later work, outside
  layout 2; the note IDs of §6.4 keep it possible.

## 10. Open questions

- ADR-0014 Unverified 1–8, each with its measurement and owner.
- Every capacity here is a placeholder until measured: 4,096 messages and
  32 KiB per block per track and per event area (2,064 reserved for
  endings), the SysEx cap (4 KiB), the capture ring's 10,000 messages per
  second per USB port.
- Whether a MIDI track may drive an instrument track (Pro Tools' common
  layering): a later `midi_output.track` key, a version bump.
- Sample-based MIDI tracks (events fixed in frames when the tempo changes,
  Pro Tools' sample timebase): a later `timebase` key, a version bump.
