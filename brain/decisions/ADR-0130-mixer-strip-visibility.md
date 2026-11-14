# ADR-0130 - Mixer strip visibility is view state of the Mix window

Status: accepted (ui-engineer, 2026-11-09)

## Context
`ui.mixer.views` (Pro Tools "Show/Hide Tracks" in the Mix window, Logic's
"Customize the Mixer") needs a way to take strips out of the Mix window: a
48-track session does not fit, and an engineer mixing the drums does not want
the vocal stack beside them. ADR-0048 already gives folders, which close a
folder into one card; that is a structural choice stored in the session.

## Decision
1. Strip visibility is **view state of the Mix window**, not document state.
   It lives in `MixWindow.qml` (`hiddenIds`, keyed `t<i>` track, `a<i>` aux
   bus, `v<i>` VCA) and is never written to the session file, never sent to
   the engine, never part of undo.
2. It **resets at `sessionOpened`**, as `showIO` and `showInsertsAE` do
   (`MixModel::resetViews`), for a session file and for an unsaved recording
   alike. It also clears when the number of tracks, aux buses or VCAs
   changes, because the indices then name other strips: nothing stays hidden
   under another strip's name.
3. A hidden strip **stays in every model**. It keeps playing, its meter keeps
   reading, and it counts in groups, VCAs, folders and solo logic: a hidden
   strip that is soloed still mutes the others. Only its card is not drawn,
   and the x position of every later card (strips, aux returns, folder
   cards, VCAs) closes up: column = slot minus the hidden cards before it.
   Folder-closed (`trackSlots` = -1) and hidden are different things.
4. The **master is pinned** and cannot be hidden: it has no key and
   `setStripHidden` refuses anything that is not `t|a|v` plus an index.
5. A **Strips** button in the view controls opens a menu listing every strip,
   hidden or not, with a switch per strip, per kind (audio, aux buses, VCAs)
   and "Show all". A badge on the button counts what is hidden, so a hidden
   strip can neither be lost nor go unnoticed (a mix that sounds wrong with a
   strip nobody sees is a control that lies). Aux buses are the only bus
   strips this model has; "Bus" and "Aux" are one kind.

## Alternatives rejected
- **Persist in the session file.** Needs a session-format version bump and a
  migration for a preference of the window, and two clients would fight over
  it. Pro Tools keeps it per session; we reset instead, which is safe because
  "Show all" is one click and the badge says it is needed.
- **Remove the strips from the model.** Would silence them, break groups,
  VCAs and solo, and make Show a destructive-undo question. Never.

## Consequences
- UI only: no contract, protocol or session-format change.
- `MixWindow.qml` is over the ~500-line guideline; the additions are the
  minimum and the file is split by a separate refactor PR (the two files
  that would host the split are held by other features this round).
- Existing mix screenshots are refreshed by a follow-up once the sidechain
  work that holds them has merged.
