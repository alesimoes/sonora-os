# Pro Tools workflow parity audit, 2026-11-01

Why: the owner finds the DAW's usability well below Pro Tools. This note walks
a recording engineer's first hour of a tracking session and records, for each
step, what Sonora does today against the Pro Tools behaviour. Scope is the
**workflow** only (ADR-0004): nothing here comes from decompiling or copying
Pro Tools code or assets.

## Method and its limits

- Read: `keymaps/protools.json`, the QML under `ui/qt/qml`, the controllers in
  `ui/qt/src`, the screenshots in `docs/images` (edit, mix and transport scenes
  at 1280x720 and 1920x1080), the section headings of
  `brain/specs/ui-design.md`, and the open lines of `planning/progress.json`
  through `tools/progress.sh`.
- Not run: the app. No new screenshots were taken. A status is therefore read
  from code, command names and the committed pictures; anything I could not
  see in those is marked "unverified". Re-check a row before building on it.
- Pro Tools behaviour is cited to Avid's published *Pro Tools Shortcuts Guide*
  (PT-A 2025.6, PT-B 2021.6, PT-C 12.5; URLs in
  `brain/research/protools-logic-key-bindings.md`, Sources) and to the *Pro
  Tools Reference Guide* chapter named in each row. The Reference Guide
  chapter names are from memory of the published guide and were not re-checked
  against it.
- Excluded on request: Rec arms only and Play starts the take; a live growing
  waveform during recording. Both are being fixed elsewhere.

Status key: works / partly / missing / differs. Effort: S under a day, M a few
days, L a week or more.

## Findings, in the order an engineer hits them

Row 1 is hit earliest and most often in the first hour.

| # | Area | Status | Pro Tools behaviour (source) | Sonora today | Effort |
|---|---|---|---|---|---|
| 1 | Record-enable per click, plus modifiers | partly | Click a track's Rec button to arm; Option-click arms/disarms every track, Shift-click the selected tracks (Reference Guide, Recording; Shortcuts Guide, Track Controls). | `R` button per strip; `track.toggle_arm`, `arm_all`, `disarm_all` exist on Sonora-origin chords. No click modifiers. | S |
| 2 | Input monitoring Auto / Input | partly | Default Auto Input: a track monitors input when stopped or recording and tape when playing; Input Only is per track (the I button) and a global option (Reference Guide, Recording). | Per-track `I` button and `track.toggle_input_monitor`. Auto behaviour on transport state is open under `ui.record.punch_loop`; no global Auto/Input choice. | M |
| 3 | I/O Setup | missing | A window mapping hardware inputs and outputs to named paths, with presets; tracks pick inputs from them (Reference Guide, I/O Setup). | No window. `ui.mixer.io` (doing) adds a strip I/O view, but nowhere names hardware inputs 1-18 of the UMC1820. | M |
| 4 | New Session dialog | partly | Name, location, file type (BWF), sample rate, bit depth, I/O settings (Reference Guide, Sessions). | New session exists in the File menu. No dialog for sample rate or bit depth; the rate comes from the engine Hello. Bit depth appears nowhere in the UI sources (engine side unverified). | M |
| 5 | New Tracks dialog | partly | Track > New: count, mono/stereo, Audio/Aux/Master/MIDI, timebase (Reference Guide, Tracks). | New Tracks panel asks for a count (`sonora-ui-new-tracks.png`). Type and format choices not seen (unverified). | S |
| 6 | Transport window and counters | works | Main and Sub counters in Bars|Beats, Min:Secs, Timecode, Samples; Start/End/Length (Reference Guide, Transport). | Main counter with format switch, Transport window with all four formats, tempo/meter, pre/post-roll, loop and punch; Edit toolbar has Start/End/Length. | - |
| 7 | Transport keys | works | Space plays/stops; Return to start; keypad 0-4 in Transport mode; Shift+Space half speed (PT-A, Transport). | In `protools.json`; Ctrl+Space and F12 record. | - |
| 8 | Edit modes Shuffle/Slip/Spot/Grid | partly | F1-F4, `~` cycles; Grid has absolute and relative; Spot asks for a time (Reference Guide, Edit Modes). | Four mode buttons, F1-F4, grave. Grid value cycles on a button. No Spot dialog, no Relative Grid found (unverified). | M |
| 9 | Tools and Smart Tool | works | F5-F10 pick Zoomer, Trim, Selector, Grabber, Scrubber, Pencil; Command+7 Smart Tool; corners give fades, upper half selects, lower half grabs (Reference Guide, Edit Tools). | Keys bound; Smart/Trim/Select/Grab/Pencil in the toolbar; fade and trim hit zones in `edit_controller.h`. Scrubber has a key but no toolbar button, so it may do nothing visible (unverified). | S |
| 10 | Edit vs timeline selection, link | partly | The two selections can be linked (Link Timeline and Edit Selection, Shift+/) (Reference Guide, Selecting). | `ui.editing.selection` ("linked timeline/edit selection") is `doing`. No link toggle seen. | S |
| 11 | Zoom | partly | R/T zoom with Commands Focus, Zoom Toggle E, five presets, zoom to selection (PT-A, Zoom). | `zoom.in/out/toggle/fit_selection/fit_session/vertical_*` bound; presets absent. In/out are live only in Commands Focus. | S |
| 12 | Count-off, click, pre/post-roll | partly | Click and Count Off toggles in the Transport window; Setup > Click/Countoff; Pre-roll toggle beside the pre-roll value (Reference Guide, Transport). | Click, Count-off and pre/post-roll fields in the Transport window (`sonora-ui-transport-click.png`). Pre-roll on/off is an open `ui.record.punch_loop` line. No click sound settings seen. | S |
| 13 | Loop playback and loop record | partly | Loop Playback and Loop Record; each loop record pass is a take (Reference Guide, Recording). | Loop and Loop Rec buttons, `transport.toggle_loop_record`. Takes per pass are `ui.comping.loop_takes` (todo). | M |
| 14 | Playlists and comping | partly | Playlist selector per track; new/duplicate playlist; Show Playlists as lanes; promote a lane to main (Reference Guide, Playlists). | Playlist menu: new, duplicate, choose, rename, delete unused. No lanes, no promote. `ui.comping.lanes` is L and todo. | L |
| 15 | Markers / memory locations | partly | Keypad Enter stores a location; period, number, period recalls (Reference Guide, Memory Locations). | Add marker, Memory Locations window, "+" in the ruler. Recall by number needs chord sequences the schema cannot express (research note, section 4). | M |
| 16 | Punch | partly | QuickPunch and TrackPunch; punch range from the selection (Reference Guide, Recording). | Punch Set/Clear and range from selection. TrackPunch and QuickPunch are open lines. | M |
| 17 | Armed track meters its input while stopped | unverified | Armed tracks meter input while stopped so the preamp can be set (Reference Guide, Recording). | Strip meters exist; I did not confirm they follow input for an armed, stopped track. If not, it is a day-one problem. | M |
| 18 | Mixer: faders, pan, sends, inserts | works | Mix window strips with Inserts A-E/F-J, Sends A-E/F-J, I/O, fader, pan, R/S/M (Reference Guide, Mixing). | The mix scene shows this, plus solo-safe and a delay compensation readout. | - |
| 19 | Track name, colour, height | partly | Double-click name; track colour; heights Micro to Extreme (Reference Guide, Tracks). | Names and colours show. `ui.workflow.windows` and `ui.editing.names_colours` are todo. | S |
| 20 | Fades and crossfades | partly | Smart Tool handles; Fades dialog; D/F/G in Commands Focus (PT-A, Fades). | Corner handles, `edit.fade_to_start/end/selection`, a fade shape selector. No Fades dialog. | S |
| 21 | Separate / Heal / Consolidate | partly | Command+E separates, Command+H heals, Option+Shift+3 consolidates (PT-A, Edit). | All three are bound. My grep found no handler for `edit.heal_separation` or `edit.consolidate` outside `contracts/api/.../command.h` and the key maps. Unverified; may be bound to nothing. | S |
| 22 | Tab to transient / next boundary | missing | Tab moves to the next clip boundary, or transient with Tab to Transients on (PT-A, Edit). | No Tab binding, no transient detection. | M |
| 23 | Nudge | works | Keypad plus and minus nudge by the Nudge value (Reference Guide, Nudge). | `nudge.forward/back`; nudge scene. | - |
| 24 | Clip gain | missing | Clip Gain line on each clip (Reference Guide, Clip Gain). | `ui.editing.clip_gain` is todo. | M |
| 25 | Import audio | missing | File > Import Audio and the workspace browser (Reference Guide, Importing). | `ui.editing.import` todo. The only browser is for plug-ins. | M |
| 26 | Undo and history | works | Undo/redo; Undo History window. | Undo History window and scene. | - |
| 27 | Preferences | missing | Setup > Preferences: Operation, Editing, Display, Processing. | `ui.workflow.preferences` todo. | S |
| 28 | Playback engine / buffer | partly | Setup > Playback Engine: device, buffer size. | Latency window only; no device or buffer choice in the UI. | M |

## Ranking logic

First hour of tracking: make or open a session, create and name tracks, map
inputs to them, arm, set levels, set click and count-in, record, play back,
fix a take. The top of the list is therefore the path from an empty session and
an 18-channel interface to a recorded take with a set level. Mixing and fine
editing come later, and most of that already works.

## Top 15 BACKLOG lines

Ids marked (new) are not in `progress.json`; the others extend an open item
and its owner should decide whether to fold them in. Check with
`tools/progress.sh get ID` before pasting.

```
- [ ] ui.workflow.io_setup (new): Setup > I/O Setup window: name the hardware inputs and outputs (1-18 on the UMC1820), keep the map with the session, and pick a track's input from the named paths. A track with no input mapped says so on its strip. Effort M. Source: Avid Pro Tools Reference Guide, I/O Setup.
- [ ] ui.record.arm_modifiers (new): Click on a track's R button arms it; Option-click arms or disarms all tracks; Shift-click arms the selected tracks; the same on the I button. Effort S.
- [ ] ui.record.auto_input: Auto Input monitoring as the default (input when stopped or recording, playback when playing), per-track Input Only, one global toggle in Setup. Folds into ui.record.punch_loop. Effort M.
- [ ] ui.record.armed_meter (new): An armed track meters its input while stopped so the preamp can be set; unarmed tracks meter playback. Confirm today's behaviour first. Effort M.
- [ ] ui.workflow.new_session_dialog (new): File > New Session dialog: name, location, sample rate (44.1, 48, 88.2, 96 kHz), bit depth (16, 24, 32 float). Say how the engine handles a rate change. Effort M. Source: Reference Guide, Sessions.
- [ ] ui.workflow.new_tracks_types (new): New Tracks panel adds mono/stereo, Audio/Aux/Master, and a name. Effort S.
- [ ] ui.record.preroll_toggle: Pre-roll on/off in the Transport window with a key, and count-off honoured on a record pass. Folds into ui.record.punch_loop. Effort S.
- [ ] ui.editing.tab_to_transient (new): Tab and Ctrl+Tab move the edit cursor to the next/previous clip boundary and, with Tab to Transients on, the next transient. Detection needs a spec first. Effort M. Source: Shortcuts Guide, Edit.
- [ ] ui.editing.heal_consolidate_check (new): Confirm `edit.heal_separation` and `edit.consolidate` act in the Qt UI; wire them or take them off the key map. Effort S.
- [ ] ui.editing.zoom_presets (new): Zoom presets 1-5 stored and recalled, Zoom Toggle on E, R/T live where Pro Tools has them. Effort S. Source: Shortcuts Guide, Zoom.
- [ ] ui.editing.link_selection: A visible Link Timeline and Edit Selection toggle (Shift+/) with its state in the toolbar. Folds into ui.editing.selection. Effort S.
- [ ] ui.editing.markers_recall: Recall a memory location by number from the keypad. Needs the chord-sequence work in the key-bindings note, section 4. Effort M.
- [ ] ui.editing.spot_dialog (new): Spot mode opens a dialog for a start time in the chosen format; Relative Grid as a second Grid mode. Effort M. Source: Reference Guide, Edit Modes.
- [ ] ui.comping.promote_playlist: Promote a playlist or lane to the main playlist, ahead of the lane view in ui.comping.lanes (L). Effort M.
- [ ] ui.workflow.setup_menu: Setup grows from Latency only to Playback Engine (device, buffer), Click/Countoff and Preferences. Folds into ui.workflow.preferences. Effort M.
```

## Open questions

- Do `edit.heal_separation` and `edit.consolidate` have a handler in the Qt UI?
  Row 21 says I found none; an indirect lookup could have hidden one.
- Is bit depth a session field in the engine? If not, the New Session dialog
  needs an engine change first.
- Does the Scrubber tool do anything without audio scrubbing in the engine? If
  not, F9 should refuse loudly: a control that lies is worse than a missing one.
- The Reference Guide chapter names are unquoted; a later pass should quote
  the section.
