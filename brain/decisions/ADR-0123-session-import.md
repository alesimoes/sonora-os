# ADR-0123: Import Session Data from another Sonora session

Status: Proposed (architect, 2026-11-09). Backlog: `interchange.session_import`.

## Context
Pro Tools "Import Session Data" and Logic "Import from other projects" bring
tracks (with their material and settings) from one session into the open one.
Sonora has Import Audio (a WAV becomes a track) and Save As / templates, but
no way to take a track out of another `.sonora` session. ADR-0018 says a
session folder owns its media and never references outside it, so an import
must copy the audio. ADR-0009 says the document changes only by operations
through `apply`, so an import is one undoable `Compound`.

## Decision
1. **A pure planner in `common/session_files`** (`session_import.h/.cpp`,
   no Qt, no engine, no platform header):
   `bool plan_session_import(dest, src, request, plan, refusal)`, and
   `list_importable_tracks(src)` for the panel's rows. `dest` and `src` are
   `model::Snapshot`s (the other session is read with `model::deserialize`;
   opened read-only, never written). `SessionImportRequest` picks source
   tracks by id and flags: `material` (active playlist regions, fades,
   crossfades), `mix` (fader, pan, mute; solo off), `inserts` (with values;
   plugin state files are not carried), `automation` (volume and pan lanes),
   `offset` (frames added to every region start; 0 keeps times) and `markers`
   (default off). It also carries what a snapshot cannot say: the source
   folder, the source file's declared format version, whether a take is open
   here, and an injected `source_readable` probe, so the planner stays pure
   and a test can make a file missing.
2. **The plan holds two things**: `ops`, one `Compound` labelled "Import
   Session Data" (`AddSource` per needed source, `AddTrack`, `PlaceRegion`,
   ...), with every id remapped from `dest.next_id` upward in a fixed order
   (deterministic, so a test can compare bytes); and `copies`, the list
   `FileCopy{source_path, dest_path, source}` of audio files to bring in
   (absolute in the source, relative to the destination folder, and the new
   source's id). A source already
   in `dest` with the same non-empty `umid` is reused, not copied. A name
   clash gets a ` 2`, ` 3` suffix, never an overwrite.
3. **Copying follows Import Audio** (`AudioImporter`): a worker on the
   interface side copies each file into `<session>/Audio Files/` (hard link
   on one volume, else a durable copy) *before* the Compound is applied; a
   file copied but not placed is deleted. The Compound is applied on the
   interface thread only when every copy succeeded; any failure applies
   nothing. Undo takes the tracks back and leaves the copies, as for a take.
4. **Refused, naming the reason, before anything is written**: a source
   session of a newer format than ours; a source file missing or unreadable
   (listed by name); a different sample rate (no resampling in v1); import
   while a take is open; a request whose Compound would pass the 256-member
   limit (`TooLarge`, naming how many of the tracks fit). The planner does
   not split it itself, so one import is always one undo step; the interface
   offers the rest as a second import. A refusal leaves the plan empty.
   Skipped items are listed as `dropped:` / `approximated:` lines, as
   `sonora-convert` reports them: MIDI and instrument tracks, buses, sends,
   VCAs and groups are not imported in v1.
5. **No change to the engine, the wire or the session format.** The result is
   ordinary operations the existing replication carries. Protocol: none.
   Formats: none.
6. **UI**: the Import panel's browser accepts a `.sonora` session folder; the
   operator sees its tracks (name, channels, regions, length) with the flags
   of 1 and imports the ticked ones. Refusals stay on screen until dismissed,
   as Import Audio's do.

## Rationale
A planner that returns operations is testable without Qt or disk, replays on
every client and gets undo for free. Remapping ids from `next_id` keeps the
"no id is reused" rule (ADR-0030 does the same). Copying before applying means
a crash leaves orphan files at worst (the recovery offer and Import Audio's
cleanup already handle that) and never a source naming a missing file. Losing
audio is the worst outcome, so we fail loudly up front.

## Consequences
- One new file group in `common/session_files` and a UI model; no engine,
  protocol or format work.
- Plugin state of imported inserts is not carried in v1: the insert arrives
  with its values and no state blob (`approximated:`).
- Bus structure is not rebuilt: tracks arrive on the Master; sends are
  dropped. A later ADR can map buses by name.

- The contract lands first with a stub that refuses every import
  ("not implemented"), so nothing built against the header can report a
  success that imported nothing.

## Alternatives rejected
- **Reference the other session's files in place**: breaks ADR-0018 and moving
  the stick between machines.
- **A new engine operation `ImportSession`**: protocol and codec work and file
  I/O on the control thread, for nothing the existing operations cannot say.
- **Whole-session merge**: that is Open / Save As; the value is picking tracks.
- **Resample on import**: doubles the test surface; refuse first, follow-up.

## Unverified
- Hard links on the stick: exFAT has none. Researcher: confirm the persistence
  partition's filesystem; the code falls back to a copy either way.
- Time to copy a 40-track, 20 GB session over USB 3 (progress and cancel are
  specified; the figure is not known).
