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
| `accent` | `#3FC1B0` | 8.3 | selection, focus, the insertion cursor, loop on (punch on is `record`, Update 2026-10-31) |
| `record` | `#F0524F` | 5.3 | record arm, the record button, the playhead while recording |
| `solo` | `#F2C14E` | 11.0 | solo |
| `mute` | `#6B9BF2` | 6.7 | mute; outline only when muted implicitly by another track's solo |
| `input` | `#4CC38A` | 8.3 | input monitoring: the I button and its armed state |
| `caution` | `#E8B04B` | 9.4 | the status line, synthetic-data labels, gain above 0 dB |
| `alarm` on `alarmSurface` | `#FF5A5F` on `#3A1618` | 5.3 | banners: engine gone, software rendering |
| `playhead` | `#F5F7FA` | 16.8 on a lane | the playhead while playing |
| `bypass` | `#9D7CF0` | 5.8 | a bypassed insert (filled, `textOnAccent` on it at 5.8:1) |

**Update, 2026-10-31** (design audit,
`brain/research/ui-design-audit-2026-10.md`). Principle 1 does not hold in
the tokens as shipped, and the table above is incomplete:

- `marker` `#E0A83C`, `markerRange` (marker at 35 %) and `conductor`
  `#8CCF7E` are in `theme.h` and are hereby part of this spec.
- Solo (42°), caution (39°), marker (40°) and meter amber (39°) are four
  meanings within 3° of one hue. Two of them may share a hue only if they
  never appear in the same region of a window and differ in form, for
  example a filled button against a text line. Solo keeps its yellow. Marker
  and caution move at least 25° of hue away from solo, or become
  form-distinct: caution is only ever text, never a fill or a chip.
- `input` and `meterGreen` share a hue (151°). Input monitoring moves at
  least 25° of hue away from the meter's green, so that a lit I is not read
  as signal.
- Green means signal and input. "Measured OK" (latency) is `text`, not green.
- Punch on is `record`, since a punch records; loop on stays `accent`.
- `accent` means selection, focus, a lit mode toggle (window tab, edit mode,
  tool, send bank, pre, follow-pan, click, count-off, loop) and the primary
  button of a dialog. Nothing else.
- Scale marks: `meterMark` rises from `#4A525B` (2.42:1 on the well) to at
  least `#606974` (3.45:1), to meet the 3:1 indicator target.

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
| `textCounter` | 40 | Medium, `tnum` | the main counter (was 28; Update 2026-10-31, glance set) |
| `textDisplay` | 40 | Bold, outlined | NO DATA over dead meters, nothing else |

**Update, 2026-10-31: reading distance.** The scale above is sized for a
desk at about 60 cm. On a 24-inch 1080p panel (0.277 mm/px), the 28 px
counter's cap height is about 5.6 mm, which subtends about 13 arcmin at
1.5 m; a 13 px name subtends about 6 arcmin. Engineers read the screen from
the mix position. Two tiers follow:

- **Glance set.** These are read from the mix position and are sized for it
  at the 1920×1080 reference: the main counter at least 40 px; the transport
  state at 15 px Bold; a strip's gain readout at 13 px with tabular figures;
  a strip's name at 13 px DemiBold (11 px at Narrow width); R/I/S/M letters
  at 11 px Bold on buttons of at least 24×24 px (WCAG 2.2, 2.5.8). The meters, the clip
  indicators and the record state belong to this set.
- **Work set.** These are read at the desk: everything else, at the sizes in
  the table.
- `textOverline` (9 px) labels a field. It never carries a value or a scale:
  fader and meter scales are `textCaption` with tabular figures.
- A **UI scale** preference (1.0, 1.25, 1.5, 2.0) multiplies every size in
  `theme.h`. The default comes from the screen's physical size: 1.0 for a
  24-inch 1080p screen, 1.25 for a 27-inch 1440p screen. The High-DPI line
  under Open is answered by this.

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

**Update, 2026-10-31:** an empty insert or send slot is a blank `sunken`
well with no border and no letter at rest; the letter appears on hover and
while dragging. Twenty strips used to show about 250 boxed letters for
about 30 slots in use, and an empty slot was louder than a running one.
A status chip (STOP, REC, NO ENGINE) is flat: no highlight edge, no hover
and no pointer, so a status is never mistaken for a button. Buttons have
both.

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
  **Update, 2026-10-31:** colour belongs to the track and not to its display
  index. A new track takes the next palette entry once, and the colour is
  stored with the track; moving, inserting or deleting tracks recolours
  nothing. Duplicating Snare used to shift every track below it to the next
  colour, so colour could not be used to find a track
  (`ui.editing.names_colours`).
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

**Update, 2026-10-31.**

- NO DATA is drawn in each dead meter's well, sized to the well. It is not
  drawn at the centre of the window over the timeline, as `Main.qml` did;
  the engine-gone banner already says it for the whole window.
- The Edit window's track-header meter is at least 6 px wide per channel,
  with a 4 px over indicator. It was about 3 px wide.

## Fader

Our own taper (`fader_taper.h`): +12 dB at the top, 0 dB at 78 % of the
travel, −10 at 58 %, −20 at 42 %, −40 at 20 %, −60 at 8 %, −inf at the
bottom; piecewise linear in dB, monotonic and exactly invertible. Drag rides
it, Ctrl-drag moves a tenth as far, Alt-click returns to 0 dB. Gain reads
`+3.5`, `0.0`, `−12.0`, `-inf`; above 0 dB the readout turns `caution`.
Pan reads `<100` … `C` … `100>` and draws as a bar filled from the centre.
**Update, 2026-10-31:** on a strip, pan is a knob (next section), with the
arc filled from the centre. The bar stays only where the space is wide,
such as the send window.

## Knobs, faders and sliders

**Update, 2026-10-31.** One rule picks the control:

| Control | For | Form |
|---|---|---|
| Vertical fader | levels ridden during playback: track, aux and master gain, and a send in the expanded send view | the taper above; throw per the strip anatomy |
| Knob | bounded, continuous values that are set and left, where width is short: pan, a send level on a strip, a generic plugin editor's parameters when there are 8 or more | 28 px at Narrow, 32 px otherwise; a 270° sweep; an arc from the neutral point (centre for pan, minimum for a level) in `textSecondary`, with an `accent` ring when focused; the readout under it in `textCaption` with tabular figures, always shown |
| Horizontal slider | dialogs and parameter lists with at least 160 px of track | a bar filled from its neutral point |
| Segmented buttons | stepped choices (mode, curve, filter type) | never a slider or a knob |

Every continuous control behaves the same way. A vertical drag rides it, at
200 px for the full range for a knob. Ctrl-drag moves a tenth as far.
Alt-click returns it to its default. A click on the readout lets you type a
value. The wheel moves one step. A knob is never turned by a circular drag.

## Mixer strip anatomy

**Update, 2026-10-31.** The fader and its meter are the strip, and every
other section is budgeted around them. The audit measured a throw of about
70 px on a 525 px strip (13 %), with the 10 insert and 5 send slots taking
49 %. Each section added since the first mixer had come out of the fader.

**Widths.** These are fixed and chosen per view (`ui.mixer.views`); they are
never computed by dividing the window by the number of strips. When the
strips do not fit, the strip area scrolls sideways and the master stays
pinned at the right.

| Width | px | At 1920 px |
|---|---|---|
| Narrow | 64 | about 27 strips |
| Regular (default) | 96 | 18 strips, a UMC1820's inputs |
| Wide | 128 | about 13 strips; master is always Wide |

**Heights, top to bottom.** These are for a Regular strip at the 1920×1080
reference, where the strip height H is about 930 px. Sections marked
*view* collapse and expand as a whole from the Mix window's View menu, as
Pro Tools' mix window views do. Defaults are as marked, and the choice is
kept with the session.

| Section | Default | Height | Notes |
|---|---|---|---|
| Colour cap and kind band | shown | 4 + 16 | the cap in the track colour; the band reads the input or the kind (AUX, MASTER) |
| I/O (input, output) | *view*, shown | 2 × 18 | |
| Inserts A–E | *view*, shown | 5 × 18 + 4 | `insertSlotHeight` rises from 16 to 18 so 11 px DemiBold fits with 3 px above and below |
| Inserts F–J | *view*, hidden | 5 × 18 + 4 | a collapsed bank with a plugin in it shows a "+N" chip on the bank above |
| Sends A–E | *view*, shown | 5 × 18 + 4 | |
| Sends F–J | *view*, hidden | 5 × 18 + 4 | same "+N" rule |
| Pan | shown | 44 | knob and readout; a stereo track has two knobs side by side |
| R / I / S / M | shown | 24 | one row of buttons at least 24 × 24 px (WCAG 2.2, 2.5.8); at Narrow, a 2 × 2 grid (50 px) |
| Fader and meter | shown | **the rest** | |
| Gain readout | shown | 20 | `sunken` well, 13 px, tabular figures |
| Name plate | shown | 24 | 13 px DemiBold, abbreviated by the rules below |

**Budgets.**

- With the default views, the fader zone is at least 55 % of H. That is
  about 540 px at the reference, and about 500 px of throw after the cap.
- With every view expanded, the fader zone is still at least 35 % of H and
  at least 240 px of throw. If a section would push it lower, that section
  scrolls inside its own frame; the fader never gives up the space.
- The fader cap is 28 px tall and at most 36 px wide. The fader's scale sits
  left of the slot and is never covered by the cap.
- The meter's height equals the fader's travel, top-aligned with the +12 dB
  mark, with an 8 px over indicator above it, as wide as the meter. Channel
  width is 8 px at Regular and Wide, 6 px at Narrow, and 12 px for each of
  the master's channels.
- The selected strip is visible from the mix position: a 2 px `accent` edge
  on its name plate, in addition to the `raised` surface. `raised` on
  `panel` alone is 1.16:1.

## Strip kinds

**Update, 2026-10-31.** An audio track, an aux return and the master are
told apart by three cues: the kind band's text, the controls the strip has,
and, for the master only, its surface and position. No new colour is used,
because of principle 1.

| Kind | Kind band | Controls | Surface and place |
|---|---|---|---|
| Audio track | the input ("In 3") | everything above | `panel`, in track order |
| Aux return | "AUX" in overline, then the bus name | no R or I; S, M, inserts, sends and a post-fader meter | `panel`, after the tracks, with an 8 px gap before the first aux |
| Master | "MASTER" in overline | fader, a two-channel meter with the scale on both sides, inserts when `ui.mixer.io` brings them, no R/I/S/M | `raised`, Wide, pinned at the right edge behind a `space4` gap and a 1 px `borderStrong` rule; it does not scroll |

- A strip never lacks a meter. When the engine publishes no feed (aux and
  master until state-block layout 3, `ui.mixer.metering`), the strip draws
  the no-data face in its meter well, not an empty space.
- The master has one name everywhere: the name plate reads "Master" and the
  I/O view reads its output ("Out 1-2"). Before this update the header read
  "Out 1-2" and the name plate "Monitor".
- Removing a bus is not a `×` on its name plate. It is in the strip's
  context menu, labelled "Remove bus and its sends", and it is one undo
  step.

## Names: abbreviation and truncation

**Update, 2026-10-31.** One Qt-free function, with tests, abbreviates track,
bus, send, insert and plugin names. It is the same in the Mix window, the
Edit header and every chooser. In order:

1. If the full name fits at the field's font, show it.
2. Otherwise abbreviate deterministically:
   - remove spaces;
   - remove lower-case vowels, right to left, keeping each word's first
     letter;
   - then remove lower-case consonants, right to left.

   Digits, upper-case initials and a trailing side mark (L, R) are never
   removed, because they tell siblings apart. "Talkback" becomes "Tlkbck",
   "Lead Vox" becomes "LeadVx" and then "LdVx", and "Gtr L 2" keeps "L2". A
   plugin uses its format's short name when one exists, and otherwise this
   rule on its name without the vendor prefix.
3. Use an ellipsis only when the abbreviation still does not fit: in the
   middle for file names and paths, at the end for prose.
4. A field whose text is shortened shows the full text in a tooltip after
   500 ms on hover. The selected strip's full name also reads in the Edit
   header.
5. Minimum glyphs at the name plate's size: 6 at Narrow, 8 at Regular. A
   width that cannot hold them is not offered.
6. Reasons and explanations are never shortened. They wrap, up to three
   lines, and anything longer becomes a held notice. Before this update the
   plugin chooser cut "…which no out-of-proces…".

## Plugin window chrome

**Update, 2026-10-31.** The same chrome serves the generic editor, the
native suite's editors and third-party editors (`plugins.host.editors`).

- **Header bar**, 32 px, `panel` with the highlight edge:
  - left: the track name (`textBody` DemiBold), the slot letter and the
    plugin name, followed by the format, version and latency in
    `textTertiary`;
  - right, in this order: Bypass (lit in `bypass`), the preset menu (the
    current preset's name and ▾), Compare (lit while the state differs from
    the loaded preset), a pin, and `×`.
- **Destructive and rare actions** (Remove, Make inactive) live in the
  header's ▾ menu. They are never a button beside Bypass.
- **Pin.** An unpinned plugin window is reused by the next insert opened. A
  pinned one stays open, so several can be open at once.
- **Placement.** A floating window opens beside the strip that owns it, not
  over it, when there is room. It never covers the master strip or the
  transport bar by default, and its position is remembered per insert with
  the session.
- **Body.** A native editor is drawn by the plugin at its own size and the
  chrome never scales it. A generic editor shows each parameter in the
  plugin's own units and value text (dB, ms, a ratio such as 4:1, never a
  bare %), in the plugin's own groups: knobs in a grid when there are 8 or
  more continuous parameters, otherwise rows with sliders of at least 160 px;
  stepped parameters as segmented buttons.
- **States.**
  - A faulted plugin turns the header `alarmSurface` with the reason, and
    the body repeats the reason.
  - An editor that cannot open (no display bridge, a missing feature) says
    why in its body and offers the generic editor. A blank window is a
    control that lies.
  - An inactive plugin's name is italic `textSecondary`, as in its slot.

## Windows, dialogs and menus

**Update, 2026-10-31.**

- **Close.** Every floating window closes with `×` at the top right of its
  header, and with Esc. Before this update there were five ways: `×`,
  Close, Done, Cancel and a named button.
- **Modal dialogs** dim the window behind them with black at 40 %. Buttons
  sit bottom right: the secondary (Cancel) first, the primary (filled
  `accent`) last; Enter takes the primary. A destructive action is never
  the filled primary. Its label names the action ("Remove bus"), and it is
  one undo step or asks first.
- **Window tabs** are only for full windows (Edit, Mix). Floating windows
  (Transport, Memory Locations, Undo History, Latency) open from a Window
  menu and their keys, never from a tab.
- **Menus.** One row is 24 px. The label is left-aligned in `text`, and the
  key equivalent from the active key map is right-aligned in `textTertiary`,
  so the menus teach the map. Separators group items, and an unavailable
  item is drawn at 45 %. Items are not boxed buttons and are not centred.

## Top bar

**Update, 2026-10-31.** The bar is laid out by priority, not as one fixed
row. As the width shrinks, items go in this order: key profile, format and
render path first; then take and xrun counts collapse into a single
indicator; then the menus fold into one menu button. Kept to the end:
loop, punch and tempo, the transport buttons, the window tabs, the counter
and the record state.

- The health or alarm line is **never** elided or dropped. When the bar has
  no room for it, it moves to the banner row under the bar. Before this
  update, at 1280 px it was the first thing to disappear.
- The minimum supported width is 1280 px. A capture at that width (below)
  shows nothing clipped.
- "Open" appears once, in the File menu. The window tab and the menu that
  are both called "Edit" must be told apart by more than position; the
  slice that rebuilds the bar picks the labels and shows them in its
  screenshots.

## Screenshot capture standards

**Update, 2026-10-31.** The screenshots are how the owner reviews the
interface. A capture that misstates the product, or shows it at a size it
is not used at, defeats that.

- **Sizes.** The reference is 1920×1080 at UI scale 1.0. The Mix and Edit
  main scenes are also captured at 2560×1440 at scale 1.25. One scene per
  window at 1280×720 proves that nothing clips at the minimum width. Until
  the rig changes, every capture is 1280×720.
- **No rig artefacts.** The headless rig has no GPU. In capture mode the
  software-rendering banner is suppressed, and the product's behaviour on a
  real machine is unchanged. One scene keeps the banner, to document it.
  The status line about the built-in fake engine appears only in scenes
  about it.
- **Content.** Mixer scenes use the 18-track demo session with 2 auxes and
  the master. One scene shows 48 tracks, the density the target hardware
  implies. Plugin windows have scenes for the generic editor and for the
  native editor's chrome.
- **Freshness.** A UI PR regenerates every scene its change touches and
  lists them in its description. A capture older than the QML it shows is
  stale and is regenerated in the same PR or deleted.
- **Form.** `docs/images/sonora-ui-<window>-<feature>.png`, lossless PNG,
  never cropped or retouched. A new scene is a new file under
  `ui/qt/src/scenes/`. Changing the rig's sizes or capture mode touches
  `main.cpp` and `tools/build-ui-entrypoint.sh`, and is its own PR.

## Review of UI PRs

**Update, 2026-10-31.** Review used to check the code and not what the
screen shows. From now on, the reviewer of any PR that changes what the
interface draws opens every added or changed screenshot with the Read tool
and checks it against this spec. The review names the screenshots it
opened. Any of the following is a **blocking** finding:

1. A visible change with no screenshot, or a stale screenshot left beside a
   new one.
2. Anything clipped or overlapping, including at the 1280×720
   minimum-width scene.
3. A colour or size that is not a token, or a colour used for a second
   meaning (principle 1).
4. Text under its contrast target (Colour tokens), or a glance-set element
   under its size (Typography).
5. A broken mixer strip budget (Mixer strip anatomy), a control of the
   wrong kind (Knobs, faders and sliders), a name shortened against the
   rules, or strip kinds that cannot be told apart.
6. Window chrome that departs from the plugin-window or dialog rules.
7. A control that lies: one that shows a state it does not have, or a blank
   where a reason belongs.

Off-grid spacing (not on the 4 px grid) and misalignment are non-blocking
and become BACKLOG lines, as other non-blocking findings do.

## Open

- Inter in the appliance image (ISO package list).
- Light theme: not planned. A control room is dark.
- High-DPI: every size is in device-independent pixels; untested on a
  scaled display. **Update, 2026-10-31:** the UI scale preference
  (Typography) is the answer. It stays untested until the capture rig
  renders at scale 1.25.
