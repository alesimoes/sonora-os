# ADR-0174: Import audio writes first, places once

Status: accepted (2026-11-14). Replaces the never-committed ADR-0073 named in earlier plans.

## Context
`AudioImporter` (ui/qt/src/audio_importer.cpp) converts files to the session rate on a worker thread and then adds tracks, sources and regions. A take must never be lost and a half import must never leave the session pointing at missing files.

## Decision
1. Conversion runs off the UI thread and writes each file as `<name>.part` inside the session folder.
2. A failure or cancel deletes every `.part` written so far; the session is untouched.
3. `commit()` renames all `.part` files, then submits one `Compound` operation ("Import Audio" / "Import Audio Files"), so one undo removes the whole import.
4. If the model refuses the compound (for example the track limit), the renamed files are deleted and nothing is placed. The same applies when the session or its rate changed during conversion, or a rename fails.
5. A finished run that nobody commits is discarded on `reset()`; its copies are not in the session.

## Rationale
Disk work is slow and can fail; the model edit is fast and atomic. Doing the first fully before the second means a failure at any step leaves either the old session or the complete new one. Failing loudly beats a session with dangling sources.

## Consequences
The temporary space of a large import is paid before anything shows. Two cases lacked a deterministic test: model refusal after the rename, and cancel mid-conversion (the test accepted Cancelled or Done). The tests task adds them using progress-bounded waits.

## Alternatives rejected
- Place regions first, convert in the background: a failure leaves regions with missing sources, which is silent loss.
- Convert straight to the final name: a crash leaves a truncated file that looks valid.
