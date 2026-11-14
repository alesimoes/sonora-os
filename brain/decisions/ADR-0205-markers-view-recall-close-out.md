# ADR-0205: Markers view recall close-out (ADR-0137 text never landed)

Status: accepted (2026-11-14)

## Context
ui.editing.markers shipped in #215 (markers, Memory Locations window, recall via transport.locate) and #704 (zoom and track layout stored with a marker). #704's code cites ADR-0137, but no ADR-0137 file is on main; QA asked for its text. The number is spent, so this ADR records the decision.

## Decision
- A marker recalls its position always, and optionally the Edit window's zoom (samples per pixel), left edge, track height and hidden tracks, each behind a per-marker Zoom / Track layout toggle (default on).
- The view part is interface state, not session state: kept per machine in `MarkerViewStore` (Qt-free seam, `marker_views.h`), keyed by session and marker id, never sent to the engine and never in the session file. No protocol, no format.
- A recall whose stored track no longer exists skips that track and still locates; a damaged store file loads as empty. Neither blocks the locate.

## Rationale
Zoom is not part of a take; putting it in the session would need a protocol bump and make a layout tweak an undoable edit. Failing soft is right here: recall is not on the record path and a lost view costs nothing.

## Consequences
Views do not travel with the session to another machine. Not verified on a real UI session beyond test_ui_markers (1/1 on 2026-11-13).

## Alternatives rejected
Storing the view in the session (protocol change, travels, but pollutes undo and the format). A separate Views feature (Pro Tools keeps them in Memory Locations, so users expect one place).

## Open
Marker files over 500 lines to split; edit-markers screenshot scene still fails "session is still opening" (BACKLOG line, shared wait helper).
