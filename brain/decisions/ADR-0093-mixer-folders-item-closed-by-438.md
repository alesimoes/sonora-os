# ADR-0093 - `ui.mixer.folders` is already delivered by ADR-0048 / #438

- Status: Accepted
- Date: 2026-11-08
- Item: `ui.mixer.folders`
- Relates to: ADR-0048, ADR-0023, ADR-0089 (same pattern)

## Context

The item "Folder tracks and track stacks (routing and basic)" is still `todo`.
Main already has the basic part decided in ADR-0048 section 4: `Folder` and
`SetFolder`/`RemoveFolder` in the model (`common/model`), session format
`folders`, the `folder.new` and `folder.dissolve` commands
(`ui/qt/src/session_model_commands_track.cpp`), `EditController::folder_new`
and `folder_dissolve` (`ui/qt/src/edit_vcas.cpp`), the Mix tags and closed card
(`ChannelStrip.qml`), and the scene `docs/images/sonora-ui-mix-folders.png`.
It arrived with the session-model bundle #438.

## Decision

Close the basic item by reference. No code. The "routing" half (a folder that
is a summing bus) and nesting were named "not done here" in ADR-0048 section 2;
they are not part of this item and become a new BACKLOG line on a
separate, owner-visible decision (they touch the routing graph and the RT path).

## Rationale

Re-implementing a delivered feature would only add a conflicting PR. The
RT path is untouched: a folder is structure and a mute/solo Compound.

## Consequences

The coordinator ticks the item and adds a BACKLOG line "Folder as summing bus,
nested folders" (follow-up, senior/high risk, needs its own ADR).

## Alternatives rejected

- Build the summing folder now: needs a bus-graph contract and RT review; too
  large and undecided for a "basic" item.
- Leave the item open: it would be planned again and again.
