# Native session format

Status: design, written before the code (CLAUDE.md, ROADMAP Phase 2). Nothing
here is implemented. The reader and writer live in `common/model` (ADR-0007
§1, M8). The in-memory model they load into is in
[`timeline-model.md`](timeline-model.md); the architectural decisions behind
both are in [ADR-0009](../decisions/ADR-0009-timeline-snapshots-and-plan-swap.md).

## What a session is on disk

```
<Session Name>/
  <Session Name>.sonora          the session file (this spec)
  Audio Files/                   every source, one BWF per take per track
  Session File Backups/          rotated saves, autosaves, pre-migration copies
  Plugin State/                  plugin state blobs, content-addressed (version 2, ADR-0012)
```

The folder is the unit. It is what goes on the persistence partition, what
gets copied to another machine, and what `sonora-convert` reads and writes
(ADR-0007 §4.5). Every path inside the session file is relative to the
folder, so moving the folder moves the session.

Two rules govern the relationship between the file and the audio:

1. **A closed audio file is never modified.** Not by a save, not by an edit,
   not by undo. Moving a region does not rewrite the file's `bext`. The one
   exception is recovery, which gives a take whose header was never
   finalised the length on disk (its RIFF and data sizes, a trailing part of
   a frame cut, the pad byte added), and only that.
2. **Undo never deletes audio.** Sources are an append-only pool (see
   "Sources"). A take that is undone stays in `Audio Files/` and in the
   session's source list, marked unused. Removing unused audio is an explicit
   operator action with a confirmation, never a side effect.

Losing a take is the worst outcome in the product. These two rules mean that
nothing the session file does, including being corrupted, can destroy one.

## Encoding: canonical JSON

UTF-8, no BOM, LF line endings, a final newline. Two-space indent, one key per
line, keys in the order this spec lists them, arrays of scalars on one line.
The writer is canonical: the same model always produces the same bytes. A
save with no change is a byte-identical file, and `git diff` on two saves
shows exactly the edit.

Numbers, the place JSON usually goes wrong:

- **Every position, length and offset is an integer count of samples at the
  session rate.** Never seconds, never timecode strings, never floats. A
  float position cannot round-trip a sample exactly past about 2^24 samples
  (six minutes at 48 kHz in single precision), and a one-sample error between
  two mics is a comb filter. For MIDI (version 5), the count is of ticks at
  960,000 per quarter note instead, and every such key ends in `_ticks`
  (`midi.md` §4.4), so no reader takes one for frames.
- Integers are in `0 ≤ n ≤ 2^53 − 1`, so `jq`, Python and JavaScript read them
  exactly. At 192 kHz that is over 1,400 years of timeline. The reader rejects
  anything outside, any fraction, and any exponent on an integer field.
- **Floats appear only for gain, pan and tempo.** Gain is in dB, pan in
  −1…+1. They are written with `std::to_chars` (shortest round-trip form for
  `float`) and read with `std::from_chars` **straight into `float`**, never
  through `double`, which would round twice. Both ignore the C locale, so a
  machine set to `de_DE` reads `-2.5` as −2.5, not as −2. A value read and
  written back is bit-identical. Gain at or below −144 dB means silence;
  JSON has no −∞.
- Integers are parsed from their lexeme with `std::from_chars` into
  `int64_t`, never through a floating-point value.

The parser is strict: unknown keys, duplicate keys, missing required keys,
wrong types, trailing commas and comments are all errors, reported with the
line and the key path (`tracks[3].playlists[0].regions[12].length`). A file
is capped at 64 MiB, 64 levels of nesting, 4,000,000 values in the DOM, and
256 MiB of DOM memory as the parser accounts it; past any cap it fails with a
message naming the cap. A corrupt or hostile file therefore cannot exhaust
memory or stack. A real session is far inside: 18 tracks of 2,000 regions is
about 15 MB and under 1,000,000 values.

**Key order on read:** any order is accepted, except that `sonora_session`
must be the first key of the top-level object (it is the magic). The set of
keys is strict; their order is not, so a file that went through `jq` or a
hand edit still loads. The writer always emits the canonical order, so the
next save restores it. The keymap
loader already set this standard for error messages (`keymaps/README.md`);
the session parser shares its code, in `common/json` (ADR-0009 §5), which
is its own module and not part of `common/base`, because `common/base` must
stay RT-safe and a DOM allocates.

Why JSON, briefly (the full comparison is in ADR-0009): it is diffable, a
strict parser is small enough to own (ADR-0002 froze the package set, and
the keymap parser is about two hundred lines), and every tool an engineer
might use to inspect a broken session can read it. XML was rejected for the
same parser cost with no gain. SQLite was rejected because it is not
diffable, it is a new dependency, and its main advantage, incremental
writes, does not matter at this size (see "Size").

## Top level

```json
{
  "sonora_session": 2,
  "written_by": "sonora-engine 0.2.0",
  "save_generation": 42,
  "name": "Vocals Friday",
  "sample_rate": 48000,
  "audio_format": "int24",
  "timecode": { ... },
  "tempo": [ ... ],
  "transport": { ... },
  "monitor_bus": { ... },
  "sources": [ ... ],
  "midi_sources": [ ... ],
  "tracks": [ ... ],
  "markers": [ ... ]
}
```

`midi_sources` (version 5): MIDI takes and imported SMFs, in the same id
space as `sources` ([`midi.md`](midi.md) §4.2).

| Key | Type | Meaning |
|---|---|---|
| `sonora_session` | integer | Format version, and the magic: a file without this key as the first key is not a session file. See "Versioning". |
| `written_by` | string | Program and version that wrote it. Diagnostic only; no logic reads it. |
| `save_generation` | integer | Incremented by every write (main file or autosave) whose content differs from the last bytes written. It, not the file's mtime, decides whether an autosave is newer than the main file: an appliance without a network may have no correct clock, copying a folder rewrites mtimes, and FAT keeps them to 2 s. A save with no change writes nothing, so canonical bytes still hold. |
| `name` | string | Display name. The file name is the folder's business. |
| `sample_rate` | integer | Session rate. Every source must match it. A backend at another rate is a start-up failure (ADR-0003), not a resample. |
| `audio_format` | `"int24"` \| `"float32"` | Format of new recordings. Existing sources keep their own. |

### `timecode`: where sample 0 of the timeline sits in the day

```json
"timecode": { "origin": 172800000, "fps": "25" }
```

- `origin`: the timeline's sample 0 as samples since midnight, the same
  domain as BWF `TimeReference`. Integer, so it is exact at any frame rate.
- `fps`: `"23.976"`, `"24"`, `"25"`, `"29.97"`, `"29.97df"`, `"30"`. Display
  and interchange only. No position in the file depends on it.
- **Default origin for a new session: 01:00:00:00** (172,800,000 at 48 kHz).
  This is the broadcast convention. It also means a take recorded at the very
  start of the timeline, whose first sample sits before sample 0 once input
  latency is compensated, still has a positive `TimeReference` and never wraps
  past midnight.

Timeline positions everywhere else in the file are relative to `origin`.
Changing the session start time changes one field and moves nothing.

### `tempo`

```json
"tempo": [ { "at": 0, "bpm": 120.0, "meter": [4, 4] } ]
```

At least one entry, the first at 0, sorted, no duplicates. Phase 2 uses it
for the grid and the click only. **Audio regions are anchored in samples and
never move when the tempo changes.** Musical-time anchoring (for MIDI, later)
is a separate field added with a version bump, not a reinterpretation of
this one. `bpm` is the one other float in the file.

Bounds (version 4, ADR-0014 §2, `midi.md` §2.4): `bpm` from 1 to 1000; a
meter of at most 65535 beats, its denominator a power of two from 1 to 64;
`sample_rate` at most 384,000. Inside them the conversion between frames,
ticks and bars|beats (`MusicalMap` in `common/model`, one function for every
reader) is exact in 128-bit integers; outside them it could not be. The
loader and `SetSettings` refuse a value outside by key path; a version 3
file is brought inside them instead ("Versioning").

How the map is counted. The interface's counters, ruler and grid read it
this way, and the click must put its downbeats in the same places:

- `bpm` counts quarter notes, whatever the meter. A beat is the meter's
  denominator note, so a 6/8 bar at 120 bpm holds six eighths of 0.25 s.
- An entry's tempo and meter hold from its `at` to the next entry's `at`.
- An entry that changes only `bpm` carries the count on: the bar and beat
  in progress continue at the new tempo, so a bar may straddle the change.
- An entry that changes `meter` starts a new bar at its `at`. If that
  falls inside a bar (more than half a tick, 1/1920 of a beat, past a bar
  line), the bar it cuts short is still counted as a bar, and the new
  meter's first bar is the next number. Within half a tick of a bar line,
  the entry is on that line.
- Each entry counts from its own `at` frame, so rounding does not
  accumulate along the map.

### `transport`

```json
"transport": {
  "loop": [0, 0],
  "punch": [0, 0],
  "pre_roll": 96000,
  "post_roll": 48000,
  "punch_crossfade": 240
}
```

`[0, 0]` means unset. `punch_crossfade` is the length of the crossfade placed
at each end of a punched take (default 240, 5 ms at 48 kHz, equal-power,
centred; see `timeline-model.md`, "Punch in"). Transport settings are saved
but are not undoable.

### `monitor_bus`: Phase 2's fixed stereo bus

Decided by the owner, 2026-10-12: Phase 2 playback starts with a fixed
2-channel monitor bus. Inserts, sends, VCA and groups come later, on top of
the same processing graph.

```json
"monitor_bus": {
  "outputs": [0, 1],
  "gain_db": 0.0,
  "pan_law_db": -3.0
}
```

- `outputs`: playback channel indices, in the natural order of the
  interface's physical playback ports, the same ordering rule as capture
  (`port_order`). **Indices, not port names**: names differ between machines,
  and the image must boot the same way on hardware we have never seen. Out of
  range for the attached interface is reported at session open, and the bus
  falls back to 0 and 1 with the warning shown, not hidden.
- `pan_law_db`: constant-power, −3 dB at centre. It is in the file because it
  changes the mix. A session moved to a machine with a different default must
  sound the same. From −12 to 0; a positive law would be infinite gain at a
  hard pan.
- Every `gain_db` in the file (bus, track, region) is from −144 to +24 dB,
  and a file outside that is refused. The audio thread multiplies by the
  linear gain without checking it, so the range is what keeps infinity, NaN
  and subnormal coefficients off it.

Every track feeds this bus. There is no routing key in Phase 2 on purpose: a
field that can hold only one value is a field the Phase 3 migration has to
reinterpret. Phase 3 replaces `monitor_bus` with a `mixer` object, and the
migration is mechanical: the bus becomes the main output bus, and every track
gets an explicit output to it (see "Versioning", the v1→v2 example).

### `sources`: the append-only pool

```json
"sources": [
  {
    "id": 7,
    "path": "Audio Files/Audio 3_02.wav",
    "channels": 1,
    "length": 2880000,
    "format": "int24",
    "time_reference": 175200000,
    "umid": "060a2b340101010501010d4313000000a1b2c3d4e5f60718293a4b5c6d7e8f90",
    "kind": "recorded",
    "take": 2,
    "recorded_on_track": 3,
    "latency_compensation": 312
  }
]
```

| Key | Meaning |
|---|---|
| `id` | Session-unique integer. Shared id space with tracks, playlists, regions, markers. |
| `path` | Relative to the session folder, `/`-separated, no `..`, no absolute path. Imported audio is copied into `Audio Files/` first; a session never points outside its own folder, because the folder must survive being moved to another stick. |
| `channels`, `length`, `format` | As read from the file's `fmt ` and `data` chunks when the take closed or the file was imported. Checked against the file at open (see "Opening"). |
| `time_reference` | The file's BWF `TimeReference`, copied. Samples since midnight of the file's first frame. |
| `umid` | 32-byte SMPTE 330M basic UMID, hex. Generated when the take is created; also written into the file's `bext` (version 1). This is the source's identity for AAF (Phase 4): it becomes the source mob's MobID. Optional in version 1 of the format, because Phase 2's writer may not yet produce it. |
| `kind` | `"recorded"` or `"imported"`. |
| `take`, `recorded_on_track` | For recorded sources: the take number and the track id it was recorded on. Kept for comping (Phase 3) and for recovery. |
| `latency_compensation` | Samples by which the source was shifted earlier on the timeline to undo the round-trip latency at record time. Recorded so the operator can see it, and so a wrong figure can be corrected later. |

Sources are not undoable (`timeline-model.md`, "What undo covers"). A source
not referenced by any region in any playlist is *unused*. It stays.

### `tracks`

```json
"tracks": [
  {
    "id": 3,
    "name": "Lead Vox",
    "channels": 1,
    "input": [2],
    "monitor": "auto",
    "gain_db": -2.5,
    "pan": 0.0,
    "mute": false,
    "solo": false,
    "solo_safe": false,
    "active_playlist": 11,
    "inserts": [ ... ],
    "playlists": [
      {
        "id": 11,
        "name": "Lead Vox.01",
        "regions": [ ... ],
        "crossfades": [ ... ]
      }
    ]
  }
]
```

- Array order is display order.
- `channels` is 1 or 2 in Phase 2. `input` lists one capture channel index
  per track channel, natural port order.
- `monitor`: `"auto"` (input while armed and stopped or punching, playback
  otherwise), `"input"`, `"off"`.
- `gain_db`, `pan`, `mute`, `solo`, `solo_safe` are mix state. They are
  saved, they are not undoable, and at run time they are atomics, not part
  of the plan (`timeline-model.md`, "The monitor bus plan").
- `solo_safe` (version 3): another track's solo never mutes this one, as
  Pro Tools' solo safe; its own `mute` still does.
- A track always has at least one playlist, and `active_playlist` names one
  of them. Only the active playlist plays. The others exist for comping
  (Phase 3); the format carries them from the start so the Phase 3 change is
  behaviour, not format.
- `inserts` (version 2): the track's plugin inserts. See "Inserts".
- `kind` (version 5, second key): `"audio"`, `"midi"` or `"instrument"`.
  The example above is an audio track. The key set depends on the kind,
  strictly: a MIDI track has no `channels`, `input`, `gain_db`, `pan` or
  `inserts`, and has `midi_input` and `midi_output`; an instrument track has
  `midi_input` instead of `input`. A MIDI or instrument track's playlists
  hold MIDI regions, in ticks, with no `crossfades` key. All of it, and the
  top-level `midi_sources` after `sources`, is in [`midi.md`](midi.md) §4.

### Inserts

Version 2, [ADR-0012](../decisions/ADR-0012-plugin-hosting-m9.md) §6.
Ordered by `slot`. Empty for a track with none, never absent.

```json
"inserts": [
  {
    "id": 41,
    "slot": 0,
    "plugin": { "format": "lv2", "id": "http://gareus.org/oss/lv2/dpl#mono",
                "name": "x42-dpl Digital Peak Limiter Mono", "version": "0.9.2" },
    "active": true,
    "bypass": false,
    "params": [ { "key": "threshold", "value": -1.0 } ],
    "state": { "file": "Plugin State/3f1c…e9.state", "sha256": "3f1c…e9", "bytes": 812 }
  }
]
```

| Key | Meaning |
|---|---|
| `id` | Session-unique, from the shared id space. |
| `slot` | 0 to 9, Pro Tools' A to J. Unique on the track; gaps are allowed. Signal flows in slot order, **pre-fader**. |
| `plugin.format` | `"lv2"` or `"vst3"`. Anything else is refused. |
| `plugin.id` | The LV2 plugin URI (a scheme, no space or control character), or the VST3 class id as 32 **upper-case** hex digits, so one plugin has one spelling. The plugin's identity. |
| `plugin.name`, `plugin.version` | As scanned when the insert was made. Shown when the plugin is missing; no logic reads them. |
| `active` | `false` is Pro Tools' inactive: not instantiated, not in the chain, no latency. Structure, so undoable (ADR-0012 O1). |
| `bypass` | Instantiated and passing audio through, still delayed by its latency, so a toggle never shifts timing. Mix state: saved, not undoable. |
| `params` | Parameter values by **stable key**: the LV2 port symbol (a C identifier), or the VST3 ParamID in decimal without leading zeros. Never a port index, which changes between plugin versions. Values are **format-native**: the LV2 port value, the VST3 normalised value, which must be from 0 to 1. Finite floats, parsed straight into `float`. Keys unique per insert; at most 8192. Mix state. |
| `state` | Optional, absent when there is none. The plugin's own state, in a file under `Plugin State/` named by its SHA-256: `file` is exactly `"Plugin State/<sha256>.state"`, `sha256` 64 lower-case hex digits, `bytes` its size. Written durably before the session file that names it. Mix state. |

- **State lives beside the file, not in it.** Some plugins' state runs to
  megabytes, and the session file's bytes cross the control socket inside
  every Snapshot and undo window (64 MiB cap). State files are append-only
  like sources: a later save writes a new file, and unused ones are removed
  only by an explicit cleanup.
- **A missing plugin is kept.** An insert whose plugin is not installed
  loads with everything it had, is left out of the chain and is shown as
  missing. Dropping it would delete it at the next save.
- A `state` file that is missing or whose hash does not match is reported
  at open; the insert loads with the plugin's defaults plus its `params`,
  and the operator is told.

### Regions

```json
"regions": [
  {
    "id": 21,
    "name": "Audio 3_02",
    "position": 3600000,
    "length": 480000,
    "source": [ { "id": 7, "channel": 0 } ],
    "source_offset": 24000,
    "gain_db": 0.0,
    "mute": false,
    "fade_in": { "length": 0, "shape": "linear" },
    "fade_out": { "length": 480, "shape": "linear" }
  }
]
```

- `position`: timeline sample of the region's first frame. `length` > 0.
- `source`: one entry per track channel, naming a source and a channel within
  it. A mono track has one entry. A stereo track is either one stereo source
  (`[{7,0},{7,1}]`) or two mono sources (`[{7,0},{8,0}]`). Both shapes are
  common in AAF from other DAWs, and the format must hold either without
  conversion.
- `source_offset`: the source frame that plays at `position`. The *handle*
  is the source material before `source_offset` and after
  `source_offset + length`. Crossfades need it.
- **Invariants, checked at load:** regions in a playlist are sorted by
  `position` and **do not overlap**; `source_offset + length ≤` the shortest
  referenced source's `length`; every `source.id` exists with that channel;
  `fade_in.length + fade_out.length ≤ length`.
- Fade `shape`: `"linear"`, `"equal_power"`, `"s_curve"`. Append-only
  enumeration.

Non-overlapping is a decision, not a simplification (ADR-0009 §3). Laying a
region over another trims or splits the one beneath it in the edit operation
itself, the way Pro Tools behaves. The renderer never resolves layers. So at
any instant a track plays at most one region, or two inside a crossfade. The
real-time path needs a bound like that, and an AAF `Sequence` has the same
shape.

### Crossfades

```json
"crossfades": [
  { "left": 20, "right": 21, "length": 240, "align": "centre", "shape": "equal_power" }
]
```

A crossfade joins two regions that **butt** (`left.position + left.length ==
right.position`) on the same playlist. It extends each region into the
other's handle: with `align: "centre"` the fade runs from
`right.position − ⌊length/2⌋` to `right.position − ⌊length/2⌋ + length`, so an
odd length puts the extra sample after the cut; `"pre"` ends at the cut,
`"post"` starts at it. The same rule places the punch crossfades and the
monitoring ramps at A and B. The required handle (a region's source
material past its edge) must exist, and is checked at load and on every
edit that creates or moves a crossfade. Without the handle the crossfade is
refused, not shortened silently.

At a region's end with a crossfade, the crossfade is what plays; the
region's own fade at that end is kept, unheard, and the next sentence does
not count it. **What plays at one end of a region must not overlap what
plays at its other end**: a crossfade's part inside a region plus the
crossfade or fade at the region's other end is at most the region's
`length`. The renderer multiplies a fade and a crossfade that cover the same
frames, so every edit that would write it on purpose is refused (`SetFade`,
`SetCrossfade`). An edit that moves an edge, clears a crossfade (revealing
the fade kept under it) or adds a take's crossfades instead shortens the
fade to meet the crossfade, as a fade shortens to fit a trimmed region.

v2 files written before this rule can hold the overlap: the interface's trim
and crossfade clear, and any `op_codec` client such as `sonora-ctl`, could
produce it. The loader repairs such a file rather than refuse it: a fade
running into the crossfade at its region's other end is shortened to meet
it, and the load reports each one by key path, old and new length, for the
opener to show. The next save writes the repaired session. Two crossfades
that overlap inside a region were always refused and still are. So the rule
tightens v2 without a version bump and without breaking "Backward
compatibility: always". *Added 2026-10-14.*

This is exactly AAF's `Transition` between two `SourceClip`s in a
`Sequence`, and it is why a crossfade is its own object rather than an
overlap of two regions' fades.

### `markers`

```json
"markers": [ { "id": 40, "position": 9600000, "name": "Chorus 2", "length": 0 } ]
```

`length` > 0 makes it a range (a memory location with a selection). Sorted
by `position`.

## Ids

Every source, track, playlist, region and marker has a session-unique
integer id from one counter. A crossfade has none: it is named by its two
regions. Ids are stable for
the life of the object, across saves and across undo. The counter is **not**
stored. On load it resumes at `max(id) + 1`. Undo history is not saved, so
an id freed by an undone operation cannot be referenced by anything after a
reload, and storing a counter would add a second field that can disagree
with the first. Integers rather than UUIDs, because they make diffs readable
and tests deterministic. Interchange maps them to and from AAF MobIDs and
slot ids at the boundary; `umid` is the only globally unique identity, and
it belongs to the audio, not to the edit.

## BWF metadata, both ways

The [BWF spec](bwf-broadcast-wave.md) is the field layout. This is the
mapping.

**Recording a take writes:**

| `bext` field | Value |
|---|---|
| `TimeReference` | `timecode.origin` + the timeline position of the file's **first frame**, after latency compensation: `origin + P0 − L`, where `P0` is the playhead when capture of this file began and `L` the round-trip latency. The default origin of 01:00:00:00 keeps this positive. |
| `Version` | 1 once `umid` is written; 0 until then, as today. |
| `UMID` | The source's `umid`. |
| `Description` | `sonora take=<n> track=<id> session=<name>`, plus ` punch=<A>-<B>` (timeline samples, relative to `origin`) for a punch take. Machine-readable, and harmless in another DAW's browser. Written when the file is opened, since the punch range is set before the pass. ` track=<id>` is left out when the engine has no document for the track: an engine track's position is not a track id. At most 255 bytes, so the field keeps its NUL: a session name that does not fit is cut on a UTF-8 character boundary, never the punch range. Built: `take_description()` in `engine/core`. |
| `Originator` | `Sonora Engine`, as today. |
| `OriginationDate/Time` | Wall clock when the take began. |

**Why that matters beyond interchange:** with `TimeReference` and
`Description`, every take is *self-describing*. The session file is an
index over the audio, not the only record of where it goes. A take that
closed after the last save, or whose save was lost, can be put back on the
right track at the right sample from its own header (see "Crash recovery").

**Opening a session** checks each source against its file's `bext`: a
`time_reference` that disagrees means someone replaced the file. The source
is flagged and the operator told. Nothing is moved.

**Importing a BWF** (copy into `Audio Files/`, then spot): "spot to original
time" places it at `TimeReference − origin`. If that is negative, or the
file's rate is not the session rate, the import is refused with that reason.
No sample-rate conversion in Phase 2.

The current engine writes `TimeReference = playhead` with no origin and no
latency term (`engine/core/src/session.cpp`, `start_recording`). That stays
correct until the Phase 2 writer lands, because there is no session file to
disagree with.

## Saving: atomic, never torn

The model serialises an immutable snapshot (`timeline-model.md`) into bytes,
so a save never blocks editing and never sees a half-applied edit. The
model does no I/O. `engine/control` hands the bytes to its **one saver
thread**, which performs every write (main file, autosave, backups) in
order; a save requested while one runs is coalesced into the newest
snapshot, so two saves never race on the same `.tmp`. The saver writes
like this:

1. Write `<name>.sonora.tmp` in the session folder (same filesystem as the
   target, or rename is not atomic).
2. Flush and `fsync` the file.
3. `rename` it over `<name>.sonora`.
4. `fsync` the folder, so the rename itself survives a power cut.
5. Write the previous saved bytes, which are still in memory, to
   `Session File Backups/<name>.<yyyymmdd-hhmmss>.sonora` by the same steps.
   Keep the newest 20.

A reader therefore sees the old file or the new one, never a mixture. There
is no window in which `<name>.sonora` is missing, which is why the backup is
written from memory rather than by renaming the old file away. A leftover
`.tmp` at open is an interrupted save by definition; it is deleted, after the
main file has parsed.

`fsync` is not standard C++. Under ADR-0008 (portability), `common/model`
is platform-neutral and only produces bytes. The saver in `engine/control`
calls `write_file_durably(path, bytes)` in `common/platform`, whose POSIX
implementation does steps 1–4. `std::filesystem::rename` alone is not
enough, because it does not sync.

**Autosave.** After every committed take (the `RecordTake` operation) and
every 60 s while there are unsaved edits, the same procedure writes
`Session File Backups/<name>.autosave.sonora`. Explicit save writes the main
file. At open, an autosave with a higher `save_generation` than the main
file is offered, never applied silently.

### Size

About 400 bytes per region in canonical form. 18 tracks with 500 regions
each is under 4 MB, written in well under a second even on a stick. Save
cost does not justify an incremental format. *Unverified: the write time of
a 4 MB file with `fsync` on the persistence partition of a real USB stick.*

## Crash recovery (what Phase 5 builds on)

The power-pull exit criterion of Phase 5 needs three things, and this format
provides them:

- **The session file is never torn** (atomic save above).
- **Takes are self-describing** (BWF mapping above). At open, any file in
  `Audio Files/` that no source references is an orphan. Orphans are listed
  and offered for recovery, using `TimeReference` and `Description` to place
  them. They are never deleted.
- **A take whose header was never finalised** gets its length from the
  file size, in whole frames. That is the one write to a closed file the
  format allows. Built (`common/audiofile` `recover_take()`): the engine
  runs it over `Audio Files/` at startup, before anything records, and says
  each recovered take as a Notice; the take is left unplaced. It touches
  only files whose bext Originator is `Sonora Engine` (another program's WAV
  may carry chunks after its data), and of those only files whose RIFF size
  is one an unfinished take has: 0 (`open()`, no checkpoint yet) or a
  checkpoint's, the header and whole frames with no pad, up to the data
  size. Up to, and not only equal: a checkpoint, and `close()`, write the
  data size before the RIFF size, so a `kill -9` between the two leaves the
  previous checkpoint's RIFF size (or 0) under the new data size. No audio
  is lost there, since the audio is flushed before either write and the
  data size counts all of it; only the RIFF size, which a reader that
  trusts it needs, is wrong, and recovery rewrites that alone. A RIFF size `close()`
  wrote, or one a metadata tool wrote when it appended `LIST` or `iXML` to a
  finished take, means the file is closed, and rule 1 leaves it as it is:
  read as audio, an appended chunk is a full-scale burst. It never shortens
  a header (a file shorter than its header is reported), and is
  idempotent. **One engine per session folder:** the engine holds an
  advisory lock on the folder (`common/platform` `FolderLock`, `flock` on
  the folder itself, so nothing is written into it) from before recovery
  until it exits, and a second engine on the same folder is refused with
  the reason. Without it the second engine's recovery would treat the
  first one's open takes as unfinished and cut the frame being written.
- **The header is kept valid during a take.** At every checkpoint (below)
  the disk thread patches the RIFF and data sizes, and the TimeReference
  once known, to the frames written, so a take whose engine dies reads as
  a valid file up to its last checkpoint without any recovery; recovery
  then adds what reached the file after it. The checkpoint's header counts
  no pad byte, which `close()` adds, so it matches the file as it is. Every
  drain pass (10 ms) also flushes the writer's buffer to the OS, without a
  sync, so the **loss window at `kill -9`** (the process dies, the kernel
  lives) is only what the capture rings still held: about one pass. Tested
  against `kill -9` of a recording process (`engine/core`
  `tests/posix/test_take_kill.cpp`, which prints the frames lost).
- **Audio reaches the medium on a schedule.** Written data sits in the page
  cache until the kernel flushes it, so without a sync a power cut can lose
  tens of seconds of a take. The disk thread checkpoints every open take
  file every `SessionConfig::sync_seconds` (2 s, *placeholder*): header
  patch, then `fdatasync` through `common/platform`, the folder `fsync`ed at
  the first one so the file's name survives too; never from the audio
  thread. The **loss window** at a power cut is then at most that interval
  plus the capture ring's depth, per track. A failed `fdatasync` fails the
  take with the reason (Linux clears the error, so a retry would claim data
  that is gone). *Unverified:* the `fdatasync` cost on a stick while 18
  tracks record, which must not cause a capture overrun; it sets the
  interval. A checkpoint pass drains the rings after each file it syncs, so
  the rings wait for the slowest single sync, never for the sum over 18
  tracks. `DiskThread::longest_checkpoint_ns()` is that wait, the figure to
  read: `sonora-engine` prints it at exit ("longest checkpoint") and on the
  `--record` `RESULT` line (`checkpoint_ms=`).
- **Punch takes are recovered by their own header.** A recovered orphan
  whose `Description` carries `punch=<A>-<B>` is offered for placement as
  that punch, over `[A, B)` with the session's punch crossfades. Without
  that field, or if its length cannot cover `[A, B)`, it is recovered as an
  **unused source**, never placed over existing material automatically.

What is lost in a power cut: edits since the last autosave, the undo
history, which is never saved, and at most the loss window of audio at the
end of each take being recorded. No take's placement: every take carries it
in its header.

*Unverified, and the measurement that settles it is the researcher's:*
whether `rename` plus a directory `fsync` is durable through the persistence
overlay (`home/rw` on ext4, `brain/research/usb-stick-layout.md`). Test: in
QEMU, save in a loop and kill the VM at random points 200 times; the file
must always parse and be either the previous or the next save. Repeat on a
physical stick with the power pulled, when hardware time exists.

## Opening

In order, failing loudly at the first problem that would make the session
lie:

1. Parse and validate (strict, as above). Failure: say where, offer the
   newest backup or autosave.
2. Version check and migration (below).
3. `sample_rate` against the backend. Mismatch: refuse, say both rates.
4. Every source against its file: exists, rate, channels, length,
   `TimeReference`. A missing file makes the source **offline**: its regions
   play silence, the UI marks them, and the session opens with a list of
   every missing file on screen. Refusing to open would be worse, because
   it would also stop the operator recording. A file shorter than `length`
   is reported the same way, and the regions past its end play silence.
5. Orphan scan of `Audio Files/` (crash recovery above).

## Versioning

`sonora_session` is one integer that goes up by one for **any** change to
what the file can contain or what a key means. The parser is strict, so a
version cannot carry keys it does not define.

- **Backward compatibility (old file, new program): always.** The reader
  migrates in memory, one version at a time (`v1→v2`, `v2→v3`), each step a
  pure function on the parsed tree. Each step has a frozen fixture file in
  `common/model/tests/` that must load forever.
- **Before the first save over an older version,** the original is copied to
  `Session File Backups/<name>.v<old>.sonora` and is never rotated out. With
  Phase 5's A/B updates, a rollback to the previous image would otherwise
  meet a file it cannot read.
- **Forward compatibility (new file, old program): refused, loudly.** The
  message names the file's version, the program's, and the pre-migration
  backup if one exists. There is no lenient read-only mode. A lenient reader
  that ignores what it does not understand shows the operator a session that
  is not the one on disk, and a lenient writer deletes what it ignored.
- Adding a key with a default is still a version bump. That keeps the rule
  mechanical: the migration fills the default.

**v1→v2 (inserts, ADR-0012):** every track gains `"inserts": []`. Nothing
else changes. The frozen v1 fixture loads through it.

**v2→v3 (solo safe):** every track gains `"solo_safe": false`. Nothing else
changes. The frozen v2 fixture loads through it; v3's is
`v3-solo-safe.sonora`.

**v3→v4 (tempo bounds, ADR-0014 §2, `midi.md` §2.4):** the tempo map is
brought inside the bounds below. A `bpm` outside 1 to 1000 is clamped to the
nearer end; a meter denominator that is not a power of two from 1 to 64 is
rounded to the nearest one (halfway to the larger: x/3 opens as x/4, x/6 as
x/8, anything above 64 as 64). Each change is reported at open with its key
path, the old value and the new, and the version 3 file, kept as the
pre-migration backup at the first save, holds the original (owner question
O7). This is the first migration that changes a value rather than filling a
default, and the reason is stated: no integer arithmetic is exact over an
unbounded binary32 bpm, and a beat of 1/3 is not a whole number of ticks.
Nothing else changes. The frozen v3 fixture loads through it unchanged, and
`v3-tempo-out-of-bounds.sonora` checks the report; v4's is
`v4-tempo-bounds.sonora`. A version 3 session above 384 kHz is refused: a
rate cannot be changed without resampling every source, and no backend this
build drives runs there.

**v4→v5 (MIDI, ADR-0014 §3, `midi.md` §4):** every track gains
`"kind": "audio"` and the top level `"midi_sources": []`. Nothing else
changes. The frozen v4 fixture loads through it; v5's is `v5-midi.sonora`,
with one track of each kind.

Worked example, the Phase 3 mixer's migration, at the next free version: `monitor_bus` becomes
`mixer.buses[0]` named "Main" with the same `outputs`, `gain_db` and
`pan_law_db`; each track gains `"output": <id of Main>` and empty `sends`
(inserts already exist from v2), and buses gain empty `inserts`. Nothing about regions changes. If that migration turns out to
need anything other than filling defaults, Phase 2's format was wrong, and
that is the test of this design.

## Interchange constraints (Phase 4, AAF)

Fixed now so the Phase 4 round trip is a mapping, not a redesign. Each line
is what AAF holds and how the format matches it.

| AAF | Native | Note |
|---|---|---|
| Source mob, MobID | `sources[]`, `umid` | Why `umid` exists. A source without one gets one generated at export, and the report says so. |
| Composition mob, one timeline slot per track channel | `tracks[]`, `source[]` per channel | Multi-mono slots map to per-channel `source` entries without conversion. |
| Slot edit rate | `sample_rate` | Edit rate = sample rate, so positions stay integers. AAF from video-rate editors (e.g. 25/1) is converted at import with exact rational arithmetic, and the report states any rounding. |
| `Sequence` of `SourceClip`, `Filler` | non-overlapping `regions` | The non-overlap invariant is the reason the mapping is direct. |
| `Transition` | `crossfades[]` | Same shape: between two clips, overlapping each by its handle. |
| Fade on a lone clip | `fade_in`, `fade_out` | *Assumption:* represented by `SourceClip`'s optional fade properties. Which representation libaaf reads and writes, and what other DAWs emit, is for the researcher before Phase 4. |
| `Timecode` component | `timecode.origin`, `fps` | Export rounds `origin` to a frame boundary and reports it if it was not on one. |
| Markers (`DescriptiveMarker`) | `markers[]` | |
| Region / clip gain | `gain_db` | AAF `OperationGroup` with an audio gain effect. |
| Volume and pan automation | *(not in v1)* | Phase 4's exit criterion needs it. It arrives as a version bump with sample-anchored breakpoints, the same integer rule. |
| — | inactive playlists, unused sources | Not representable as a composition. Export drops them and the `ConversionReport` lists them (ADR-0007 §4.5). |

What would have painted us into a corner, and is therefore excluded from
the format: float or timecode positions; overlapping layered regions;
sources identified only by path; positions in musical time for audio;
sources outside the session folder.

`.ptx` import (libptformat) and AAF import produce a `model::Session` and are
saved by this writer. The engine and the UI only ever open native files
(ADR-0007 §4.5).

## Not in version 1

Inserts arrive in version 2, solo safe in version 3, the tempo bounds in
version 4 and MIDI in version 5 (above; [`midi.md`](midi.md), ADR-0014).
Still not in the format: automation, sends, VCA, groups, an instrument
track's instrument (ADR-0014 M-5), clip-based time stretch, video,
interface state (zoom, scroll, window layout; ADR-0007 §5 puts it outside
the session), and the undo history. Each is a version bump when it arrives.

## Open questions

- Which AAF fade representation to emit (above); researcher, before Phase 4.
- Whether Phase 2's take writer writes `bext` v1 with a UMID or leaves it to
  Phase 4. The format allows either; the recovery story works without it.
- Durability of rename on the persistence overlay (above); researcher.
