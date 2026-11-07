"""Generates sn-lume.svg, sn-talon.svg, sn-aurea.svg (round 4).

Run:  python gen_faceplates.py <out_dir>
Layout fractions were measured by eye on public reference photographs of the
original front panels and are listed in the README. Nothing is traced.
"""
import os, sys
from fp import *


# ------------------------------------------------------------------ SN-LUME
def lume():
    W, H = 900, 249
    X0, FW = 26, 848
    PANEL = "#c5cac7"
    o = [svg_open(W, H, "SN-Lume")]
    o.append('<linearGradient id="g_panel" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#cdd2cf"/>'
             '<stop offset="1" stop-color="#bcc2bf"/></linearGradient>')
    o.append('<linearGradient id="g_bezel" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#3f4345"/>'
             '<stop offset="1" stop-color="#2a2d2f"/></linearGradient>')
    o.append('<filter id="f_satin" x="0" y="0" width="100%" height="100%"><feTurbulence type="fractalNoise" '
             'baseFrequency="0.8" numOctaves="2" seed="4"/><feColorMatrix type="matrix" '
             'values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 0.9 -0.3"/></filter></defs>')
    o.append(rect(0, 0, W, H, "url(#g_panel)"))
    o.append(rect(0, 0, W, H, "#000", extra=' filter="url(#f_satin)" opacity="0.05"'))
    o.append(line(0, 1.5, W, 1.5, "#fff", 1, ' stroke-opacity="0.5"'))
    o.append(line(0, H - 1, W, H - 1, "#8d9290", 1))
    # rack ears: same sheet, dark domed rack bolts
    for ex in (13, 887):
        for ey in (40, 209):
            o.append(circ(ex + 0.8, ey + 1.5, 8, "#000", extra=' opacity="0.35"'))
            o.append(circ(ex, ey, 8, "#17120f", "#000", 0.8))
            o.append('<ellipse cx="%s" cy="%s" rx="4" ry="2.6" fill="#fff" opacity="0.25"/>' % (ex - 2, ey - 3))
    o.append(line(X0, 0, X0, H, "#9aa09d", 0.8))
    o.append(line(W - X0, 0, W - X0, H, "#9aa09d", 0.8))
    # small panel screws (positions measured on the reference)
    for sx, sy in ((X0 + 0.05 * FW, 20), (X0 + 0.96 * FW, 18), (X0 + 0.032 * FW, 229),
                   (X0 + 0.347 * FW, 229), (X0 + 0.658 * FW, 229), (X0 + 0.987 * FW, 229),
                   (547, 40), (452, 174)):
        o.append(screw(sx, sy, 3.3 if sy < 100 or sy > 200 else 2.4))
    # wordmark block (top-left): red condensed bold, one straight rule under it
    o.append(text(78, 63, "SN-LUME", 27, "#8e1b24", "start", "bold", NARROW, 0.5, "normal"))
    o.append(line(78, 69, 192, 69, "#8e1b24", 1.1))
    o.append(text(79, 79, "SONORA ENGINEERING", 6, "#8e1b24", "start", "normal", NARROW, 1.6))
    o.append(text(218, 55, "LEVELING AMPLIFIER", 6.8, "#333", "start", "normal", NARROW, 0.8))
    o.append(text(218, 67, "OPTO SERIES", 6.8, "#333", "start", "normal", NARROW, 0.8))
    # neutral round emblem above the meter
    o.append(circ(452, 24, 10, "#1b1c1e", "#9aa0a5", 1.2))
    o.append(circ(452, 24, 7.5, "none", "#c9cdd0", 0.6))
    o.append(text(452, 27, "SN", 8, "#e6e8ea", "middle", "bold"))
    # VU meter: dark bezel with silver bevel on right and bottom edges
    bx, by, bw, bh = 355, 50, 192, 107
    o.append(rect(bx + 2, by + 3, bw, bh, "#000", extra=' opacity="0.4" filter="url(#f_shadow)"'))
    o.append(rect(bx, by, bw, bh, "url(#g_bezel)", "#1a1c1d", 1))
    o.append('<polygon points="%s,%s %s,%s %s,%s %s,%s" fill="#8d9296"/>' % (
        bx + bw - 14, by, bx + bw, by, bx + bw, by + bh, bx + bw - 14, by + bh - 14))
    o.append('<polygon points="%s,%s %s,%s %s,%s %s,%s" fill="#777c80"/>' % (
        bx, by + bh, bx + bw, by + bh, bx + bw, by + bh - 14, bx + 20, by + bh - 14))
    o.append(vu(377, 68, 150, 70, 452, 172, 78, -8, "SN-LUME", "#efe5bf", 7.5))
    o.append(text(452, 62, "LEVEL INDICATOR", 4.5, "#aeb3b0", "middle", "normal", NARROW, 1))
    # Gain knob  (printed 10..100, 270 degrees)
    def big_knob(cx, cy, ang, labels, name, pid, tip=None):
        g = ['<g id="ctl-%s" data-param="%s">' % (pid, pid)]
        g.append(scale(cx, cy, labels, 33, 37.5, 45, 11, "#1d1d1d", NARROW, "bold", True, tw=1.1))
        g.append(knob_pointer(cx, cy, 27, ang))
        g.append(text(cx, 216, name, 8.5, "#222", "middle", "normal", NARROW, 1.2))
        g.append('</g>')
        return "".join(g)
    gl = [(a, str(10 * (i + 1))) for i, a in enumerate(even(10))]
    o.append(big_knob(248, 147, 22, gl, "GAIN", "gain"))
    pl = [(a, str(10 * i)) for i, a in enumerate(even(11))]
    o.append(big_knob(652, 147, -35, pl, "PEAK REDUCTION", "peak_reduction"))
    o.append(tooltip(210, 80, "+18.0 dB", 56))
    # meter selector, top right: three printed positions
    o.append('<g id="view-meter-select">')
    o.append(text(776, 30, "GAIN REDUCTION", 6.5, "#333", "middle", "normal", NARROW, 0.4))
    o.append(text(735, 45, "OUTPUT +10", 6.5, "#333", "middle", "normal", NARROW, 0.4))
    o.append(text(822, 45, "OUTPUT +4", 6.5, "#333", "middle", "normal", NARROW, 0.4))
    o.append(knob_pointer(776, 88, 25, 0))
    o.append('</g>')
    # power toggle
    o.append('<g id="ctl-bypass" data-param="bypass">')
    o.append(text(776, 139, "ON", 6.5, "#333", "middle", "normal", NARROW, 0.5))
    o.append(toggle(776, 175, "up", 1.0, "#d8dade"))
    o.append(text(776, 207, "POWER", 6.5, "#333", "middle", "normal", NARROW, 0.6))
    o.append('</g>')
    # limit / compress toggle and the link toggle that sits where the trim nut is
    o.append('<g id="ctl-mode" data-param="mode">')
    o.append(text(78, 141, "LIMIT", 6.5, "#333", "middle", "normal", NARROW, 0.5))
    o.append(toggle(78, 175, "up", 1.0))
    o.append(text(78, 207, "COMPRESS", 6.5, "#333", "middle", "normal", NARROW, 0.5))
    o.append('</g>')
    o.append('<g id="ctl-link" data-param="link">')
    o.append(text(128, 141, "STEREO", 6, "#333", "middle", "normal", NARROW, 0.4))
    o.append(toggle(128, 175, "up", 0.85))
    o.append(text(128, 207, "DUAL", 6, "#333", "middle", "normal", NARROW, 0.4))
    o.append('</g>')
    # our additions, kept small in the blank strip under the meter
    o.append('<g id="ctl-external_sc" data-param="external_sc">')
    o.append(text(404, 187, "EXT KEY", 5.5, "#333", "middle", "normal", NARROW, 0.4))
    o.append(toggle(404, 204, "down", 0.6, nut=True))
    o.append('</g>')
    for pid, cx, nm, ang in (("emphasis", 452, "EMPHASIS", -100), ("mix", 500, "MIX", 60)):
        o.append('<g id="ctl-%s" data-param="%s">' % (pid, pid))
        o.append(knob_pointer(cx, 203, 9, ang, wing=False))
        o.append(text(cx, 228, nm, 5.5, "#333", "middle", "normal", NARROW, 0.4))
        o.append('</g>')
    o.append('</svg>')
    return "\n".join(o)


# ----------------------------------------------------------------- SN-TALON
def talon():
    W, H = 900, 166
    X0, FW = 26, 848
    BLUE0 = 482
    o = [svg_open(W, H, "SN-Talon")]
    o.append('<linearGradient id="g_silver" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#d7d9db"/>'
             '<stop offset="0.5" stop-color="#cdcfd1"/><stop offset="1" stop-color="#bfc1c4"/></linearGradient>'
             '<linearGradient id="g_blue" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3576b7"/>'
             '<stop offset="1" stop-color="#2862a0"/></linearGradient>'
             '<radialGradient id="g_cap" cx="0.35" cy="0.3" r="0.9"><stop offset="0" stop-color="#f4f5f6"/>'
             '<stop offset="0.5" stop-color="#a9acb0"/><stop offset="1" stop-color="#6b6f73"/></radialGradient>'
             '<linearGradient id="g_btn" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#3b3b3d"/>'
             '<stop offset="1" stop-color="#0c0c0d"/></linearGradient>'
             '<linearGradient id="g_btnred" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#d8412d"/>'
             '<stop offset="1" stop-color="#8e170b"/></linearGradient>'
             '<filter id="f_brush" x="0" y="0" width="100%" height="100%"><feTurbulence type="fractalNoise" '
             'baseFrequency="0.004 0.9" numOctaves="2" seed="9"/><feColorMatrix type="matrix" '
             'values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 1.1 -0.35"/></filter></defs>')
    o.append(rect(0, 0, W, H, "url(#g_silver)"))
    o.append(rect(0, 0, W, H, "#000", extra=' filter="url(#f_brush)" opacity="0.16"'))
    o.append(rect(BLUE0, 0, W - BLUE0 - 26, H, "url(#g_blue)"))
    o.append(line(BLUE0, 0, BLUE0, H, "#1d4a7a", 1.2))
    o.append(line(BLUE0 + 1.5, 0, BLUE0 + 1.5, H, "#fff", 0.8, ' stroke-opacity="0.25"'))
    o.append(rect(W - 26, 0, 26, H, "url(#g_silver)"))
    o.append(rect(W - 26, 0, 26, H, "#000", extra=' filter="url(#f_brush)" opacity="0.16"'))
    o.append(rect(0, 0, 26, H, "none", extra=''))
    o.append(line(26, 0, 26, H, "#8e9195", 1))
    o.append(line(W - 26, 0, W - 26, H, "#7e8286", 1))
    o.append(line(0, 1.2, W, 1.2, "#fff", 1, ' stroke-opacity="0.55"'))
    o.append(line(0, H - 0.8, W, H - 0.8, "#7e8286", 1))
    for ex in (13, 887):
        for ey in (30, 136):
            o.append(oval_slot(ex, ey, 12, 24))
            o.append(screw(ex, ey, 4.2, 25))
    # etched sub-panel outlines on the silver
    o.append(rect(56, 8, 262, 150, "none", "#9ea1a5", 0.9, 3))
    o.append(rect(326, 8, 142, 150, "none", "#9ea1a5", 0.9, 3))
    for sx, sy in ((43, 14), (43, 152), (476, 14), (476, 152)):
        o.append(screw(sx, sy, 2.6))
    # legends
    o.append(text(66, 20, "SN-TALON", 6.5, "#333", "start", "normal", WIDE, 1.2))
    o.append(text(190, 20, "PEAK LIMITER", 7, "#333", "middle", "normal", WIDE, 2.4))
    o.append(circ(182, 86, 1.8, "#3b3d40"))
    # big knobs: knurled black, silver cap; scale printed on the panel
    def big(cx, cy, ang, name, pid):
        g = ['<g id="ctl-%s" data-param="%s">' % (pid, pid)]
        labs = [(a, t) for a, t in zip(even(9), ("-24", "-18", "-12", "-6", "0", "+6", "+12", "+18", "+24"))]
        keep = [(a, t if t in ("-24", "-12", "0", "+12", "+24") else None) for a, t in labs]
        g.append(scale(cx, cy, keep, 29.5, 34, 42, 6.5, "#222", NARROW, "normal", False, tw=0.8))
        g.append(knob_knurled(cx, cy, 25, ang))
        g.append(text(cx, 150, name, 8, "#222", "middle", "bold", WIDE, 1.4))
        g.append('</g>')
        return "".join(g)
    o.append(big(112, 88, -20, "INPUT", "input"))
    o.append(big(252, 88, 8, "OUTPUT", "output"))
    o.append(tooltip(52, 34, "-6.0 dB", 50))
    # attack / release small knobs, numerals 1..7 (7 = fastest)
    def small(cx, cy, ang, name, pid, ny):
        g = ['<g id="ctl-%s" data-param="%s">' % (pid, pid)]
        labs = [(a, str(i + 1)) for i, a in enumerate(even(7))]
        g.append(scale(cx, cy, labs, 16.5, 19.5, 25, 6, "#222", NARROW, "normal", False, tw=0.8))
        g.append(knob_knurled(cx, cy, 14, ang))
        g.append(text(cx, ny, name, 6.5, "#222", "middle", "bold", WIDE, 1))
        g.append('</g>')
        return "".join(g)
    o.append(small(372, 56, 40, "ATTACK", "attack", 17))
    o.append(small(372, 124, -60, "RELEASE", "release", 91))
    # ratio push-button column (labels left); 4 pressed shown recessed
    o.append(text(446, 20, "RATIO", 7, "#333", "middle", "normal", WIDE, 1.2))
    o.append('<g id="ctl-ratio" data-param="ratio">')
    by0, bh, gap = 28, 27, 3.5
    for i, lab in enumerate(("20", "12", "8", "4")):
        y = by0 + i * (bh + gap)
        pressed = (lab == "8")
        x = 435 + (3 if pressed else 0)
        o.append(rect(x + 1.5, y + 2, 22, bh, "#000", extra=' opacity="0.35" filter="url(#f_shadow)"'))
        o.append(rect(x, y, 22, bh, "url(#g_btn)", "#000", 0.8, 1.5))
        o.append(line(x + 1, y + 1.2, x + 21, y + 1.2, "#fff", 0.8, ' stroke-opacity="0.22"'))
        o.append(text(428, y + bh / 2 + 3, lab, 8, "#222", "end", "bold", WIDE, 0))
    o.append('</g>')
    # pilot lamp on the blue
    o.append('<g id="lamp-pilot">' + jewel(520, 17, 5.2, True) + '</g>')
    # meter in a deep dark recess
    mx, my, mw, mh = 502, 30, 180, 96
    o.append(rect(mx, my, mw, mh, "#06080a", "#000", 1, 2))
    o.append('<polygon points="%s,%s %s,%s %s,%s %s,%s" fill="#1d2023"/>' % (
        mx, my, mx + mw, my, mx + mw - 12, my + 12, mx + 14, my + 14))
    o.append(vu(mx + 14, my + 14, mw - 28, mh - 28, mx + mw / 2, my + mh + 14, 82, -14, "", "#f1e4bb", 7))
    # lettering below the meter, white on blue
    o.append(text(502, 157, "SN-TALON", 13, "#ffffff", "start", "bold", WIDE, 2))
    o.append(text(502, 142, "PEAK LIMITING AMPLIFIER", 5.6, "#e3edf8", "start", "normal", WIDE, 1.8))
    # meter-function buttons, right of the meter
    o.append(text(716, 20, "METER", 7, "#fff", "middle", "normal", WIDE, 1.2))
    for i, (lab, red) in enumerate((("GR", False), ("+8", False), ("+4", False), ("OFF", True))):
        y = by0 + i * (bh + gap)
        o.append(rect(706, y + 2, 20, bh, "#000", extra=' opacity="0.35" filter="url(#f_shadow)"'))
        o.append(rect(704 + (3 if lab == "GR" else 0), y, 20, bh, "url(#g_btnred)" if red else "url(#g_btn)", "#000", 0.8, 1.5))
        o.append(line(705, y + 1.2, 723, y + 1.2, "#fff", 0.8, ' stroke-opacity="0.22"'))
        o.append(text(733, y + bh / 2 + 3, lab, 7, "#fff", "start", "bold", WIDE, 0.4))
    # our additions on the blank blue at the right end
    for pid, cx, cy, nm in (("link", 788, 46, "LINK"), ("external_sc", 836, 46, "KEY")):
        o.append('<g id="ctl-%s" data-param="%s">' % (pid, pid))
        o.append(toggle(cx, cy + 6, "up", 0.8))
        o.append(text(cx, cy + 35, nm, 6, "#fff", "middle", "bold", WIDE, 0.8))
        o.append('</g>')
    o.append('<g id="ctl-bypass" data-param="bypass">')
    o.append(toggle(788, 112, "up", 0.8))
    o.append(text(788, 140, "BYPASS", 6, "#fff", "middle", "bold", WIDE, 0.8))
    o.append('</g>')
    o.append('<g id="ctl-mix" data-param="mix">')
    o.append(knob_knurled(836, 108, 11, 50))
    o.append(text(836, 136, "MIX", 6, "#fff", "middle", "bold", WIDE, 0.8))
    o.append('</g>')
    o.append('</svg>')
    return "\n".join(o)


# ----------------------------------------------------------------- SN-AUREA
def aurea():
    W, H = 900, 249
    o = [svg_open(W, H, "SN-Aurea")]
    o.append('<linearGradient id="g_steel" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#628396"/>'
             '<stop offset="0.5" stop-color="#587b8f"/><stop offset="1" stop-color="#4f7083"/></linearGradient>')
    o.append(hammer(W, H, "f_hammer", 0.55, seed=11))
    o.append('<radialGradient id="g_vig" cx="0.5" cy="0.45" r="0.8"><stop offset="0.6" stop-color="#000" stop-opacity="0"/>'
             '<stop offset="1" stop-color="#000" stop-opacity="0.22"/></radialGradient></defs>')
    o.append(rect(0, 0, W, H, "url(#g_steel)"))
    o.append(rect(0, 0, W, H, "#0a1a24", extra=' filter="url(#f_hammer)" opacity="0.30"'))
    o.append(rect(0, 0, W, H, "url(#g_vig)"))
    o.append(line(0, 1.2, W, 1.2, "#fff", 1, ' stroke-opacity="0.28"'))
    o.append(line(0, H - 0.8, W, H - 0.8, "#000", 1, ' stroke-opacity="0.35"'))
    # rack slots at the four corners, slotted panel screws inside the face
    for ex in (12, 888):
        for ey in (52, 197):
            o.append(oval_slot(ex, ey, 14, 26))
    for sx, sy in ((68, 45), (38, 214), (840, 46), (830, 214)):
        o.append(screw(sx, sy, 5.2, 20))
    CREAM = "#ece6d0"
    # nameplate, left, centred on x=121
    nx = 121
    o.append(text(nx, 66, "SN-AUREA", 12, CREAM, "middle", "bold", ENGR, 1.4))
    o.append(text(nx, 80, "PROGRAM EQUALIZER", 7.2, CREAM, "middle", "normal", ENGR, 0.8))
    o.append(text(nx, 93, "SONORA ENGINEERING", 6.4, CREAM, "middle", "normal", ENGR, 0.8))
    o.append(text(nx, 103, "NATIVE SERIES", 5.4, CREAM, "middle", "normal", ENGR, 0.8))

    def eq_knob(cx, cy, ang, name, pid, labels=None, R=31, name_y=None, name_fs=8.5):
        g = ['<g id="ctl-%s" data-param="%s">' % (pid, pid)]
        labs = labels or [(a, str(i)) for i, a in enumerate(even(11))]
        g.append(scale(cx, cy, labs, R + 5, R + 8, R + 15, 10.5, CREAM, ENGR, "normal", False, tw=0.0))
        g.append(knob_fluted(cx, cy, R, ang))
        if name:
            g.append(text(cx, name_y if name_y is not None else cy - R - 31, name, name_fs, CREAM, "middle", "normal", ENGR, 1))
        g.append('</g>')
        return "".join(g)

    top = 74
    o.append(eq_knob(283, top, -20, "BOOST", "lf_boost"))
    o.append(eq_knob(400, top, -50, "ATTEN", "lf_atten"))
    o.append(eq_knob(553, top, -10, "BOOST", "hf_boost"))
    o.append(eq_knob(662, top, 50, "ATTEN", "hf_atten"))
    o.append(tooltip(178, 60, "+6.0 dB", 52))
    # bandwidth, centre bottom, SHARP / BROAD at the ends
    bwy = 182
    labs = [(a, str(i)) for i, a in enumerate(even(11))]
    o.append(eq_knob(477, bwy, -40, None, "hf_q", labs))
    o.append('<g>' + text(477 - 52, bwy + 46, "SHARP", 5.8, CREAM, "middle", "normal", ENGR, 0.4)
             + text(477 + 52, bwy + 46, "BROAD", 5.8, CREAM, "middle", "normal", ENGR, 0.4)
             + text(477, 238, "BANDWIDTH", 8.5, CREAM, "middle", "normal", ENGR, 1) + '</g>')
    # selector handles
    def selector(cx, cy, ang, labels, pid, r_lab=44, fs=9.5, cap=None, name=None, name_y=None, cap_y=None):
        g = ['<g id="ctl-%s" data-param="%s">' % (pid, pid)]
        for a, t in labels:
            lx, ly = pol(cx, cy, r_lab, a)
            g.append(text(lx, ly + fs * 0.35, t, fs, CREAM, "middle", "normal", ENGR))
        g.append(knob_wing(cx, cy, 31, ang))
        if cap:
            g.append(text(cx, cap_y, cap, 8, CREAM, "middle", "normal", ENGR, 1))
        if name:
            g.append(text(cx, name_y, name, 8.5, CREAM, "middle", "normal", ENGR, 1))
        g.append('</g>')
        return "".join(g)
    o.append(selector(346, 182, 15, list(zip((-48, -16, 16, 48), ("20", "30", "60", "100"))), "lf_freq",
                      44, 9.5, "CPS", "LOW FREQUENCY", 238, 182 - 56))
    hf = list(zip((-105, -70, -35, 0, 35, 70, 105), ("3", "4", "5", "8", "10", "12", "16")))
    o.append(selector(607, 182, -35, hf, "hf_freq", 44, 9.5, "KCS", "HIGH FREQUENCY", 238, 182 - 60))
    o.append(selector(774, 70, 0, list(zip((-50, 0, 50), ("5", "10", "20"))), "hf_atten_freq",
                      42, 9.5, "KCS", "ATTEN SEL", 14, 124))
    # the IN toggle, left of the low-frequency selector; our toggle beside it
    o.append('<g id="ctl-bypass" data-param="bypass">')
    o.append(text(231, 143, "IN", 8, CREAM, "middle", "normal", ENGR, 0.8))
    o.append(toggle(231, 176, "up", 1.0, tip="#16171a"))
    o.append('</g>')
    # lamp, fuse holder and the mains knob (decorative, as on the original)
    o.append('<g id="lamp-pilot">' + jewel(722, 126, 6.2, True) + '</g>')
    o.append('<g id="deco-fuse">' + circ(711, 183, 12, "url(#g_blk)", "#000", 0.8) + circ(711, 183, 8, "none", "#555", 0.8)
             + line(705, 183, 717, 183, "#888", 1.4) + '</g>')
    o.append('<g id="deco-power">' + text(754, 160, "OFF", 6.8, CREAM, "middle", "normal", ENGR, 0.5)
             + text(790, 168, "ON", 6.8, CREAM, "middle", "normal", ENGR, 0.5)
             + knob_pointer(771, 187, 13, 90, wing=False) + '</g>')
    # our additions: drive, output, iron, in the blank strip under the nameplate
    for pid, cx, nm, ang in (("drive", 76, "DRIVE", -30), ("output", 130, "OUTPUT", 0)):
        g = ['<g id="ctl-%s" data-param="%s">' % (pid, pid)]
        g.append(scale(cx, 190, [(a, None) for a in even(7)], 17, 20, 23, 6, CREAM, ENGR, tw=0.7))
        g.append(knob_fluted(cx, 190, 14, ang, 10))
        g.append(text(cx, 232, nm, 6.5, CREAM, "middle", "normal", ENGR, 0.8))
        g.append('</g>')
        o.append("".join(g))
    o.append('<g id="ctl-iron" data-param="iron">')
    o.append(text(182, 143, "IRON", 6.5, CREAM, "middle", "normal", ENGR, 0.6))
    o.append(toggle(182, 176, "up", 0.85, tip="#16171a"))
    o.append('</g>')
    o.append('</svg>')
    return "\n".join(o)


if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, fn in (("sn-lume", lume), ("sn-talon", talon), ("sn-aurea", aurea)):
        with open(os.path.join(out, name + ".svg"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write(fn() + "\n")
        print("wrote", name)
