# Comping exit test: human run checklist

Status: **not yet run.** Nobody has run this checklist on a real build. Every
box below is blank on purpose. A tick is added only by the person who ran the
step, on the commit they ran it on (ADR-0183).

The scripted path is `ui/qt/tests/test_comping.cpp` (`test_exit`). It proves
the model and the window. It does not prove that an engineer finds each
control where Pro Tools puts it, or that the audio is what they expect. This
checklist is that second check, done by a person with ears.

## Before you start

- Build under test: commit `________` (`git rev-parse --short HEAD`).
- Key profile: **Pro Tools** (`keymaps/protools.json`). Select it in the
  window's key profile list. Every key below comes from that file; if a key
  here does not do what it says, the file or this page is wrong, and that is
  a defect to report, not a reason to guess.
- Input: one connected input on the interface (the Behringer UMC1820 is the
  target). With no connected input the engine refuses to record; that refusal
  is itself a step below.
- A vocal or any single phrase you can loop, around 8 bars.
- Keys marked **(mouse)** have no key in the profile. Use the pointer.
- Keys marked **(unverified)** are bound in `protools.json` with
  `"verified": false`. A miss there is a finding against the binding, and the
  test records it.

Record each result as PASS or FAIL in the box, with a note if it is not what
the step says.

## 1. Refusal with no connected input

| Step | Key | Expected | Result |
|---|---|---|---|
| 1.1 Arm track 1 | `shift+1` (Sonora key, track 1) | Track 1 arms. | [ ] PASS [ ] FAIL |
| 1.2 Record with no input connected | `ctrl+space` | Recording does not start. A **held notice** says why and stays until Dismiss. It does not vanish after a few seconds. | [ ] PASS [ ] FAIL |
| 1.3 Dismiss | (mouse) Dismiss | Notice clears. | [ ] PASS [ ] FAIL |

Connect the input before section 2.

## 2. Record three loop takes

Each take is one loop-record pass of the phrase. Each pass makes its own
playlist on the track.

| Step | Key | Expected | Result |
|---|---|---|---|
| 2.1 Loop playback on | `ctrl+shift+l` (or `numpad_4`) | Loop indicator lights. | [ ] PASS [ ] FAIL |
| 2.2 Loop Record mode on | `alt+l` **(unverified)** | Loop Record mode shows on. If nothing shows, record it as FAIL and note the key. | [ ] PASS [ ] FAIL |
| 2.3 Take 1: record | `ctrl+space` (or `f12`) | Recording starts; the loop plays. | [ ] PASS [ ] FAIL |
| 2.4 Take 1: stop after the loop | `space` | Transport stops. Track 1 has one playlist holding the pass. | [ ] PASS [ ] FAIL |
| 2.5 Take 2: record | `ctrl+space` | Recording starts on a new pass. | [ ] PASS [ ] FAIL |
| 2.6 Take 2: stop | `space` | Track 1 now has two playlists. | [ ] PASS [ ] FAIL |
| 2.7 Take 3: record and stop | `ctrl+space`, then `space` | Track 1 has three playlists, one per take. | [ ] PASS [ ] FAIL |
| 2.8 Listen back | `space` from the start | Each take plays back as recorded: no dropouts, no silence where the take should sound. | [ ] PASS [ ] FAIL |

Loop Record off afterwards: `alt+l` again (unverified).

## 3. Show the lanes and make a new comp

| Step | Key | Expected | Result |
|---|---|---|---|
| 3.1 Show the take lanes | `ctrl+alt+shift+t` (Sonora key; `comp.toggle_lanes`) | Track 1's three takes appear as three lanes, each named by its playlist. | [ ] PASS [ ] FAIL |
| 3.2 New comp | `ctrl+alt+n` (Sonora key; `comp.new`) | A new empty playlist named "<name> comp" becomes the one that plays. The playlist that was active is now a lane. | [ ] PASS [ ] FAIL |
| 3.3 Hide and show the lanes again | `ctrl+alt+shift+t` twice | Lanes hide, then show, with no change to the takes. | [ ] PASS [ ] FAIL |

## 4. Swipe a comp across lanes

The gesture is the pointer, not a key. A drag across a lane selects a range on
that lane only. The range is outlined and the **To Main** button on that lane
lights up.

| Step | Key | Expected | Result |
|---|---|---|---|
| 4.1 Swipe lane 1 over bars 1 to 3 | (mouse) drag on lane 1 | Range outlined on lane 1 only. To Main lit on lane 1. | [ ] PASS [ ] FAIL |
| 4.2 Copy it to the comp | `ctrl+alt+c` (Sonora key; `comp.copy_to_main`) | Bars 1 to 3 are now on the comp playlist. Undo is one step. | [ ] PASS [ ] FAIL |
| 4.3 Swipe lane 2 over bars 4 to 6 | (mouse) drag on lane 2 | Range outlined on lane 2. | [ ] PASS [ ] FAIL |
| 4.4 Copy it | `ctrl+alt+c` | Bars 4 to 6 come from lane 2. | [ ] PASS [ ] FAIL |
| 4.5 Pick a whole region with a click | (mouse) click one region on lane 3 | That whole region is picked, not a sub-range. | [ ] PASS [ ] FAIL |
| 4.6 Copy the region | `ctrl+alt+c` | The region is on the comp. | [ ] PASS [ ] FAIL |
| 4.7 Gesture stays in its lane | (mouse) start a drag on lane 1, release over lane 2 | The range stays on lane 1; nothing is taken from lane 2. | [ ] PASS [ ] FAIL |
| 4.8 Quick Swipe (optional) | `ctrl+alt+q` (Sonora key; `comp.toggle_quick_swipe`) | Quick Swipe on. A swipe now copies as the button comes up, with short joins. If a join cannot take a crossfade, a held notice says how many joins were kept as hard cuts. Switch it off again. | [ ] PASS [ ] FAIL [ ] not run |

## 5. Audition the comp

| Step | Key | Expected | Result |
|---|---|---|---|
| 5.1 Play from the start | `space` | The comp plays: bars 1 to 3 from lane 1, bars 4 to 6 from lane 2, the region from lane 3. Each join is clean. | [ ] PASS [ ] FAIL |
| 5.2 Stop | `space` | Stops at the playhead. | [ ] PASS [ ] FAIL |
| 5.3 Loop the swiped range | `ctrl+shift+l` (or `numpad_4`), then `space` | The range the swipe left as the selection loops. | [ ] PASS [ ] FAIL |
| 5.4 Stop the loop | `space`, then `ctrl+shift+l` | Stops; loop playback off. | [ ] PASS [ ] FAIL |

## 6. Consolidate, then undo

Consolidate needs a range on the one selected track (or a selected region).
It renders the range to a new file and places it as one clip.

| Step | Key | Expected | Result |
|---|---|---|---|
| 6.1 Consolidate with no range | `alt+shift+3` (`edit.consolidate`) | Refused with a **held notice** saying a range is needed. It stays until Dismiss. | [ ] PASS [ ] FAIL |
| 6.2 Dismiss | (mouse) Dismiss | Notice clears. | [ ] PASS [ ] FAIL |
| 6.3 Select bars 1 to 6 on track 1 | (mouse) drag on track 1 | Range selected on the one track. | [ ] PASS [ ] FAIL |
| 6.4 Consolidate | `alt+shift+3` | Progress shows "Consolidating… n %" with Cancel. When done, one new region appears. | [ ] PASS [ ] FAIL |
| 6.5 Cancel while running (optional) | (mouse) Cancel during progress | Progress stops; no partial region is placed. Hard to catch on a short range; use a longer one. | [ ] PASS [ ] FAIL [ ] not run |
| 6.6 Undo the consolidate | `ctrl+z` | The new region goes; the comp is back as it was. One undo step. | [ ] PASS [ ] FAIL |
| 6.7 Undo Copy to Main (lane 3) | `ctrl+z` | The region from lane 3 comes off the comp. | [ ] PASS [ ] FAIL |
| 6.8 Undo Copy to Main (lane 2) | `ctrl+z` | Bars 4 to 6 come off the comp. | [ ] PASS [ ] FAIL |
| 6.9 Undo Copy to Main (lane 1) | `ctrl+z` | Bars 1 to 3 come off the comp. | [ ] PASS [ ] FAIL |
| 6.10 Redo | `ctrl+shift+z` | One step back on, in order. | [ ] PASS [ ] FAIL |

Each undo must match exactly one action above. If one undo takes out two
actions, or none, that is a FAIL for the step that caused it.

## Overall

- Sections passed: `____` of 6.
- Steps marked FAIL: list the step numbers and what you saw.
- Keys marked (unverified) that did not do what they say: list them.
- Tester: `____________________`   Date: `__________`   Commit: `__________`

A FAIL is reported with the step number, the commit and the key. It goes to
the owner of the area named in `.claude/routing.toml` for the window, the
model or the keymap. Nothing here is passed until a person has ticked it.
