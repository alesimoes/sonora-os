# UI design audit, 2026-10

Status: research note, written 2026-10-31 against `origin/main` at
`1243f637`. It audits the Qt interface's visual quality against
[the design language](../specs/ui-design.md) and the owner's direction
(Pro Tools workflow improved, a finish at the level of Logic, dark, read from
a control-room distance on a 1080p to 1440p screen, 16 to 48 tracks on a
UMC1820). The spec amendments it calls for are in `ui-design.md`, marked
"Update, 2026-10-31"; the work is sequenced in `planning/BACKLOG.md`, Track 3
item 7.

Method. Every `docs/images/sonora-ui-*.png` (37 captures, all 1280×720) was
opened and measured in pixels; causes were confirmed in `ui/qt/qml/` and
`ui/qt/src/theme.h`. Contrast ratios are WCAG 2.1, computed from the token
values. Measurements are from the 1280×720 captures unless a line says
otherwise. Nothing here is sampled from another product's artwork: the bar is
the workflow and the finish, described in our own terms (ADR-0004).

Severity: **P1** hurts daily use, **P2** looks unprofessional, **P3** polish.
The BACKLOG refers to finding `#n` as Fn.

## Findings

### Mix window

| # | Screen | Issue (measured) | Sev | Spec |
|---|---|---|---|---|
| 1 | mix-sends, mix | Fader throw is about 70 px of a 525 px strip (13 %); the 10 insert and 5 send slots take 255 px (49 %). Before sends existed (mix-monitor, an older capture) the throw was about 150 px: each section added since has come out of the fader, because `faderArea` is whatever `ChannelStrip.qml` leaves after fixed-height sections. | P1 | missing: strip anatomy and height budgets |
| 2 | mix-* | Strip width is `floor(window / (tracks + auxes))` (`MixWindow.qml:91`): 57 px for 20 strips at 1280. At 48 tracks on 1920 px it would be about 37 px, with no horizontal scroll and no width presets. | P1 | missing: strip widths (`ui.mixer.views`) |
| 3 | mix-* | R/I/S/M are 12 px squares at a 57 px strip (`stateButton` clamps to 12..18 px). WCAG 2.2's target-size minimum is 24×24 px. The record-arm and solo buttons are the ones hit fastest. | P1 | Component states: no minimum size |
| 4 | mix, mix-sends | The master strip reads `NO DATA` over both meters (bus peaks need state-block layout 3, `ui.mixer.metering`). The no-data face is the honest answer, but the master meter is the one an engineer always watches. | P1 | Meters (engine gap, tracked) |
| 5 | mix-sends | Aux strips have no meter, no solo and no inserts (left to `ui.mixer.io`). Their empty insert area looks like a missing rendering, not a strip kind. | P1 | missing: strip kinds |
| 6 | mix-* | 15 empty slots per strip, each a bordered box with its letter in `textTertiary`: the 20 strips in mix-sends have 280 slots, about 30 of them in use, so about 250 boxed letters are on screen at once and the empty slots outnumber the used ones about 8 to 1. | P2 | Component states: insert slot "empty" |
| 7 | mix-sends | Names are cut with an ellipsis: "Bass …", "Lead …", "Talkb…", and "Ve…" and "De…" on the aux strips, which give 16 px of the name plate to a remove `×`. Insert names are abbreviated differently from capture to capture ("FkCmpr" here, "FkCmprs" in mix-monitor) because the abbreviation depends on the width. | P2 | missing: truncation rules |
| 8 | mix-* | Track, aux and master strips share one surface (`panel`), one cap and one name plate. The master differs only by width (92 px) and an 8 px gap. Its header reads "Out 1-2" and its name plate reads "Monitor". | P2 | missing: strip kinds |
| 9 | mix-* | Pan is an 8 px tall bar 40 px wide, drawn as a slider, with a 16 px readout. It cannot be read from the mix position and leaves no room for a stereo pair's two pans. | P2 | Fader section: pan drawn as a bar |
| 10 | mix-sends | The meter is 10 px wide with a 9 px scale (`textOverline`). The scale marks (`meterMark` `#4A525B`) are 2.42:1 on the well, under the spec's own 3:1 target for indicators. At strip widths under 56 px the fader scale labels sit under the fader cap ("-10" is hidden in mix-sends). | P2 | Meters: mark contrast; Typography: 9 px is for field labels |
| 11 | mix-* | A selected strip is told apart by `raised` on `panel` (1.16:1) and a `borderStrong` edge (1.61:1). In a 48-strip mixer you cannot find it at a glance. | P2 | Component states: channel strip |
| 12 | mix-panel | The generic plugin panel docks over the right 420 px and covers strips 14 to 18 and the master. Its parameters read in percent ("Threshold 32 %"), not in the plugin's units. Bypass, Make inactive and Remove are three buttons of equal weight in a row, with no separation for the destructive one. | P2 | missing: plugin window chrome |
| 13 | (none) | No capture shows a plugin's native editor window or its chrome: `plugins.host.editors` is still `todo`. mix-panel is the only plugin window captured. | P2 | missing: plugin window chrome |
| 14 | mix-browser | Reasons for a dimmed plugin are cut off ("which no out-of-proces…") with no way to read the rest. | P3 | missing: truncation rules |

### Top bar and transport

| # | Screen | Issue (measured) | Sev | Spec |
|---|---|---|---|---|
| 15 | edit-tracks, edit-new-tracks | At 1280 px the left row runs past the edge ("Take 1 / xruns / disk o" is cut at x = 1280). `rightInfo` gets `max(0, remaining)` (`TransportBar.qml:320`), so at width 0 the alarm-coloured health line (`healthText`), the key profile, the rate and buffer, and the render path all disappear. In older captures they survive only cut from the left ("…eters show no data", "…TERISER) · meters 30 Hz"). An alarm that is the first thing to go when space runs out is a refusal the operator never sees. | P1 | missing: top bar priorities and minimum width |
| 16 | all | The main counter is 28 px, with a cap height of about 20 px. On a 24-inch 1080p panel (0.277 mm/px) that is 5.6 mm; at 1.5 m it subtends about 13 arcmin. Track names (13 px) subtend about 6 arcmin, and the xruns line (10 px) about 4.5. The type scale is sized for a desk at 60 cm, not for a control room. There is no UI scale setting, and the spec's "High-DPI untested" still stands. | P1 | Typography: no distance tier |
| 17 | edit vs mix | One top bar, three vintages in the docs: "Save" or "File ▾" in the same slot, and a "Transport" tab that is present in some captures and absent in others. "Open" is both a button and a File menu item. The window tab "Edit" and the menu "Edit ▾" share a label 300 px apart. | P2 | missing: top bar layout |
| 18 | transport* | "Transport" sits in the tab row as a peer of Edit and Mix but opens a floating window. That window covers the Edit toolbar's START/END/LENGTH and repeats the main counter. | P2 | missing: window model |
| 19 | all | The STOP / REC status chip is drawn as a raised 15 px bold button. A status that looks clickable invites a click that does nothing. | P2 | Component states: no status-chip style |
| 20 | all | The key-hint strip clips mid-word at the right on every capture ("numpa…", "ctrl+b…") and takes 30 px of height on every screen. | P3 | missing |

### Edit window

| # | Screen | Issue (measured) | Sev | Spec |
|---|---|---|---|---|
| 21 | edit-tracks | Track colour follows the display index (`region_colour(index)`). Duplicating Snare shifted every track below it: Hi-Hat went from green to orange, Tom 1 from olive to red. With 16 to 48 tracks, colour stops being a way to find a track. | P1 | Timeline: "Region colour comes from the track's display index" is wrong |
| 22 | edit* | The track header's meter is about 3 px wide at the header's right edge. It is unreadable at any distance, and the spec does not define it. | P2 | Meters: no header meter |
| 23 | edit* | 64 px lanes fit 6.5 tracks at 720 px and about 13 at 1080. The captures show no track-height choice; 16 to 48 tracks need small, medium and large heights. | P2 | Spacing: one fixed track height (`ui.editing.zoom`) |

### Colour meaning

| # | Screen | Issue (measured) | Sev | Spec |
|---|---|---|---|---|
| 24 | mix-input-monitor | Input monitoring `input` `#4CC38A` and the meter's `meterGreen` `#3DD68C` have the same hue (151°). A lit I button reads as signal. | P2 | Principle 1 |
| 25 | markers, mix-* | Solo `#F2C14E` (42°), caution `#E8B04B` (39°), marker `#E0A83C` (40°) and meter amber `#F2B33D` (39°) are four meanings within 3° of one hue. `kMarker`, `kMarkerRange` and `kConductor` exist in `theme.h` but not in the spec. | P2 | Principle 1; Colour tokens incomplete |
| 26 | transport-punch vs repairs | Punch-on is `record` red in the current build and `accent` in an older capture. The spec says accent. | P2 | Meaning table |
| 27 | latency, edit-conductor | Green also means "measured OK" (latency) and tempo or meter changes (`kConductor` `#8CCF7E`, 110°). | P3 | Principle 1 |
| 28 | edit-no-engine, refusal-reconnect | `NO DATA` is drawn at 40 px in `alarm` at the centre of the window (`Main.qml:117`), over regions and waveforms. The spec puts it over dead meters only. | P2 | Typography: `textDisplay` |
| 29 | mix-*, latency, edit | `accent` means selection and focus, and also every lit mode toggle (tab, edit mode, tool, Sends A-E, PRE, FMP, Click, Count-off, Loop) and the primary dialog button. The spec mentions only selection, focus and loop or punch. | P3 | Meaning table |

### Windows, dialogs, menus

| # | Screen | Issue (measured) | Sev | Spec |
|---|---|---|---|---|
| 30 | dialogs | Five ways to close a window: `×` (Send, Latency, Memory Locations, Undo, Transport), "Close" (Keys, Plugin browser), "Done" (insert panel), "Cancel" (New Tracks, Repeat, Save Copy In), "Leave in Audio Files" (Recovery). The primary button is filled in Repeat, Save Copy In and Recovery but not in New Tracks. | P2 | missing: window and dialog chrome |
| 31 | dialogs | Modal dialogs draw over a live timeline with no scrim, so modal and modeless look the same. | P2 | missing |
| 32 | file-menu, clipboard | Menu items are centred, each in its own boxed button, and show no key equivalents. A desktop DAW's menus teach the key map; here only the clipping hint strip does. | P2 | missing: menus |
| 33 | markers, mix-sends | Destructive actions take one click and look like everything else: the `×` on each Memory Locations row, the `×` on the aux name plate (removes the bus and every send to it), and Remove beside Bypass. Undo exists, so this is not a P1. | P3 | missing |

### Capture rig

| # | Screen | Issue (measured) | Sev | Spec |
|---|---|---|---|---|
| 34 | 34 of 37 | A 30 px `alarm` banner, "Software rendering…". It is true of the headless rig (no GPU) but not of the product, and it is the loudest element on every capture the owner reviews. | P2 | missing: capture standards |
| 35 | all | Every capture is 1280×720 (`main.cpp:349,624`; Xvfb and cage in `build-ui-entrypoint.sh`), below the 1920×1080 to 2560×1440 target. Budgets and type cannot be judged at the size they are meant for. | P2 | missing: capture standards |
| 36 | mix-browser, mix-monitor, mix-panel, keys, edit-no-engine | These are stale: they come from an earlier build (no Transport tab, no I button, the old top-right block). The docs show at least three UI vintages side by side. | P2 | missing: capture standards |
| 37 | (none) | No capture at 48 tracks, the density the target hardware implies. | P2 | missing |

## Already good: keep it

- **Token discipline.** One `theme.h`, read by both QML and the scene graph.
  The spec's contrast table checks out: `text` is 14.1:1 on `panel`,
  `textSecondary` 7.3, `textTertiary` 4.5 and 5.1 on `sunken`.
  `textTertiary` is correctly swapped for `textSecondary` on a selected
  (`raised`) header and on the selected strip's scale.
- **Insert state ladder.** Fault ("! PltR" in `alarm` on `alarmSurface`),
  bypass (filled violet, 5.8:1), inactive (italic) and running are told apart
  without reading.
- **Meters.** IEC 60268-18 release, a 2 s hold, a latched over, colour zones
  at −18 and −6 dBFS, and every value at or above 5.4:1 on the well.
- **Honest states.** The no-data face, the refusal page (one message, the
  cause in `alarm`, no shell), the plugin chooser that dims an unusable
  plugin and says why, and the implicit-mute outline and the solo-safe
  padlock in mix-solo-safe.
- **Recording is unmistakable.** The counter turns `record`, the REC chip
  and the transport button light, and the playhead turns red: three
  redundant cues.
- **Timeline drawing.** Region name bands at ≥ 4.87:1, waveforms at ≥ 4.88:1
  on the body, fades drawn as the engine plays them, a crossfade box, and
  overline labels (START, END, LENGTH, GRID, FADE) in tabular figures.
- **Edit toolbar grouping.** Modes, tools, grid and fades in segmented
  groups, read left to right as an engineer works.
- **Transport window hierarchy.** One large counter, secondary formats in a
  quiet column, and tempo-map chips.
