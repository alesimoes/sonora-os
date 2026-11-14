# Plan: ui.mixer.views - show/hide strips

Slice ui (S/M, risk low, pleno): QML only, ADR-0130.
- `MixWindow.qml`: `hiddenIds`, per-kind switch, `hiddenCount`, `setStripHidden`,
  `showAll`, reset at `sessionOpened`, compacted columns, "Strips" menu + badge.
- Follow-ups already on main: `sessionOpened` on the unsaved-recording open
  (`session_model_open.cpp`), its test, and the "Sends F-J" text pin
  (`test_mix_layout.cpp`).
- Tests: `test_mix_layout.cpp` (hide, per kind, show all, master pinned,
  not saved, reset at open). Scene `views-hidden` in `mix_views_collapsed.cpp`.
- Open question, default taken: persisting the choice in the session file -
  no (ADR-0130).
