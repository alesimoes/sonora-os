# ADR-0129 - Record-mode switches in the Transport window, bar and keymap

Status: accepted (recommended defaults), 2026-11-09.

## Summary
UI half of ADR-0043 (no wire change). The Transport window gets three switches,
Input Only, Pre/Post-Roll and QuickPunch; the transport bar shows a QUICKPUNCH
and an INPUT ONLY chip only while each is on; `keymaps/protools.json` binds the
chords. Every switch sends its ADR-0043 command and is lit from the replica
(`Transport::input_only`, `rolls_on`, `quick_punch`), never from the press, so a
refused or unanswered command leaves it unlit.

## Decisions
1. `TransportModel` exposes `inputOnly`, `rollsOn`, `quickPunch` from the
   settings it already compares each tick; they notify with `settingsChanged`.
2. Window: one row of three `TransportKey`s (`transportInputOnly`,
   `transportPrePostRoll`, `transportQuickPunch`). Pre/Post-Roll is lit when
   rolls are on (its default); the other two when on.
3. Bar: chips only when on, because a mode that changes what Record does must
   be visible, and the default state needs no chip. Pre/Post-Roll off has no
   chip (it is shown in the window).
4. Keys (all `"verified": false`, none confirmed in Avid's guide): `alt+k`
   Input Only, `ctrl+k` Pre/Post-Roll, `ctrl+shift+p` QuickPunch,
   `ctrl+shift+t` `track.toggle_track_punch`.
5. TrackPunch's header button is a later PR: it needs a new QML file or an
   `EditWindow.qml` hook, both held by other features. Its chord works now.
