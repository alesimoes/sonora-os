# ADR-0015 — MIDI sequences at scale: canonical chunks in memory, binary sidecar files on disk, a chunk table in the replicated window

- Status: **Accepted by the owner, 2026-10-20**, with every recommended
  default for O1–O5 ("For the owner"). Implementation, M-1c: PR 1
  (`m1c-midi-storage`) lands §3, §5 and §6's cleanup, storage and format
  first as the work allows; §1, §2 and §4 (chunks, the chunk table,
  Snapshot in parts) are PR 2; the saver's pacing behind checkpoints waits
  for the engine's saver thread (PR 3). BACKLOG Track 2 item 4 tracks
  each.
- Update 2026-10-20 (M-1c PR 1, #109), **accepted by the owner,
  2026-10-21**: `ModelAction::Load` carries a session bundle (the session
  file and its sequence files; protocol 8, window format 6) because the
  engine reads nothing from the interface's folder, so §4's "only attach
  and resync carry sequence bytes" adds Load, and PR 2's ≤ 16 MiB parts
  apply to Load as well as Snapshot (until then Open is refused by name
  over 64 MiB); §6's sync pacing moves to PR 3 with the engine saver,
  which also makes the engine sole owner of `MIDI Sequences/`, and must
  merge before M-4 records MIDI.
- Note 2026-10-25 (M-1c PR 2), for the owner's acceptance: the spine has
  two levels, a leaf per 64 chunks, because a 60-minute region's single
  level measured 70.3 KiB, past Unverified 1's 64 KiB; the protocol is 10,
  not 9, which loop record (#117) takes; a Snapshot with no chunk comes
  without a header. A Load carries chunk-table slices in the Snapshot's
  `SWCP` parts, so a sequence file over 16 MiB is split, not refused (the
  architect's review of #120: one 60-minute take at DIN's 1,000 messages a
  second is about 17 MiB). Only a single chunk over a part is refused, by
  name, where it would come into being: `apply`, the format-5 and `.msq`
  open, and SMF import (through `apply`); the encoder keeps its refusal as
  a backstop. For M-4: the take compound must not be refused in a way that
  loses data; the raw take file (§7.2) is kept and an over-cap chunk is
  reported, never dropped silently. Measurements in `midi.md` §4.6.
- Update 2026-10-25 (M-1c PR 3), for the owner's acceptance: **File >
  Save is a request to the engine**, `ModelAction::Save` (protocol 11):
  the session file's absolute path in the engine's filesystem and the
  canonical bytes the interface serialised (the interface still picks the
  name, the generation and "nothing to save"). The engine's saver thread
  (`engine/control` `SessionSaver`) writes every sequence those bytes name
  from its own document, then the session file (pre-migration copy, newer
  version refused, tmp + sync + rename + folder sync), and cleans up per
  O4; it replies when the save is durable, or Ok with a caution when the
  folder would not sync. A sequence the bytes name that the engine's
  document does not hold (an optimistic edit it refused) fails the save by
  name. While a take is open the saver waits for the next bump of the disk
  thread's checkpoint counter before it touches the disk, one batch per
  bump, the clients are told "Saving after the next checkpoint", and
  cleanup waits for the transport to stop. The folder logic moved from
  `ui/qt` to a new layer-2 module, `common/session_files`
  (`sonora_session_files`: common/model and common/platform, no Qt), so
  the engine and the interface's tests share one implementation; the
  interface binary no longer links it, so in the product only the engine
  writes or deletes in `MIDI Sequences/`. `FakeEngine` writes the files
  unsynced and never cleans up. Takes kept unplaced over the budget and
  rebuilding a missing sequence from its take wait for M-4's SMF reading.
- Date: 2026-10-20. Revised the same day with the rt-engineer's review of
  PR #102 ("accept with changes"); the PR thread maps each point to where
  it landed.
- Decided by: architect, from ADR-0014 Unverified 6 as measured in M-1
  (PR #100, branch `m1b-midi-model`, `brain/specs/midi.md` §4.6 there) and
  its BACKLOG item ("Unverified 6's decision, before M-4").
- Amends, if accepted: **ADR-0014 §1** ("Events are stored in the session
  file, not in the pool") and its rejected alternative "Sequences beside
  the session file, content-addressed", which that ADR named as the remedy
  if Unverified 6 failed; **ADR-0007's M4 update, item 2** (a Snapshot is
  one message under the 64 MiB model cap); `session-format.md` (a new
  folder and region key). **ADR-0009 §1 and §2 are not amended**: undo is
  still a pointer over immutable states, replication still the forward
  operation.
- Version numbers (session format, undo window format, protocol) mean the
  next free number when each PR lands, as ADR-0014 states.
- Numbering: follows ADR-0014.

## Context

ADR-0014 put each MIDI region's events inline in the session file as JSON
arrays, as a window onto a copy-on-write `MidiSequence` (three
`std::vector`s: notes at 24 bytes, other events at 16, SysEx). It named one
measurement that would decide whether that holds. M-1 ran it
(`common/model/tests/measure_midi_session`, Release, the build container on
the owner's laptop, 2026-10-20):

| Case | Measured | Cap it meets |
|---|---|---|
| 18 instrument tracks, 5 min, 8 notes/s, **30** CC/s per track | 205,200 entries, 8.5 MiB, 1.1 M DOM values, opens in 101 ms | inside |
| same, **100** CC/s per track | 583,200 entries, 23.2 MiB, 3.0 M DOM values | **does not open**: the 256 MiB DOM-memory cap binds first, at about 89 bytes a value |
| 200 note drags in one region of 50,000 CC events | each drag copies the sequence, about 800 KiB; the 64 MiB undo budget keeps 79 of 202 steps | the undo window is **132 MiB**, twice the 64 MiB a Snapshot may carry: **no replica can attach** |

Three distinct failures, from one cause, the whole sequence being the unit
of everything:

1. **Open.** The JSON reader's caps (64 MiB input, 4,000,000 values,
   256 MiB DOM) exist so a hostile file fails by name. Inline MIDI costs
   about 30 bytes and 7 values a note, and about 89 DOM bytes a value. A
   session that saves but does not open is the closest thing to a lost take
   that is not one.
2. **Undo.** A one-note drag copies every event of the region. ADR-0009 §1
   charges the copy to the budget; it is charged correctly and it is
   simply too big.
3. **Attach.** `encode_window()` carries the current state and every undo
   step in one Snapshot, capped at 64 MiB (ADR-0007 M4 update, item 2).
   Inline sequences put all MIDI bytes in it. Past 64 MiB the engine drops
   undo steps until it fits, and if the current state alone is over, no
   interface can connect at all.

What a heavy real session needs is assumed, not measured: orchestral
templates run 50 to 100 MIDI tracks with two or three controller lanes;
live recordings run for hours. **Design target, an assumption:** 16 M notes
and events per session (2^24), about 2.3 hours of the measured 100 CC/s
case on 18 tracks. Nothing can make such a session yet (no recording before
M-4, no import before M-6, no editing UI before M-7), so the cost of
changing the representation is lowest now, before M-3's plan builder and
M-4's take compound are written against it.

What any remedy must keep:

- ADR-0009 §1/§2: every step is a whole immutable state, undo moves a
  pointer, what replicates is the forward operation applied by a pure
  `apply` on both sides, the engine decides eviction.
- ADR-0009's "the UI cannot edit optimistically": the replica must hold
  every sequence an operation can name, because `EditMidi` removes by index
  into the region's current sequence.
- ADR-0005 and `session-format.md`: the session lives on ext4 on the
  stick's partition 3, saves are atomic and never torn, 20 backups and an
  autosave every 60 s, orphans are offered, never deleted.
- ADR-0014 §6: takes are SMF files in `MIDI Files/` written while recording
  and recoverable on their own. Nothing here touches that path.
- The audio thread is not involved: sequences are read only by the control
  thread, the saver and the plan builder (ADR-0014 §4: MIDI needs no
  residency gate). That stays true.

## Decision

### 1. A sequence is a sparse array of canonical chunks, in `common/model`

- `MidiSequence` stops being three vectors. It becomes an immutable
  **spine** of `(chunk index, shared_ptr<const MidiChunk>)`, sorted, with
  empty chunks absent. A `MidiChunk` holds the notes, events and SysEx
  whose data tick (a note's start) is in `[k · S, (k + 1) · S)`, in the
  orders ADR-0014 already fixes.
- **The chunk span `S` is fixed: 3,840,000 ticks, four quarter notes** (2 s
  at 120 bpm). Data ticks, not bars, so a meter change never re-chunks.
  *Placeholder until measured* (Unverified 1).
- **The chunking is canonical**: a function of the sequence's contents
  alone, never of the edit history. Two paths to the same notes give the
  same chunks, so `apply` on the engine and on a replica builds identical
  structures without coordination, a file saved and reopened gives the
  same bytes (DAW 2's byte-identical exit criterion), and identical chunks
  can be shared.
- The public reading API (iterate notes, events and SysEx in order; count;
  index ↔ position) is what M-3's plan builder, M-4's resolver and M-7's
  piano roll are written against. `EditMidi` keeps its **global indices**;
  each spine entry holds the **cumulative** note, event and SysEx counts
  before its chunk, so an index maps to a chunk by binary search, and every
  add or remove rewrites the spine (charged in §2).
- Readers that cache by identity (M-3's flatten cache) key on the chunk
  pointer, so an edit re-flattens the chunks it touched and not the
  region. A note belongs to the chunk of its start but may sound into the
  next ones, and its off frame depends on the region's window: the cache
  key is the chunk pointer together with the region's window and the
  tempo map's revision, never the pointer alone. That is a side benefit
  for ADR-0014 Unverified 7, not a claim.

### 2. Undo: structural sharing reaches into the sequence; ADR-0009 is unchanged

- An `EditMidi` copies the spine and the chunks it touches, and shares the
  rest. The step is charged, as ADR-0009 §1 says, what it alone allocated.
  The charge is computed from entry counts and fixed per-entry sizes, so it
  is deterministic across compilers and allocators.
- Rough cost for the measured case (50,000 CC events at 100/s, 8 notes/s,
  so 250 chunks of about 200 events and 16 notes): a chunk is about
  3.6 KiB, the spine about 10 KiB (40 bytes an entry: chunk index,
  pointer, three cumulative counts). A drag that moves a note across a
  chunk boundary copies two chunks and the spine, **about 17 KiB instead of
  800 KiB**. 200 drags charge about 3.4 MiB; the 64 MiB budget keeps about
  3,800 such steps. A 60-minute region has a 70 KiB spine, which then
  dominates; a two-level spine is the upgrade, and likely needed for long
  regions if Unverified 1 confirms it.
- A transform of the whole region (quantize all, transpose all) copies
  every chunk it changes, exactly as today. The newest step is never
  evicted (ADR-0009 §1), so it can always be undone once.
- **Operation size.** An `EditMidi` crosses as JSON through the op codec
  and the same reader caps: about 7 values an added note, so one operation
  holds about 500,000 entries before the 4,000,000-value cap. Over that it
  is refused by name, not truncated; a binary op encoding or parametric
  transforms are later work if real use reaches it (Unverified 4).

### 3. On disk: one binary sidecar file per sequence, content-addressed

- Each distinct sequence is written to
  **`MIDI Sequences/<sha256>.msq`**, named by the SHA-256 of its bytes, as
  ADR-0012's plugin state is. A region holds a reference instead of its
  arrays:

  ```json
  "data": { "file": "MIDI Sequences/9b2e…41.msq", "sha256": "9b2e…41",
            "bytes": 3654210, "notes": 43200, "events": 540000, "sysex": 0 }
  ```

  Regions that share a sequence (split, duplicate, paste) share the file,
  which inline JSON did not: today a split writes the sequence twice.
- **The `.msq` format** is ours, little-endian, versioned, and canonical:
  magic and version, counts, then notes, events and SysEx, each section in
  its fixed order, ticks as minimal unsigned LEB128 deltas from the
  previous entry of the section, lengths and SysEx sizes as LEB128, bytes
  raw. One sequence has exactly one encoding, so its hash is stable. Rough
  size: a CC event about 6 bytes, a note about 10, against about 20 and 30
  in JSON. The measured 100 CC/s session is **about 3.5 MiB instead of
  23.2**; the 16 M target is about 100 MiB. The byte layout goes into
  `midi.md` §4.4 with the implementation; the decoder is bounded and
  non-throwing like `common/midi`'s SMF reader, and fuzzed. Each section
  is non-decreasing in its first key, so deltas are unsigned: the decoder
  refuses a non-minimal LEB128, a count that disagrees with the header,
  and anything that would put an entry before its predecessor.
- **Write order, the plugin-state rule, through tmp and rename.** The
  name is the hash, so a file torn by a crash must never carry it. For
  each sequence the snapshot names that is not yet on disk, the saver
  writes `<sha>.msq.tmp`, `fdatasync`s it and renames it to `<sha>.msq`;
  **one folder `fsync` covers the batch**; then the session file is saved
  atomically as today. A session file never names a sequence that is not
  durable. A leftover `.msq.tmp` at open is an interrupted save and is
  deleted.
- **What is on disk is a set of hashes**, the saver's own, never object
  identities (a freed sequence's address can be reused). At open it holds
  the hashes of the referenced files that verified; the saver adds each
  file it renames and **removes each file cleanup deletes**, so a sequence
  still held by an undo step or a replica is rewritten if it is saved
  again after its file went.
- **The saver encodes and hashes, not the document thread.** It is handed
  the immutable snapshot (shared, safe to read from another thread) rather
  than finished bytes, and serialises both the sidecars and the session
  file. SHA-256 cost is not measured: `sha256_hex` exists in
  `contracts/plugin`, moves to `common/base` (which `common/model` may
  use) and is measured in M-1c PR 1 (Unverified 6). It is paid in full at
  first save; later saves hash what changed.
- **Open** reads the session file (now small: regions, not events) and
  each referenced file once, checking hash, size and counts. A missing or
  damaged file does not drop its regions (O2).
- **Medium.** Every disk figure here assumes the worst case ADR-0005
  allows, the stick's ext4 persistence partition; an internal disk, which
  ADR-0005 also offers for sessions, only makes them smaller.
- **Migration.** The next session format number reads PR #100's inline
  format (5 on that branch) and moves each inline sequence to a sidecar at
  the first save, reported, with the pre-migration backup kept as
  `session-format.md` already requires.

### 4. Replication: the window carries a chunk table; a Snapshot travels in parts

- `encode_window()` already shares playlists and tracks across steps by
  index. It gains a **chunk table**: every chunk any retained step
  references, once, in the `.msq` encoding of a chunk, and the sequences in
  the window refer to chunks by table index. A chunk shared by 200 undo
  steps crosses once.
- **The window's chunk table travels in parts.** `contracts/api` gains
  `ModelUpdate` kinds for a Snapshot in parts: a header naming the part
  count and total bytes, parts of at most **16 MiB** each, and the window
  itself last. **Each part is a self-contained slice of the chunk table**
  (whole chunks, their own count), decoded and checked as it arrives, never
  a byte segment of one blob: the replica never holds more than one part's
  bytes beyond what it has decoded, so it needs no large `Reassembler`.
  The new state replaces the old only when the last part and the window
  have decoded; a bad part abandons the attach and asks for `Resync`. The
  64 MiB per-message cap stays, so the bound against a hostile peer is
  unchanged. The **replica has its own cap** on the announced total, an
  interface setting (default 512 MiB), not the engine's figure. Parts use
  the existing large-message path (at most 2 in flight over 1 MiB per
  client), so a slow interface delays its own attach and nobody else's.
- **Encoding the window leaves the document thread.** Today
  `encode_window()` runs there, and with a ~100 MiB chunk table edits would
  wait behind it. It runs on a worker holding the immutable window it
  encodes, as the saver does; the engine holds one encoded window per
  attaching client until its last part is sent (Unverified 3, 7).
- **Edits do not move chunks.** An `EditMidi` crosses as the forward
  operation it already is, a few hundred bytes for a drag, and the replica
  builds the same chunks with the same `apply` (§1). Only attach and resync
  carry sequence bytes.
- The replica holds every sequence in memory, as the engine does, because
  `apply` needs them. At the 16 M target that is about 256 MiB in each
  process (Unverified 3).
- This is an additive `contracts/api` change (ADR-0007 §6: ui-engineer and
  rt-engineer review) and a protocol bump, since it adds what the engine
  may send; the undo window format moves with it. ADR-0007's rule "over
  64 MiB, `snapshot()` drops the oldest steps until it fits" becomes "over
  the session's MIDI budget", and outside MIDI it binds exactly as before.

### 5. A stated budget, enforced where it can fail loudly

- **An engine setting, `midi_max_entries`, default 16,777,216** (2^24
  notes, events and SysEx messages in the document, every retained undo
  step's private chunks included). In memory about 256 MiB for the
  document, the same in each interface, plus M-3's plans (Unverified 7 of
  ADR-0014). An engine setting, not a session key, so a session over it
  is opened by raising it, not by editing the file.
- **At open**, a session over the budget is refused by name, with its count
  and the setting to change. Never opened half-loaded.
- **History goes before work.** Before an edit or a take placement is
  refused, undo steps are evicted, oldest first, down to the newest one,
  exactly as ADR-0009 §1's byte budget evicts (the floor rises and is sent
  to every replica). It is refused only if the document's current state
  alone would still be over. Otherwise history that could be dropped would
  refuse a take, which is common and recoverable.
- **At edit**, an operation that would still cross it is refused by name,
  as an out-of-range conversion is (ADR-0014 §2).
- **At take close**, never while recording. The SMF take is on disk and in
  the pool whatever happens. A take whose placement would still cross the
  budget stays there, **unplaced**: a `midi_sources` entry no region
  refers to, saved with the session, its SMF and header event in
  `MIDI Files/`, so it survives a reboot. A Notice names it; once there is
  room it is placed like an orphan, and that placement is an ordinary
  undoable `PlaceMidiRegion`. A warning shows from 90 % of the budget, so
  the operator learns of it before a take, not after.
- Counting is in entries, not allocator bytes, so it is the same on GCC and
  MSVC and on every machine (ADR-0009's reason for an engine-sent floor).

### 6. Crash recovery and cleanup

- Recovery is unchanged in kind. The session file is never torn; it never
  names a sequence that was not durable first; an autosave or backup names
  sequences that are still on disk, because of the rule below. Edits since
  the last autosave are lost, as today. A take is in its SMF, as today.
- **From take to sequence file, in order.** The disk thread writes the SMF
  take and checkpoints it (ADR-0014 §6) → the take closes → the control
  thread reads the file back and converts to ticks → the take compound
  places a new `MidiSequence` → the autosave that follows every
  `RecordTake` writes its sidecar, then the session file. A crash between
  close and that autosave loses nothing: the take is recovered from
  `MIDI Files/` by its header event, as today. The sidecar is a second
  copy of the take's data, about twice the bytes for recorded MIDI, which
  is small beside audio.
- **Saves while recording never stall a take.** In loop and punch
  recording takes close while other tracks still capture, so the autosave
  after a `RecordTake` runs during capture. On ext4 every `fdatasync` and
  folder `fsync` forces a journal commit, and under `data=ordered` that
  commit also flushes the take files' dirty pages, which is how a save
  could delay the disk thread's own syncs into a capture overrun. So,
  **while any take is open**:
  - the saver issues its syncs (each sidecar's `fdatasync`, the batch's
    folder `fsync`, the session file's) **only in the slot right after a
    disk-thread checkpoint has completed**, which the disk thread
    announces by bumping an atomic counter, and at most one batch of them
    per checkpoint period (`sync_seconds`). A saver commit therefore
    never sits in front of a take's checkpoint, and the take files' pages
    it flushes are the few written since that checkpoint;
  - cleanup is deferred until the transport stops;
  - the disk thread never waits for the saver; the saver waits, and a
    save that has to wait says so in the interface ("Saving after the
    next checkpoint").
  With no take open, the saver syncs as it does today. Whether this holds
  capture overruns at zero is Unverified 5.
- **Sequence files are derived state, not recordings,** so unlike audio
  and takes they are cleaned up without asking: after a save is durable,
  the saver may delete a sequence file that **no kept file names** (the
  main file, the autosave, each of the 20 backups, each pre-migration
  copy). A crash during cleanup leaves extra files, never a missing one.
- **The reference sets are held in memory.** The saver wrote every kept
  file, so it keeps each one's set of sequence names and drops a set when
  its file rotates out; the sets are rebuilt once at open by reading the
  kept files. A kept file that cannot be parsed has unknown references, so
  while it exists **nothing is deleted** (and the operator is told). Each
  deletion removes its hash from the saver's on-disk set (§3).
- **The bound is per session, not per sequence:** at most about
  22 × the MIDI bytes that changed between kept versions. One edited track
  of the measured case is about 0.2 MiB, so about 4.4 MiB. The worst case
  is a whole-session transform before every save: **about 22 × 100 MiB ≈
  2.2 GiB at the 16 M target**, against a persistence partition that is
  what is left of an 8 to 64 GB stick (roughly 3 to 30 % of it).
  Append-only, the same worst case would grow by about 6 GiB an hour at a
  60 s autosave (O4).
- A sequence file a kept file names is never deleted. Files in
  `MIDI Files/` (takes, imports) are never deleted by this rule.

### 7. When it lands

Before M-3, not only before M-4: M-3's flatten cache and M-4's resolver
are the first code written against the sequence type. As **M-1c**, two or
three PRs, each with its owners' review:

1. `common/model`: chunked `MidiSequence`, the `.msq` codec with fuzzing,
   the window's chunk table, the next session and window formats with the
   migration from PR #100's inline format; `measure_midi_session` rerun
   (Unverified 1, 2).
2. `contracts/api` and `ui/client`: Snapshot in parts, protocol bump,
   conformance and `FakeEngine` (ui-engineer, rt-engineer).
3. `engine/control`: the saver writes sidecars and cleans up; the budget
   and its Notices (Unverified 5).

PR #100 can merge as it is: its inline reader becomes the migration source
(O5).

## Rationale

- **It fixes the cause, once, for all three failures.** The whole sequence
  being the unit of storage, sharing and transfer is what breaks open,
  undo and attach. Canonical chunks are a smaller unit for sharing and
  transfer; a binary file per sequence takes the bulk out of the JSON
  reader's caps. Each failure gets a remedy that reuses a mechanism we
  already have: structural sharing (ADR-0009 §1), content-addressed
  sidecars written first (ADR-0012's plugin state), the window's shared
  tables (ADR-0007 M4).
- **ADR-0009 stays as it is.** Undo is still a pointer over whole states,
  replication still the forward operation. Delta-encoded undo would have
  been smaller per step and cost the model its simplest property.
- **Fail loudly at open, never while recording.** The budget is checked
  where the operator can act on it; a take is protected by its own file
  regardless, and is placed later rather than lost.
- **Exact.** Our own canonical format holds the model as it is: ticks at
  960,000, explicit note lengths, release velocity, SysEx. An SMF cannot
  (see Alternatives); the takes stay SMF because there exactness is in
  frames, which an SMF can hold.
- **Boots the same on unseen hardware.** Sizes are counted in entries, the
  format is little-endian and defined to the byte, and nothing depends on
  the machine except how long it takes (Unverified 2, 5, 6).

## Consequences

- `MidiSequence`'s API changes before anything but M-1 uses it; PR #100's
  tests that build sequences from vectors go through a builder.
- Session format, undo window format and protocol each move by one, at
  landing. A file of PR #100's format opens and migrates; an older engine
  refuses the new one by version, as always.
- The session file is no longer self-contained for MIDI: copying it without
  `MIDI Sequences/` loses MIDI, as copying it without `Audio Files/`
  loses audio. Session export and "Save Copy In" (DAW 8) copy the folder.
- The session file stops being hand-diffable for MIDI content. Tests that
  compared inline arrays compare decoded sequences instead.
- A saver failure on a sidecar fails the save by name and leaves the
  previous session file in place, as a failed main-file write does.
- The interface keeps a full copy of every sequence, about 256 MiB at the
  target. A remote or memory-poor interface would need on-demand chunks,
  which this ADR does not provide.
- `sha256_hex` moves from `contracts/plugin` to `common/base`, since
  `common/model` may not depend on `contracts/plugin`; the plugin code
  calls it there.
- `session-format.md` gains the folder, the `data` key and the cleanup
  rule; `midi.md` §4.4 and §4.6 are rewritten with M-1c. Neither is edited
  by this PR, to stay off PR #100's diff.

## Unverified

Each names the measurement that settles it and who runs it.

1. **Edit cost with chunks.** `measure_midi_session` rerun on the chunked
   model: bytes charged per drag (expected about 17 KiB), steps kept of
   200, window bytes, worst drag time on the control thread (today
   1.5 ms), for spans of 1, 4 and 16 quarters, and a 60-minute region's
   spine. rt-engineer, in M-1c PR 1. Over 64 KiB a drag means a two-level
   spine.
2. **Open and save at the target.** A 16 M-entry session on the laptop:
   at open, **hash time and decode time reported apart** (decode assumed
   under 1 s), and peak memory; first save time, hash and write apart.
   rt-engineer, M-1c PR 1. On the stick: OS 1.
3. **Attach at the target.** Time for an interface to receive and decode a
   ~100 MiB Snapshot in parts over the M7 socket, and **peak RSS on both
   sides**, not only the steady state: the engine's encoded window held
   per attaching client, the replica's undecoded part, the decoded
   ~256 MiB. With it, a test that each part decodes and is checked on its
   own, and that a bad part abandons the attach without touching the
   replica's state. ui-engineer with the rt-engineer, M-1c PR 2.
4. **Operation size in real use.** Whether whole-region transforms on dense
   regions reach about 500,000 entries in one `EditMidi`. Revisit with
   M-7's operations.
5. **Sidecar writes during capture.** A save of 1, 18 and 100 sidecars
   (tmp, `fdatasync`, rename, one folder `fsync`) paced as §6 says, during
   loop recording on 8 audio tracks and 4 MIDI tracks: capture overruns
   (must be 0), the disk thread's checkpoint latency with and without the
   save, and how long the save waited. Laptop proxy in M-1c PR 3
   (researcher, with the rt-engineer); the stick's ext4 under the `/home`
   overlay in OS 1. If overruns are not 0, sidecar writes during a take
   wait until the transport stops.
6. **SHA-256 throughput**, measured through `sha256_hex` once it is in
   `common/base`, on the laptop in M-1c PR 1 (minutes of work, not later),
   MiB/s on 100 MiB; then without SHA extensions on the oldest CPU class we
   expect, since the image runs on hardware we have not seen (researcher,
   OS 1).
7. **Encoding the window at the target.** `encode_window()` time for a
   16 M-entry document with 200 retained steps, on the worker, and how long
   the document thread is held (expected: only to take the snapshot).
   rt-engineer, M-1c PR 1.

## For the owner

**Ruled by the owner, 2026-10-20: every recommended default below is
accepted (O1–O5).**

- **O1. MIDI out of the session file.** *Recommended: yes,* binary files in
  `MIDI Sequences/`, as §3. The cost is a session file that no longer shows
  MIDI content as text, and a folder that must travel whole. The
  alternative (inline below a size, sidecar above) keeps small sessions
  readable at the price of two formats and two code paths for ever.
- **O2. A sequence file missing or damaged at open.** *Recommended:* the
  session opens; each region on it is kept with its window, shown "MIDI
  data missing", silent and not editable, never dropped (a save keeps the
  reference); where the region came from a take, rebuilding from the take
  file is offered. The alternative refuses to open the session.
- **O3. The budget.** *Recommended: 16,777,216 entries by default*, an
  engine setting; undo history evicted first, then open, edit or take
  placement refused above it, takes kept unplaced, a warning at 90 %. The alternatives are a higher default (more memory on a machine
  we have not seen) or none (out-of-memory decides, while recording).
- **O4. Cleaning up sequence files.** *Recommended: automatic,* deleting
  only files no kept session file names, because they are derived state.
  With it the folder holds at most about 22 × the MIDI bytes changed
  between kept versions: about 4.4 MiB for one edited track of the
  measured case, about 2.2 GiB in the worst case at the 16 M target. The
  alternative is the audio rule (only an explicit, confirmed Clean Up),
  which in that worst case grows the folder by about 6 GiB an hour.
- **O5. PR #100.** *Recommended: merge it with its inline format;* M-1c
  migrates from it. The alternative holds #100 until M-1c, so no inline
  MIDI format ever lands, at the cost of blocking M-2 work stacked on it.

## Alternatives rejected

- **Keep inline JSON and raise the reader's caps.** The 100 CC/s session
  needs about 270 MiB of DOM to open; the 16 M target about 8 GiB (about 88 M values). Raising
  caps that exist to stop a hostile file is the wrong lever, and undo and
  attach stay broken.
- **Inline JSON with a streaming reader for MIDI arrays.** Fixes the DOM
  cost only. At 30 bytes a note the target is about 400 MiB of session
  file, past the 64 MiB input cap, and undo and attach stay broken.
- **Delta-encoded undo for `EditMidi`** (each step stores the edit, a
  state is rebuilt by replay or by inverse). About 100 bytes a drag rather
  than 17 KiB, but it replaces ADR-0009 §1's "one state per step, undo is
  a pointer, no inverse operation" with inverses or replay for one
  operation kind, and both engine and replica would need them exact.
  Chunk sharing reaches the budget with the model unchanged.
- **Sidecars as SMF.** An SMF's division is at most 32,767 per quarter
  against our 960,000; it stores ons and offs, so two overlapping notes of
  one pitch read back by FIFO pairing can swap lengths; its delta time
  needs filler events past 2^28 ticks (about 140 s at 120 bpm here). Not
  exact, so not storage. SMF stays the take and interchange format.
- **Sidecars as canonical JSON, one per sequence.** Reuses the strict
  reader, but one dense region hits the same DOM caps on its own, at six
  times the bytes.
- **A file per chunk, or pack files of chunks.** Dedupes across versions
  (an edit writes about 7 KiB), but the target is about 80,000 chunks:
  either that many files to `fsync` on a USB stick, or a pack format with
  compaction and its own recovery. File per sequence reuses plugin state's
  rule; packs are the upgrade if Unverified 5 shows write amplification
  matters.
- **Replicas read sidecars from the session folder.** Unsaved edits are not
  on disk, so the interface would need a save per edit; it couples the UI
  process to the engine's folder, against ADR-0007's process split and a
  later Windows or remote interface. Plugin state can be by reference
  because the interface never applies an operation to it; MIDI cannot.
- **On-demand chunks in the replica.** Memory per interface would follow
  what is shown, but `apply` would block on a fetch or stop being pure.
  Rejected while the interface is local; named in Consequences.
- **History-dependent chunks (split when full, merge when small).** Adapts
  to density, but engine and replica could only agree by replaying the
  same history, and the same notes saved by two paths would hash
  differently.
- **One Snapshot message with a larger cap.** The `Reassembler` would hold
  a message of hundreds of MiB before checking it, which the 64 MiB bound
  exists to prevent.
