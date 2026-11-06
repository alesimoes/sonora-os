# Plugin editor mockups (style proposal)

Images only, for the owner to approve the look before any editor code is
written. Each picture is the editor window itself, drawn flat and front-on at
its real size (900 px wide; the PNGs are rendered at 2x). Nothing here is a
replica of a real unit: the names are ours, and the era and layout are only
evoked.

| File | Plugin | Window | Era / look |
|---|---|---|---|
| `sn-lume.svg` / `.png` | `plugins/native/opto` (`org.sonora.opto`) | 900 x 320 | Optical leveler: cream painted steel of the 1960s, big glass VU, two chicken-head knobs, toggle switch |
| `sn-talon.svg` / `.png` | `plugins/native/fet` (`org.sonora.fet`) | 900 x 340 | FET limiter: black front with a brushed-silver strip, 1970s, four silver knobs, a row of ratio push buttons, needle VU |
| `sn-aurea.svg` / `.png` | `plugins/native/passiveeq` (`org.sonora.passiveeq`) | 900 x 460 | Passive tube EQ: blue-grey painted panel, black bakelite boost/cut knobs, silver stepped frequency selectors, boxed sections |

The amber readout boxes under each knob are not on the old hardware; they are
ours, so a mouse user can read and type exact values (double-click a readout to
type). The slim dark side strips with screws are the rack ears, and double as
the window frame.

## Control to parameter mapping

Parameter ids are the ones in each plugin's `kParams` table.

### SN-LUME (opto)

| Control | Parameter id | Range / steps | Notes |
|---|---|---|---|
| GAIN knob (chicken head) | `gain` | -12 to +40 dB | readout `%+.1f dB` |
| PEAK REDUCTION knob | `peak_reduction` | 0 to 100 % | scale printed 0 to 10, readout in % |
| MODE toggle | `mode` | compress / limit | |
| EMPHASIS knob | `emphasis` | 0 to 100 % | |
| MIX knob | `mix` | 0 to 100 % | |
| STEREO LINK button | `link` | off / on | |
| EXTERNAL KEY button | `external_sc` | off / on | host-routed; hidden when the chain has no side-chain input |
| BYPASS button | `bypass` | off / on | common bypass parameter |
| VU meter | none (live meter) | gain reduction | the GR / OUT buttons select what the needle shows; view state only, not a parameter |

`quality` (eco / standard / high) is not on the panel: it is a non-automatable,
session-level choice, so it goes in a small menu opened from the window's
title strip rather than on the faceplate.

### SN-TALON (fet)

| Control | Parameter id | Range / steps | Notes |
|---|---|---|---|
| INPUT knob | `input` | -24 to +24 dB | |
| ATTACK knob | `attack` | log taper, `kAttackMinMs` to `kAttackMaxMs`, default 0.5 ms | scale marked slow / fast |
| RELEASE knob | `release` | log taper, 59.4 to 1159.4 ms | scale marked fast / slow |
| OUTPUT knob | `output` | -24 to +24 dB | |
| RATIO buttons 4, 8, 12, 20, ALL | `ratio` | 5 steps: 4:1, 8:1, 12:1, 20:1, all buttons | radio group, one lit |
| LINK button | `link` | off / on | |
| KEY button | `external_sc` | off / on | host-routed, as above |
| MIX knob | `mix` | 0 to 100 % | parallel mix, small knob |
| BYPASS button | `bypass` | off / on | |
| VU meter | none (live meter) | gain reduction | |

`quality` is in the title-strip menu, as for SN-LUME.

### SN-AUREA (passiveeq)

| Control | Parameter id | Range / steps | Notes |
|---|---|---|---|
| LOW BOOST knob | `lf_boost` | 0 to 14 dB | scale 0 to 10 |
| LOW CUT knob | `lf_atten` | 0 to 16 dB | |
| LOW FREQUENCY selector | `lf_freq` | 4 steps: 20, 30, 60, 100 Hz | stepped, snaps |
| HIGH BOOST knob | `hf_boost` | 0 to 16 dB | |
| HIGH BANDWIDTH knob | `hf_q` | 0.3 to 3.0, log | sharp (high Q) to broad (low Q) |
| HIGH BOOST FREQ selector | `hf_freq` | 7 steps: 3, 4, 5, 8, 10, 12, 16 kHz | |
| HIGH CUT knob | `hf_atten` | 0 to 20 dB | |
| HIGH CUT FREQ selector | `hf_atten_freq` | 3 steps: 5, 10, 20 kHz | |
| DRIVE knob | `drive` | 0 to 24 dB (default 6) | tube make-up stage drive |
| OUTPUT knob | `output` | -12 to +12 dB | |
| IRON button | `iron` | off / on | |
| BYPASS button | `bypass` | off / on | |
| LEVEL LED strip | none (live meter) | output peak | |

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
- Keyboard focus order, tooltips, and a resizable window (the mockups are a
  fixed size) are left to the implementation task.
