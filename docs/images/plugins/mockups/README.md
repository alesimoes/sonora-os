# Plugin editor mockups (style proposal, round 3)

Images only, for the owner to approve the look before any editor code is
written. Each picture is the editor window itself: a front view of a classic
unit's panel, flat, 900 px wide (the PNGs are rendered at 2x). The layout,
proportions, finish, knob types and meter face follow the unit that inspired
each plugin, as commercial emulations do; only the names are ours (no
original brand, model name, logo or badge text anywhere).

| File | Plugin | Window | Inspiration |
|---|---|---|---|
| `sn-lume.svg` / `.png` | `plugins/native/opto` (`org.sonora.opto`) | 900 x 249 (3U) | 1960s tube optical leveler: smooth cool light-silver panel, small black pointer knobs with a tight 0-100 ring, centred cream VU, red italic wordmark, sparse layout |
| `sn-talon.svg` / `.png` | `plugins/native/fet` (`org.sonora.fet`) | 900 x 166 (2U) | 1970s FET limiter, silver-face/blue-band edition: brushed aluminium, blue band with the VU, 2 x 2 knurled knobs, black ratio and meter push-button columns |
| `sn-aurea.svg` / `.png` | `plugins/native/passiveeq` (`org.sonora.passiveeq`) | 900 x 249 (3U) | 1950s/60s passive tube program equalizer: light powder-blue panel, silver nameplate strip, two rows of black pointer knobs with 0-10 scales |

There are no digital readouts on any panel. The value of a control shows as a
small tooltip while the pointer hovers over it or drags it (one is drawn on
one knob per picture to show the idea); double-click types an exact value.
The slotted rack screws sit in the oval slots of the panel ears.

Where the original's scale and the parameter range differ, the printing
follows the original and the control maps onto the parameter range linearly
(or logarithmically where the taper says so). All lettering is small black caps (white on Talon's blue band).

## Control to parameter mapping

Parameter ids are the ones in each plugin's `kParams` table. Every control
group in the SVG carries `id="ctl-<param id>"` and `data-param="<param id>"`,
so the files double as the manifest the skin loader needs. `quality` is a
non-automatable session-level choice and has no control on any panel: it goes
in a small menu in the window's title strip.

### SN-LUME (opto)

| Control | Parameter id | Range / steps | Notes |
|---|---|---|---|
| GAIN knob (centre-left, scale 0-100) | `gain` | -12 to +40 dB | tooltip `%+.1f dB` |
| PEAK REDUCTION knob (centre-right, 0-100) | `peak_reduction` | 0 to 100 % | |
| LIMIT / COMPRESS bat toggle (bottom-left) | `mode` | compress (down), limit (up) | |
| STEREO / DUAL bat toggle (beside it) | `link` | on = stereo (up) | |
| EXT KEY / INT tiny toggle (bottom edge) | `external_sc` | off / on | host-routed; hidden when the chain has no side-chain input |
| POWER bat toggle (bottom-right) | `bypass` | up = processing, down = bypassed | |
| Red jewel pilot lamp | none (indicator) | lit when not bypassed | |
| EMPHASIS tiny knob (bottom edge) | `emphasis` | 0 to 100 % | discreet, under the meter |
| MIX tiny knob (bottom edge) | `mix` | 0 to 100 % | discreet, under the meter |
| METER selector (top-right: GAIN RED. / +10 / +4) | none (view state) | what the needle shows | not a parameter |
| "SN" emblem above the meter | none | decoration | neutral, not a brand |
| VU meter | none (live meter) | gain reduction | |

### SN-TALON (fet)

| Control | Parameter id | Range / steps | Notes |
|---|---|---|---|
| INPUT knob (top-left of the 2 x 2 grid, large) | `input` | -24 to +24 dB | |
| OUTPUT knob (top-right of the grid, large) | `output` | -24 to +24 dB | |
| ATTACK knob (bottom-left of the grid, small, 1-7) | `attack` | log, `kAttackMinMs` to `kAttackMaxMs` | 1 = slow (max ms), 7 = fast (min ms): the control inverts the parameter, as on the original |
| RELEASE knob (bottom-right of the grid, small, 1-7) | `release` | log, 59.4 to 1159.4 ms | 1 = slow (max ms), 7 = fast (min ms), inverted likewise |
| RATIO push buttons (one column, 20 / 12 / 8 / 4 top to bottom) | `ratio` | steps 0 to 3: 4:1, 8:1, 12:1, 20:1 | radio group, one pressed |
| All four ratio buttons pressed | `ratio` | step 4: "all buttons" | entered with shift-click on any ratio button; all four then show pressed |
| METER buttons (second black column: GR, +8, +4, OFF) | none (view state) | what the needle shows | not a parameter |
| MIX tiny knob (blue band, right edge) | `mix` | 0 to 100 % | parallel mix |
| LINK tiny toggle (blue band) | `link` | off / on | |
| KEY tiny toggle (blue band) | `external_sc` | off / on | host-routed, as above |
| BYPASS tiny toggle (blue band; red jewel lamp top-right) | `bypass` | up = processing | |
| VU meter | none (live meter) | gain reduction | |

### SN-AUREA (passiveeq)

| Control | Parameter id | Range / steps | Notes |
|---|---|---|---|
| LOW BOOST knob (top row, 0-10 printed on the panel) | `lf_boost` | 0 to 14 dB | |
| LOW ATTEN knob | `lf_atten` | 0 to 16 dB | |
| LOW FREQUENCY selector (CPS) | `lf_freq` | 4 steps: 20, 30, 60, 100 Hz | snaps |
| HIGH BOOST knob | `hf_boost` | 0 to 16 dB | |
| BANDWIDTH knob (SHARP to BROAD) | `hf_q` | 0.3 to 3.0, log | SHARP (left) = high Q, BROAD (right) = low Q: the control inverts the parameter |
| HIGH FREQUENCY selector (KCS) | `hf_freq` | 7 steps: 3, 4, 5, 8, 10, 12, 16 kHz | |
| ATTEN SEL selector (KCS) | `hf_atten_freq` | 3 steps: 5, 10, 20 kHz | |
| HIGH ATTEN knob | `hf_atten` | 0 to 20 dB | |
| DRIVE small knob (bottom row) | `drive` | 0 to 24 dB (default 6) | tube make-up stage drive |
| OUTPUT small knob (bottom row) | `output` | -12 to +12 dB | |
| IRON toggle (bottom row) | `iron` | off / on | |
| IN / OUT bat toggle, red lamp to its left (bottom row) | `bypass` | IN (up) = processing | |
| Tiny VU (bottom row) | none (live meter) | output peak | |

## How a faceplate would plug into the DAW

Today a native plugin has no hand-drawn window: the Qt UI draws every plugin
from its parameter catalog (`param_control` in `ui/qt/src/insert_controller.h`
turns a parameter into a knob, a switch or a row of choices, ADR-0011 section
2.6), and `plugins/host/src/editors.*` only hosts the windows of third-party
VST3/LV2 plugins. A faceplate would be an optional skin on that same path, so
the generic editor stays the fallback for any plugin without one.

- A skin is data plus layers, not C++ per plugin: the SVG split into a static
  background (panel, ears, engraved legends, scale printing, meter glass
  bezel), one sprite per knob style (cap and skirt, rotated), a needle layer
  per meter and a glass overlay on top.
- A single Qt painter (`QQuickPaintedItem` or `QQuickItem` with scene-graph
  nodes, whichever the UI already uses for its meters) loads the skin's layer
  list and a small manifest: for each control, its parameter id, its centre and
  radius (or rectangle), the sprite it uses and the angle range (the
  mockups use 270 degrees, -135 to +135). The control to parameter tables above
  are exactly that manifest.
- Input is the generic editor's: vertical drag and wheel on a knob, click on a
  button, double-click on a readout to type, all going through the existing
  `InsertController` set-parameter path, so automation, undo and the audio
  thread's atomics are untouched. The readouts are the same value formatting
  the catalog already provides (`"%+.1f dB"` and so on).
- Meters read `Track::take_peak()`-style atomics from the UI thread at the
  display rate and only move the needle layer; nothing waits on the audio
  thread. The needle ballistics (VU rise and fall) live in the painter.
- Rendering the SVG layers once into textures at the window's scale factor
  keeps repaint cost to a few quads per frame. The mockup SVGs are written so
  they can be split into layers (knobs and needles are separate groups).

## Not decided here

- The renderer is decided by the UI toolkit ADR (`architect`), not by this
  mockup. Nothing above depends on a particular toolkit beyond "paints
  layers and rotates sprites".
- Keyboard focus order, the hover/drag tooltip details, and a resizable window (the mockups are a
  fixed size) are left to the implementation task.
