# ADR-0150 - Names and colours review follow-ups

- Status: Accepted
- Date: 2026-11-09
- Item: `ui.editing.names_colours`
- Relates to: ADR-0023 (update of 2026-11-02, names and colours), ADR-0009 (compound limit)

## Context

Review of the names and colours slice left four small gaps in
`EditController` (`ui/qt/src/edit_names.cpp`).

## Decision

1. `set_track_colour` on a selection needing more than `kMaxCompoundMembers`
   changes is refused whole, with the count and the limit in the message,
   instead of colouring the first 256 and saying nothing. A control that
   half-works lies.
2. `rename_track` also renames the active audio playlist, in the same
   Compound ("Rename Track", one undo step), when that playlist is still
   named for the track (the old name, or the old name plus a `.NN` suffix) and
   the new playlist name is not taken. A playlist the operator named, or one
   whose new name collides, keeps its name; the track rename still goes.
3. `static_assert(theme::kRegionPaletteSize == model::kColours)` lives in
   `edit_names.cpp`, which includes both, so `theme.h` takes no model include.
4. Colouring several regions is deferred: it needs a multi-region selection
   model owned by `ui.editing.commands`.

## Consequences

The undo label of a track rename that carries its playlist is "Rename Track"
(the compound's label), the same text as the single operation's.
