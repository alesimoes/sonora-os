"""Writes README.md and index.html next to the SVGs (round 4).

Run:  python gen_docs.py <mockups_dir>
Keeping the control tables here means the README and the gallery cannot drift.
"""
import html, os, sys

PLUGINS = [
    {
        "file": "sn-lume", "name": "SN-Lume", "dir": "plugins/native/opto", "id": "org.sonora.opto",
        "size": "900 x 249 (3U)",
        "inspiration": "1960s tube optical leveling amplifier: flat pale grey-green 3U panel, one big pointer "
                       "knob each side of a centred VU in a dark bevelled bezel, bat toggles at the corners.",
        "rows": [
            ("GAIN knob (left of the meter, printed 10 to 100)", "gain", "-12 to +40 dB, linear",
             "Printed 10..100 over 270 degrees; the control maps linearly onto the parameter range. Tooltip `%+.1f dB`."),
            ("PEAK REDUCTION knob (right of the meter, printed 0 to 100)", "peak_reduction", "0 to 100 %, linear", ""),
            ("LIMIT / COMPRESS bat toggle (bottom left)", "mode", "compress (down), limit (up)", ""),
            ("STEREO / DUAL bat toggle (beside it, where the original has its trim nut)", "link", "off / on",
             "Our addition, drawn in the original's empty spot."),
            ("POWER bat toggle (right, under the selector)", "bypass", "up = processing, down = bypassed",
             "The original's mains switch, reused as bypass."),
            ("EXT KEY mini toggle (under the meter)", "external_sc", "off / on",
             "Our addition; hidden when the chain has no side-chain input."),
            ("EMPHASIS mini knob (under the meter)", "emphasis", "0 to 100 %", "Our addition, small."),
            ("MIX mini knob (under the meter)", "mix", "0 to 100 %", "Our addition, small."),
            ("Meter selector (top right: GAIN REDUCTION / OUTPUT +10 / OUTPUT +4)", "none", "view state",
             "Chooses what the needle shows; not a parameter."),
            ("VU meter", "none", "live gain reduction / output", "Needle ballistics live in the painter."),
        ],
        "match": [
            ("Panel", "#c5cac7 (top #cdd2cf, bottom #bcc2bf), matte paint with a faint grain, no brushing. Ears are part of the same sheet; two dark domed rack bolts per ear. Small slotted panel screws at the corners, under the bezel and beside it."),
            ("Proportions", "3U: 900 x 249 at 1x (19 in x 5.25 in), ears 26 px (the real 0.625 in)."),
            ("VU bezel", "Measured: x 0.388 to 0.615 of the face width, y 0.20 to 0.63 of the height, so about 192 x 107 px, aspect 1.8 : 1 (round 3 had 1.46 : 1 and was too tall). Thick dark grey frame (#3f4345) with a lighter silver bevel on the right and bottom edges. Cream window 150 x 70 inside it."),
            ("Knobs", "Cast black bakelite skirt knob, 54 px, a raised cap and a wedge-shaped pointer tab that stands proud of the skirt, with a white indicator line. Centres at x 0.262 and 0.738 of the face width, y 0.59."),
            ("Scale", "Heavy dark numerals, 11 px bold condensed, set radially (rotated) around the knob, ticks inside them. GAIN 10..100 (ten labels), PEAK REDUCTION 0..100 (eleven), 270 degrees, 0 / 10 at the lower left."),
            ("Selector and power", "Meter selector top right: a knob as large as the main ones (52 px) with three printed positions. Power toggle under it with ON above and POWER below. Both centred at x 0.885 of the face."),
            ("Lettering", "Small dark condensed grotesque caps, wide letter spacing, set under the knobs (GAIN, PEAK REDUCTION at 0.86 of the height). Red wordmark top left (our SN-LUME, plain, one straight rule) with a small two-line descriptor to its right. Neutral round SN emblem centred above the meter."),
        ],
        "changed": [
            "Panel colour and finish: flat grey-green paint (#c5cac7) instead of a cool brushed silver.",
            "VU meter is the right shape and size (aspect 1.8 : 1) in a thick dark bevelled bezel with silver bevel, instead of a small black frame.",
            "Knobs are cast pointer knobs with a wedge tab instead of plain round caps; scale numerals are bold, bigger and rotated radially; the printed ranges are 10..100 and 0..100 as on the original.",
            "Meter selector is a full-size knob with three printed positions (was a tiny dial); power toggle has ON/POWER legends; no jewel lamp (the original has none).",
            "Toggle spacing and the second (link) toggle moved to the original's nut position; our extra controls (key, emphasis, mix) shrunk into the empty strip under the meter.",
            "Wordmark in a plain red condensed face with one rule; the old italic script is gone.",
        ],
    },
    {
        "file": "sn-talon", "name": "SN-Talon", "dir": "plugins/native/fet", "id": "org.sonora.fet",
        "size": "900 x 166 (2U)",
        "inspiration": "1970s FET peak limiter, blue-stripe edition: brushed aluminium on the left, a painted blue "
                       "section on the right with the meter in a deep recess, knurled knobs, black push-button columns.",
        "rows": [
            ("INPUT knob (left, large, ticks every 6 dB, numerals -24 to +24)", "input", "-24 to +24 dB, linear", "Tooltip `%+.1f dB`."),
            ("OUTPUT knob (right of INPUT, large)", "output", "-24 to +24 dB, linear", ""),
            ("ATTACK knob (small, upper, printed 1 to 7)", "attack", "log, kAttackMinMs to kAttackMaxMs",
             "1 = slow (max ms), 7 = fast (min ms): the control inverts the parameter, as on the original."),
            ("RELEASE knob (small, lower, printed 1 to 7)", "release", "log, 59.4 to 1159.4 ms", "7 = fast, inverted likewise."),
            ("RATIO push buttons (one column, 20 / 12 / 8 / 4 top to bottom)", "ratio", "steps 0 to 3: 4:1, 8:1, 12:1, 20:1",
             "Radio group; the pressed one sits recessed."),
            ("All four ratio buttons in", "ratio", "step 4: all buttons", "Shift-click on any ratio button; all four then show pressed."),
            ("METER push buttons (GR, +8, +4, OFF; OFF is red)", "none", "view state", "Not a parameter."),
            ("LINK mini toggle (blue, right end)", "link", "off / on", "Our addition on the blank blue."),
            ("KEY mini toggle (blue, right end)", "external_sc", "off / on", "Host-routed; hidden when there is no side-chain input."),
            ("BYPASS mini toggle (blue, right end)", "bypass", "up = processing", "Pilot lamp (top left of the blue) is lit while processing."),
            ("MIX mini knob (blue, right end)", "mix", "0 to 100 %", "Our addition."),
            ("VU meter", "none", "live gain reduction", ""),
        ],
        "match": [
            ("Panel", "Left 0..0.55 of the face: brushed aluminium, horizontal grain, #cdcfd1 (top #d7d9db, bottom #bfc1c4). Right 0.55..1: painted blue #2f6cab (top #3576b7, bottom #2862a0) with a darker hairline at the join. Ears brushed like the left. 2U: 900 x 166 (19 in x 3.5 in)."),
            ("Etched outlines", "Two thin engraved rectangles on the silver (INPUT/OUTPUT section, ATTACK/RATIO section) with a corner screw each, as on the original."),
            ("Knobs", "Black knurled cylinders with a brushed-metal cap and an engraved pointer line carried to the rim. INPUT and OUTPUT 50 px at x 0.10 and 0.27 of the face, y 0.53; ATTACK and RELEASE 28 px stacked at x 0.41 (y 0.34 and 0.75). A single small pilot hole between INPUT and OUTPUT."),
            ("Scales", "Printed on the silver, not on the knob: ticks every 6 dB with numerals -24, -12, 0, +12, +24 (the original prints its own dB marks; ours follow the parameter range); 1..7 round the small knobs."),
            ("Ratio column", "Four black blocks, 22 x 27 px, almost touching, labels 20 / 12 / 8 / 4 to their left, RATIO above; the pressed block is pushed in 3 px. Just left of the blue."),
            ("Meter", "About 180 x 96 px on the blue, deep dark recess with a lighter top bevel, cream window (#f1e4bb) with the VU scale -20 to +3 and a red zone. Pilot lamp above its left corner (red jewel in a chrome bezel)."),
            ("Meter buttons", "A second black column right of the meter, GR / +8 / +4 / OFF (OFF red), METER above, labels in white to the right of each."),
            ("Lettering", "Wide, letter-spaced geometric caps. Dark on the silver; white on the blue (small descriptor line and our SN-TALON wordmark under the meter)."),
        ],
        "changed": [
            "The blue section now starts at 0.55 of the face (it was 0.58 and had the meter higher, smaller and in a thin frame): the meter sits in a deep recess with a bevel, and the lettering sits under it.",
            "Knobs are knurled black cylinders with a metal cap (round 3 had a ribbed bevel look); INPUT and OUTPUT are side by side on the left with ticks and numerals printed on the panel, ATTACK and RELEASE are small and stacked, as on the original (round 3 used a 2 x 2 grid).",
            "Ratio and METER columns are blocky black buttons with the pressed one recessed; OFF is red; ratio labels left of the buttons.",
            "Engraved section outlines, corner screws, and the single pilot hole added; brushed grain is horizontal and fine.",
            "Our added controls (link, key, bypass, mix) moved to the blank blue at the far right.",
        ],
    },
    {
        "file": "sn-aurea", "name": "SN-Aurea", "dir": "plugins/native/passiveeq", "id": "org.sonora.passiveeq",
        "size": "900 x 249 (3U)",
        "inspiration": "1950s/60s passive tube program equalizer: blue-grey hammer-finish 3U panel, four fluted "
                       "bakelite knobs in the top row, rotary frequency selectors with paddle handles below, a "
                       "toggle and a pilot lamp, no meter.",
        "rows": [
            ("LOW BOOST knob (top row, first; printed 0 to 10)", "lf_boost", "0 to 14 dB", "Tooltip `%+.1f dB`."),
            ("LOW ATTEN knob (top row, second)", "lf_atten", "0 to 16 dB", ""),
            ("HIGH BOOST knob (top row, third)", "hf_boost", "0 to 16 dB", ""),
            ("HIGH ATTEN knob (top row, fourth)", "hf_atten", "0 to 20 dB", ""),
            ("LOW FREQUENCY selector, CPS (bottom, left of centre)", "lf_freq", "4 steps: 20, 30, 60, 100 Hz", "Snaps; the paddle points at the printed value."),
            ("BANDWIDTH knob (bottom centre, 0 to 10, SHARP to BROAD)", "hf_q", "0.3 to 3.0, log", "SHARP (left) = high Q, BROAD (right) = low Q: the control inverts the parameter."),
            ("HIGH FREQUENCY selector, KCS (bottom, right of centre)", "hf_freq", "7 steps: 3, 4, 5, 8, 10, 12, 16 kHz", ""),
            ("ATTEN SEL selector, KCS (top right)", "hf_atten_freq", "3 steps: 5, 10, 20 kHz", ""),
            ("IN toggle (bottom, left of the selectors)", "bypass", "up = IN (processing)", "As on the original: toggle to switch the EQ in."),
            ("IRON mini toggle (bottom left)", "iron", "off / on", "Our addition, in the blank strip under the nameplate."),
            ("DRIVE mini knob (bottom left)", "drive", "0 to 24 dB (default 6)", "Our addition; tube make-up stage drive."),
            ("OUTPUT mini knob (bottom left)", "output", "-12 to +12 dB", "Our addition."),
            ("Pilot lamp (right, red jewel)", "none", "indicator", "Lit while processing."),
            ("OFF / ON mains knob and fuse holder (right)", "none", "decoration", "Drawn as on the original; not mapped (the host owns power)."),
        ],
        "match": [
            ("Panel", "Blue-grey steel, base #587b8f (top #628396, bottom #4f7083) with a fine hammer-finish grain and a slight darkening toward the edges. One sheet: no separate ears, just rack slots in the four corners and slotted panel screws inside the face. 3U: 900 x 249."),
            ("Top row", "Four knobs, centres at x 0.314, 0.444, 0.614, 0.736 of the panel width, y 0.30 of the height, in the original's order BOOST, ATTEN (low pair), BOOST, ATTEN (high pair), 62 px skirts with 12 flutes and a dished centre. Each has its name printed above it."),
            ("Scale", "Cream numerals 0 to 10, 10.5 px, set close round each knob (ring radius about 1.5 x the skirt), upright, no tick marks, 0 at the lower left and 10 at the lower right, 270 degrees; the same on the BANDWIDTH knob."),
            ("Bottom row", "LOW FREQUENCY paddle selector at x 0.385, BANDWIDTH knob at 0.53, HIGH FREQUENCY paddle selector at 0.675, y about 0.73. The positions are printed on a short arc above each selector (20 30 60 100 and 3 4 5 8 10 12 16), CPS and KCS above, the long names below in letter-spaced caps."),
            ("Right side", "ATTEN SEL three-position selector top right (the later model's layout), pilot lamp below it, fuse holder and OFF / ON knob at the bottom right."),
            ("Left", "Nameplate block of four short centred lines (our SN-AUREA, PROGRAM EQUALIZER, SONORA ENGINEERING, NATIVE SERIES) at x 0.13; IN toggle at x 0.257 with IN above it."),
            ("Knob type", "Black bakelite with a scalloped skirt and a dished cap, a thin cream pointer line on the rim; selectors have a tapered paddle handle on a round hub."),
        ],
        "changed": [
            "Panel colour: blue-grey steel with hammer finish (round 3 had a light powder blue with a silver strip).",
            "Layout rebuilt in the original's positions: four boost / atten knobs in the top row, the CPS selector, BANDWIDTH and KCS selector in the bottom row, ATTEN SEL top right (round 3 had two rows of identical knobs, a tiny VU and a title strip).",
            "Knobs are fluted bakelite with a dished cap and cream 0..10 numerals close round them, no ticks; the frequency selectors are paddle handles with their values printed on an arc (round 3 used knobs).",
            "Nameplate lines, pilot lamp, fuse holder, mains knob and slotted screws added; the title strip and the meter are gone (the original has none; the meter belongs to the host).",
            "Our DRIVE, OUTPUT and IRON moved to the blank strip under the nameplate, small, in the same bakelite style.",
        ],
    },
]

NEXT = [
    ("plugins/native/varimu", "Vari-Mu", "variable-mu (remote-cutoff) tube compressor: header says our own circuit values, held to the class of circuit; the class has the large dual-meter 1950s/60s tube-compressor look. Chosen unit to be confirmed by the owner."),
    ("plugins/native/mixbus", "Mix bus", "bus compressor with stepped ratio / release and a console colour stage (`native-mixbus-n2.md`): the console-bus 2U compressor class with a wide meter, stepped switches and large knobs."),
    ("plugins/native/tape", "Tape", "tape record/playback (header, from published physics): a studio tape machine transport-and-meter faceplate class (twin VU, a row of knobs)."),
    ("plugins/native/mastering", "Mastering console", "mastering console (`native-plugin-suite.md` 1.13): a mastering-console strip with stepped EQ switches."),
    ("plugins/native/tube, transformer, saturator", "Tube / iron / saturation", "no hardware unit is named in the sources; no faceplate planned until one is picked."),
]


def md_table(rows):
    o = ["| Control | Parameter id | Range / steps | Notes |", "|---|---|---|---|"]
    for c, p, r, n in rows:
        o.append("| %s | %s | %s | %s |" % (c, "`%s`" % p if p != "none" else "none", r, n))
    return "\n".join(o)


def readme():
    o = []
    o.append("""# Plugin editor mockups (style proposal, round 4)

Images only, for the owner to approve the look before any editor code is
written. Each picture is the editor window itself: a flat front view of a
classic unit's panel, 900 px wide at 1x (the PNGs are rendered at 2x). Open
`index.html` in this folder (a local file, no network, no scripts) to see all
three large, with their control tables.

Round 4 redraws all three from public photographs of the unit types they take
after (see "What was matched"): panel proportions, panel colour and finish,
knob style and size, scale printing, meter style, lettering placement and
typeface feel, rack ears and screws. Only our own names appear: no original
brand, model name, logo, badge or trade-dress text anywhere (owner rule,
2026-11-06). Nothing is traced: the photographs were studied for positions
and proportions, and every shape is drawn from scratch as SVG.

| File | Plugin | Window | Inspiration |
|---|---|---|---|""")
    for p in PLUGINS:
        o.append("| `%s.svg` / `.png` | `%s` (`%s`) | %s | %s |" % (p["file"], p["dir"], p["id"], p["size"], p["inspiration"]))
    o.append("""
There are no digital readouts on any panel. The value of a control shows as a
small tooltip while the pointer hovers over it or drags it (one is drawn per
picture to show the idea); double-click types an exact value.

Where the original's scale and the parameter range differ, the printing
follows the original and the control maps onto the parameter range linearly
(or logarithmically where the taper says so). `quality` is a non-automatable
session-level choice and has no control on any panel: it goes in a small menu
in the window's title strip.

## Reference study

Sources: Creative Commons photographs of the three unit types on Wikimedia
Commons (front views of rack units in studios), searched by unit type and
viewed in this round; used only to measure positions, proportions, colours
and the knob and scale styles. They are not copied or traced and are not in
this repository. Positions below are fractions of the face width (W) or the
panel height (H) measured by eye on those photographs. Measurements are
approximate; items marked unverified are listed at the end.
""")
    for p in PLUGINS:
        o.append("### %s: what was matched\n" % p["name"])
        o.append("| Item | What the original shows and what we drew |\n|---|---|")
        for k, v in p["match"]:
            o.append("| %s | %s |" % (k, v))
        o.append("\n### %s: what changed since round 3\n" % p["name"])
        for c in p["changed"]:
            o.append("- " + c)
        o.append("")
    o.append("""## Control to parameter mapping

Parameter ids are the ones in each plugin's `kParams` table (checked against
`plugins/native/<name>/src/<name>.cpp` this round). Every control group in
the SVG carries `id="ctl-<param id>"` and `data-param="<param id>"`, so the
files double as the manifest the skin loader needs. Controls the original has
and our parameters lack are drawn as the original does and marked "none".
""")
    for p in PLUGINS:
        o.append("### %s (%s)\n" % (p["name"].upper(), p["dir"].split("/")[-1]))
        o.append(md_table(p["rows"]))
        o.append("")
    o.append("""## How a faceplate would plug into the DAW

Today a native plugin has no hand-drawn window: the Qt UI draws every plugin
from its parameter catalog (`param_control` in `ui/qt/src/insert_controller.h`,
ADR-0011 section 2.6). A faceplate would be an optional skin on that same
path; the generic editor stays the fallback.

- A skin is data plus layers: static background (panel, legends, scales), one
  sprite per knob style (rotated), a needle layer per meter, a glass overlay.
- One Qt painter loads the layer list and a manifest (parameter id, centre,
  radius or rectangle, sprite, angle range: 270 degrees, -135 to +135). The
  tables above are that manifest.
- Input is the generic editor's (drag, wheel, click, double-click to type)
  through the existing `InsertController` set-parameter path; meters read
  atomics from the UI thread and move only the needle layer.
- Render the SVG layers once into textures at the window's scale factor.

## Next: processors whose sources name a hardware class

Found by reading `plugins/native/*` headers and `brain/research/native-*`.
Not drawn in this round; each would need the owner to pick the unit first.
""")
    for d, n, t in NEXT:
        o.append("- `%s` (%s): %s" % (d, n, t))
    o.append("""
## Unverified

- Fonts: the SVGs use system font stacks (Arial Narrow, a Gill Sans-like
  engraved face, Eurostile-like wide caps) and fall back to Arial when
  missing; the PNGs were rendered on Windows with headless Chrome. The
  skin loader should embed freely licensed fonts or outline the text; none
  is chosen here.
- DPI scaling and the GPU cost of the hammer-finish and brushed-metal noise
  filters (they are static; render once into a texture).
- Photographs were viewed once and measured by eye: positions are accurate to
  a few percent, not to the pixel.
- Assets are all original SVG drawn for this project (the generator scripts
  are in `tools/`); no external image or font file is embedded.

## Regenerating

`python tools/gen_faceplates.py .` then `python tools/render.py` (headless
Chrome) then `python tools/gen_docs.py .` from this folder.
""")
    return "\n".join(o)


CSS = """
body{margin:0;background:#16181b;color:#d9dde1;font-family:Segoe UI,Arial,sans-serif;line-height:1.45}
header{padding:28px 40px 8px}h1{margin:0 0 6px;font-size:26px;font-weight:600}
header p{margin:0;color:#98a0a8;max-width:900px}
nav{padding:8px 40px}nav a{color:#8fb7e0;margin-right:18px;text-decoration:none}
section{padding:26px 40px 34px;border-top:1px solid #2a2d31}
h2{margin:0 0 4px;font-size:21px;font-weight:600}
.insp{color:#aab2ba;margin:0 0 16px;max-width:1000px}
.meta{color:#7f8790;font-size:13px;margin:0 0 14px}
img{display:block;max-width:100%;height:auto;border-radius:3px;box-shadow:0 6px 28px #000a}
table{border-collapse:collapse;margin:18px 0 6px;font-size:13px;width:100%;max-width:1200px}
th,td{border:1px solid #2e3237;padding:5px 9px;text-align:left;vertical-align:top}
th{background:#1f2327;color:#c6ccd2}code{color:#e6c38a}
h3{font-size:15px;margin:20px 0 4px;color:#c6ccd2}ul{margin:4px 0 0 18px;padding:0;max-width:1000px;color:#aab2ba}
footer{padding:20px 40px 40px;color:#7f8790;font-size:13px;border-top:1px solid #2a2d31}
"""


def index():
    e = html.escape
    o = ["<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">",
         "<title>Plugin faceplate mockups, round 4</title>",
         "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><style>%s</style></head><body>" % CSS,
         "<header><h1>Plugin faceplate mockups, round 4</h1>"
         "<p>Faithful redraws of the unit types that inspired each plugin, for owner approval. "
         "Images only: no editor code until approved. Our names only; nothing traced.</p></header><nav>"]
    for p in PLUGINS:
        o.append("<a href=\"#%s\">%s</a>" % (p["file"], e(p["name"])))
    o.append("</nav>")
    for p in PLUGINS:
        o.append("<section id=\"%s\"><h2>%s</h2><p class=\"insp\">%s</p>"
                 "<p class=\"meta\">Plugin <code>%s</code> (<code>%s</code>), window %s, PNG at 2x.</p>"
                 "<a href=\"%s.png\"><img src=\"%s.png\" alt=\"%s faceplate\"></a>"
                 % (p["file"], e(p["name"]), e(p["inspiration"]), e(p["dir"]), e(p["id"]), e(p["size"]), p["file"], p["file"], e(p["name"])))
        o.append("<h3>What changed since round 3</h3><ul>%s</ul>" % "".join("<li>%s</li>" % e(c) for c in p["changed"]))
        o.append("<h3>Control to parameter mapping</h3><table><tr><th>Control</th><th>Parameter id</th><th>Range / steps</th><th>Notes</th></tr>")
        for c, pid, r, n in p["rows"]:
            o.append("<tr><td>%s</td><td><code>%s</code></td><td>%s</td><td>%s</td></tr>" % (e(c), e(pid), e(r), e(n.replace("`", ""))))
        o.append("</table></section>")
    o.append("<footer>Next, not drawn yet: %s. Details, reference notes and the unverified list are in README.md.</footer></body></html>"
             % e("; ".join(n for _, n, _ in NEXT)))
    return "\n".join(o) + "\n"


if __name__ == "__main__":
    out = sys.argv[1]
    with open(os.path.join(out, "README.md"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(readme())
    with open(os.path.join(out, "index.html"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(index())
    print("ok")
