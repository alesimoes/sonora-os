# ADR-0159 - Take folder: the collapsed lane view is view state

- Status: Accepted (recommended default, architect, 2026-11-12)
- Builds on: ADR-0031 (take lanes and swipe comping), ADR-0026

## Context
ADR-0031 section 6 left the take-folder view (collapsed lanes) for later. The lane UI and Quick Swipe are on main.

## Decision
A take folder is a view of a track's playlists, never stored: no session format bump, no protocol bump. Collapsed or expanded is per-track UI state, lost on close. The collapsed row paints the comped result, coloured per source lane from `model::comp_origin()`. The logic lives in a Qt-free `TakeFolderView`. MIDI comping is out of v1.

## Rationale
Storing it would add a format version for a display choice; computing from the model keeps one source of truth.

## Consequences
Folder state is not undoable and not saved. Colours recompute on each model update (one `comp_origin()` per visible track, off the audio thread).

## Alternatives rejected
- Persist the collapsed flag in the session: a format bump for cosmetics.
- A new "folder" model member: duplicates what playlists already are.
