# Interface design language

Status: in force for `ui/qt` from the Edit/Mix MVP (branch
`feature/ui-arrange-mixer`). The single source of every value below is
`ui/qt/src/theme.h`; QML reads it through the `Theme` singleton
(`ui/qt/src/theme_object.h`) and the scene-graph items read it directly.
A view that spells a colour or a size instead of naming a token is a defect.

## What we are after

Owner direction, 2026-10-13: Pro Tools' workflow and window layout as the
functional model (Edit and Mix windows, track order, transport, key
bindings), improved where it is clumsy, with a visual finish at the level
of Logic's: modern and classic, a refined dark theme, a restrained palette
with clear meaning colours, one sans family with tabular figures, a
consistent spacing grid, subtle depth, smooth meters and waveforms, readable
at a glance in a dim control room.

ADR-0004 still draws the line. We take inspiration from how those
applications *feel*; we do not copy their artwork, icons, fonts, colour
values or the pixel layout of any window. Every value here is ours and was
chosen against the contrast targets below, not sampled from a screenshot.

## Principles

1. **Colour means something, once.** Red is record. Yellow is solo. Blue is
   mute. Teal is selection and focus. Green is signal. A colour that means
   two things is read wrongly at 2 a.m.
2. **Quiet chrome, loud state.** Surfaces are near-black greys a step apart;
   only state (armed, soloed, recording, clipped, engine gone) is saturated.
3. **Depth by light, not ornament.** A raised control has a 1 px highlight
   on its top edge (`highlight`); a panel casts a 3 px soft shadow
   (`shadow`). No bevels, gloss, textures or skeuomorphic chrome.
4. **Numbers do not move.** Counters, rulers, gain and pan readouts use
   tabular figures (OpenType `tnum`), so digits do not shimmer as they
   change.
5. **Nothing lies.** A control this build cannot run is drawn *unavailable*
   (45 % opacity) and still answers a click with the reason. A meter with no
   data shows the no-data face, never a frozen bar.

## Colour tokens

Opaque unless noted. Contrast ratios are WCAG 2.1 relative-luminance ratios,
computed from the token values (`text` on `panel` is 14.1:1). Targets:
body text ≥ 7:1, secondary text ≥ 4.5:1, state colours on their
background ≥ 4.5:1, non-text indicators ≥ 3:1.

### Surfaces (darkest to lightest)

| Token | Value | Use |
|---|---|---|
| `sunken` | `#0D0F12` | wells: meter backgrounds, counters, fader slots, readouts |
| `window` | `#121417` | the window behind everything |
| `laneB` / `laneA` | `#111316` / `#14171B` | alternate timeline lanes |
| `panel` | `#1A1D22` | transport, toolbar, track headers, strips |
| `raised` | `#252930` | buttons at rest, selected header or strip |
| `hover` | `#2E333B` | a raised surface under the pointer; fader caps |
| `borderSubtle` | `#2A2F36` | hairlines between things |
| `borderStrong` | `#3A4048` | the edge of the selected strip, fader caps |
| `highlight` | `#FFFFFF` at 7 % | top edge of raised controls |
| `shadow` | `#000000` at 45 % | the soft edge under the transport bar |

### Text

| Token | Value | On `panel` | On `raised` | Use |
|---|---|---|---|---|
| `text` | `#E8EBEF` | 14.1 | 12.2 | names, values, labels that matter |
| `textSecondary` | `#A3ABB5` | 7.3 | 6.3 | ruler labels, unselected track names |
| `textTertiary` | `#7C8590` | 4.5 | *3.9, not allowed* | captions, field labels; never on `raised` |
| `textOnAccent` | `#121417` | — | — | text on a lit button (8.3:1 on `accent`, 11.0 on `solo`, 6.7 on `mute`, 5.3 on `record`) |

### Meaning

| Token | Value | On `window` | Means |
|---|---|---|---|
| `accent` | `#3FC1B0` | 8.3 | selection, focus, the insertion cursor, loop/punch on |
| `record` | `#F0524F` | 5.3 | record arm, the record button, the playhead while recording |
| `solo` | `#F2C14E` | 11.0 | solo |
| `mute` | `#6B9BF2` | 6.7 | mute; outline only when muted implicitly by another track's solo |
| `input` | `#4CC38A` | 8.3 | input monitoring (reserved; not yet drawn) |
| `caution` | `#E8B04B` | 9.4 | the status line, synthetic-data labels, gain above 0 dB |
| `alarm` on `alarmSurface` | `#FF5A5F` on `#3A1618` | 5.3 | banners: engine gone, software rendering |
| `playhead` | `#F5F7FA` | 16.8 on a lane | the playhead while playing |
| `bypass` | `#9D7CF0` | 5.8 | a bypassed insert (filled, `textOnAccent` on it at 5.8:1) |

## Typography

One family: **Inter** (SIL Open Font License, Debian `fonts-inter`), with
DejaVu Sans as the fallback that every Debian image has. Inter is in the UI
build image (`tools/Dockerfile.ui`); adding it to the appliance's package
list is open (it is an ISO change, outside this PR).

| Token | px | Weight | Use |
|---|---|---|---|
| `textOverline` | 9 | Regular, upper case, +0.6 px tracking | field labels over a value (START, END) |
| `textCaption` | 10 | Regular / DemiBold | ruler labels, meter and fader scales, region names |
| `textSmall` | 11 | DemiBold for buttons | button captions, secondary values |
| `textBody` | 13 | DemiBold for names | track names, the status line |
| `textTitle` | 15 | — | transport glyphs, window tabs |
| `textCounter` | 28 | Medium, `tnum` | the main counter |
| `textDisplay` | 40 | Bold, outlined | NO DATA over dead meters, nothing else |

## Spacing and shape

A 4 px grid: `space1` 4, `space2` 8, `space3` 12, `space4` 16, `space6` 24.
Margins and gaps take a token; nothing sits at 5 or 7 px. Corner radius
`radius` 4 for controls and panels, `radiusSmall` 2 for chips and slots.

Fixed dimensions: transport bar 64, edit toolbar 40, each ruler 20, track
header 216 wide, track lane 64 tall, region name band 14.

## Component states

| Component | Rest | Hover | Active | Unavailable |
|---|---|---|---|---|
| Button (`SButton.qml`) | `raised`, `borderSubtle`, `text`, highlight edge | `hover` | filled with its meaning colour, `textOnAccent`, no highlight | 45 % opacity; click still reports why |
| Record arm | as button | as button | `record` | 45 % while the engine is gone |
| Solo | as button | as button | `solo` | — |
| Mute | as button | as button | `mute`; outlined in `mute` when another track's solo silences it | — |
| Edit mode, tool | segmented row of buttons | — | `accent` | — |
| Track header | `panel`, `textSecondary` name | — | selected: `raised`, `text` name | — |
| Channel strip | `panel` | — | selected: `raised`, `borderStrong` | — |
| Fader cap | `hover`, `borderStrong`, a `text` line at the value | — | dragging (no change: the cap follows the hand) | — |
| Insert slot (`InsertSlot.qml`, `insertSlotHeight` 16) | empty: `sunken` well, the slot letter in `textTertiary`; running: `raised`, name in `text` | `hover` | bypassed: filled `bypass`, `textOnAccent`; inactive: `panel`, name in italic `textSecondary`; faulted: `alarmSurface`, `alarm` border and "! name"; drop target: `accent` border | — |

Insert slots read, loudest first: fault, bypass, inactive, running, empty.
Under the ten slots the chain's delay reads `dly N` in samples (from the
state block; `dly –` with no engine); the session's delay compensation reads
at the Mix window's top right, in `alarm` when it is clamped short of what
the inserts need. The plugin chooser lists a plugin that cannot go on the
track dimmed, with the reason in `caution`, never hidden.

## Timeline

- Lanes alternate `laneA` / `laneB`, a `borderSubtle` hairline beneath each.
- Bar lines are always drawn faintly (`gridBeat`); in Grid mode the grid
  value's lines are drawn, bars in `gridBar`.
- **Region colour** comes from the track's display index, cycling through
  eight muted mid-tones: blue `#4C7FD9`, teal `#2FA39A`, green `#5BA85A`,
  olive `#A3A34A`, orange `#D9873E`, red `#C9545A`, violet `#8C6BD1`, slate
  `#6B8299`. The track header shows the same colour as a 3 px bar, the strip
  as its cap.
- A region is drawn from its palette colour `p`:
  name band `mix(p, sunken, 0.40)` with the name in `text` (≥ 4.87:1 for all
  eight); body `mix(p, sunken, 0.62)`, selected `mix(p, sunken, 0.45)` with a
  1.5 px `accent` outline; waveform `mix(p, white, 0.35)` (≥ 4.88:1 on the
  body). A muted region is grey (`#2A2E34`) with a `textTertiary` waveform.
- Waveform overview: one min/max column per pixel, centred, scaled by the
  vertical zoom, from a `PeakSource` (synthetic until peak files exist; the
  toolbar says so in `caution`).
- **Fades** are drawn as the engine plays them: the gain curve
  (`fade_gain`, which calls `sonora::fade_in_gain` in common/base's
  `sonora/fade_curve.h`, the one definition the engine plays) as a 1.5 px `fadeLine`
  (`#E8EBEF`) across the waveform area, and what the fade takes away above it
  darkened by `fadeShade` (black at 55 %). A **crossfade** is a box over the
  span both regions sound in, `crossfadeFill` (text at 12 %) with a 1 px
  `crossfadeEdge` (text at 50 %), both curves inside it. A crossfade replaces
  the fades of the ends it joins, so those are not drawn: a curve nobody
  hears would be a control that lies. The Edit toolbar shows the shape the
  no-dialog fade keys make (FADE, X-FADE); a click picks the next one.
- A time selection is `accent` at 20 % over the lane.
- The insertion cursor is a 1 px `accent` line while stopped; the playhead
  a 2 px `playhead` line while playing, `record` while recording.

## Meters

| Element | Token | Value | On the well |
|---|---|---|---|
| Well | `meterWell` | `#0D0F12` | — |
| No data | `meterNoData` | `#2A2E34` | — |
| Bar to −18 dBFS | `meterGreen` | `#3DD68C` | 10.2:1 |
| −18 to −6 dBFS | `meterAmber` | `#F2B33D` | 10.3:1 |
| Above −6 dBFS | `meterRed` | `#F0524F` | 5.5:1 |
| Peak hold | `meterHold` | `#F5F7FA` | 17.9:1 |
| Over, latched | `meterOverOn` | `#FF3B3B` | 5.4:1 |
| Over, clear | `meterOverOff` | `#2B3036` | — |
| Scale marks | `meterMark` | `#4A525B` | — |

Ballistics are unchanged from `meter_model.h`: instant attack, release at
IEC 60268-18's 20 dB in 1.7 s, 2 s peak hold then the same release, the over
latched until the operator clicks it. Colour zones break at −18 dBFS (a
common line-up level) and −6 dBFS. The scale gives the top 20 dB half the
height.

## Fader

Our own taper (`fader_taper.h`): +12 dB at the top, 0 dB at 78 % of the
travel, −10 at 58 %, −20 at 42 %, −40 at 20 %, −60 at 8 %, −inf at the
bottom; piecewise linear in dB, monotonic and exactly invertible. Drag rides
it, Ctrl-drag moves a tenth as far, Alt-click returns to 0 dB. Gain reads
`+3.5`, `0.0`, `−12.0`, `-inf`; above 0 dB the readout turns `caution`.
Pan reads `<100` … `C` … `100>` and draws as a bar filled from the centre.

## Open

- Inter in the appliance image (ISO package list).
- Light theme: not planned. A control room is dark.
- High-DPI: every size is in device-independent pixels; untested on a
  scaled display.
