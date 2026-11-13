# ADR-0142: Cross-session paste brings the clipboard's audio in

Status: Proposed (rt-engineer, 2026-11-09). Backlog: `ui.editing.commands`
(the cross-session paste left open; Insert/Delete Time, #473, and the
Shuffle ripple in Cut, Clear, Paste, Duplicate and Repeat are on main).

## Summary
Paste into a session whose pool lacks a clipboard source no longer refuses.
The clipboard remembers the folder (and rate) of the session it was copied
from; a pure planner in `common/session_files` (`paste_sources.h`) maps each
clipboard source onto the destination's pool by Import Session Data's rules
(ADR-0123): reuse by non-empty umid, else an `AddSource` with a new id and a
`FileCopy` into `Audio Files/`. The copies run first; the `AddSource` members
go ahead of the paste's own only when every copy succeeded.

## Context
`ui/qt/src/edit_clipboard.cpp`'s `paste_problem` refuses a paste whose clip
names a source the destination's pool does not hold with the same content:
"the clipboard's audio was copied from another session and is not in this
one; nothing was pasted". Pro Tools and Logic both let an operator copy a
region in one session and paste it in another, and Sonora's own Import
Session Data (ADR-0123) already knows how to bring audio from another
session folder safely. ADR-0018 forbids a session naming a file outside its
own folder, so the audio must be copied in, and ADR-0009 says the document
changes only through operations, so the sources arrive as `AddSource`
members of the paste's one `Compound`.

## Decision
1. **The clipboard remembers where it came from.** `ui::Clipboard` keeps the
   absolute folder of the session it was cut or copied from and that
   session's sample rate, set at Cut and Copy. Its `Clip::sources` already
   keep each source as the origin's pool held it.
2. **A pure planner** in `common/session_files`,
   `plan_paste_sources(dest, PasteSourcesRequest{sources, origin_folder,
   origin_sample_rate, source_readable}, PasteSourcesPlan&)`. No Qt, no
   engine, no platform header; whether a file is readable is injected. For
   each clipboard source, in request order, once per id:
   - the destination holds it under the same id with equal content (a paste
     within the session): it maps to itself;
   - else the destination (or a source this plan already adds) holds a
     source with the same **non-empty umid**, as many channels and at least
     its length: it maps to that one, no file is copied;
   - else an `AddSource` with a new id, **strictly upward from the
     destination's `next_id`**, its path `Audio Files/<basename>` with
     `" 2"`, `" 3"`... before the extension when the name is taken (pool,
     MIDI pool, left takes; compared without case for exFAT), kind
     `Imported`, take, track and latency cleared; and a `FileCopy` from
     `<origin>/<path>`.
   The plan returns the id remap, the `AddSource` members, the copies,
   `next_id` past them (where the paste's own members start), and a
   `problem` string. A source to copy with no known origin folder, a known
   origin rate that differs from the destination's, or an origin file that
   is not readable refuses the paste, naming the file, ending "nothing was
   pasted", with the rest of the plan empty. The interface shows it as a
   held notice, not the status line.
3. **Copies first, ops only on success.** `bring_in_sources(plan, copy,
   out, error)` runs the copier over every file and appends the
   `AddSource` members to the paste's members only when it succeeded; on
   failure `out` is untouched. The real copier is `disk_copier`, which is
   Import Session Data's `perform_copies` (ADR-0123 decision 3): every file
   checked before any is written, a hard link or a synced copy, never an
   overwrite, and what it wrote taken back on failure or cancel. It runs on
   the interface's worker thread, never on the audio thread.
4. **Undo removes the regions, never the audio.** `AddSource` is outside
   undo (operations.h), so the paste stays one undo step whose undo takes
   back the regions and leaves the brought-in files in the pool, as an
   undone take does.
5. The paste's whole `Compound` (sources plus regions) stays bound by
   `model::kMaxCompoundMembers`; the interface's existing `over_limit`
   check counts the `AddSource` members with the rest.

## Rationale
One set of rules for "audio from another session" (umid reuse, naming,
id order, all-or-nothing copy) means one copier and one set of tests; the
planner reuses ADR-0123's helpers (`with_suffix`, `fold`, `basename_of`)
and its copier, so the two cannot drift. A pure planner tests without Qt or
a disk and gives the same plan for the same inputs. Copying before applying
means a failure leaves the session exactly as it was.

## Consequences
- `paste_problem`'s "copied from another session" refusal is replaced by the
  planner's problem string (the interface slice of `ui.editing.commands`).
- A cross-session paste can take as long as copying its files: the
  interface runs it on its import worker with a progress bar, as Import
  Session Data does.
- The clipboard lives in the interface process; it does not survive a
  restart, so the origin folder is never stale beyond that process.
- A destination grows by the pasted sources' whole files, not the pasted
  span: Sonora does not consolidate on paste.

## Alternatives rejected
- **Embedding the audio in the clipboard.** It would copy every sample on
  every Copy, held in memory, for a paste that may never come; the files
  are already on disk in the origin folder.
- **Referencing the other session's file by absolute path.** ADR-0018
  forbids a session naming a file outside its own folder: the session would
  break the moment the stick or the origin moved.
- **Keeping the refusal.** It is a gap against the workflow Sonora
  reproduces, and Import Session Data already solved the hard part.

## Unverified
- The interface wiring (Clipboard's origin folder, the worker, the held
  notice) is the next slice of `ui.editing.commands`; only the planner,
  `bring_in_sources` and `disk_copier` are built and tested here.
- `disk_copier`'s test runs on POSIX only (`SONORA_TEST_DISK_COPIER`); the
  copier is not exercised on Windows.
- A paste from a session whose folder was renamed while the clipboard held
  it is refused as a missing file; whether the operator expects a re-locate
  prompt instead is not decided.
- MIDI sources are not carried: v1 clipboards hold audio regions only.
