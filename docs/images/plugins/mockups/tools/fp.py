"""Tiny SVG helpers shared by the three faceplate generators (round 4).

Everything here is original vector drawing, MIT-style free to reuse; no font
or image file is embedded (text uses system font stacks, see README).
"""
import math, random

NARROW = "Arial Narrow, Liberation Sans Narrow, Nimbus Sans Narrow, Arial, sans-serif"
WIDE = "Eurostile, Microgramma, Bank Gothic, Verdana, Arial, sans-serif"
ENGR = "Gill Sans MT, Gill Sans, Trebuchet MS, Arial, sans-serif"


def f(v):
    return ("%.2f" % v).rstrip("0").rstrip(".")


def pol(cx, cy, r, a):
    """a in degrees, clockwise from straight up."""
    t = math.radians(a)
    return cx + r * math.sin(t), cy - r * math.cos(t)


def text(x, y, s, size=8, fill="#111", anchor="middle", weight="normal", fam=NARROW,
         ls=0, style="normal", rot=None, extra=""):
    tr = ' transform="rotate(%s %s %s)"' % (f(rot), f(x), f(y)) if rot else ""
    return ('<text x="%s" y="%s" font-family="%s" font-size="%s" fill="%s" text-anchor="%s" '
            'font-weight="%s" letter-spacing="%s" font-style="%s"%s%s>%s</text>'
            % (f(x), f(y), fam, f(size), fill, anchor, weight, f(ls), style, tr, extra, s))


def line(x1, y1, x2, y2, stroke="#111", w=1, extra=""):
    return '<line x1="%s" y1="%s" x2="%s" y2="%s" stroke="%s" stroke-width="%s"%s/>' % (
        f(x1), f(y1), f(x2), f(y2), stroke, f(w), extra)


def circ(cx, cy, r, fill="none", stroke=None, w=1, extra=""):
    s = ' stroke="%s" stroke-width="%s"' % (stroke, f(w)) if stroke else ""
    return '<circle cx="%s" cy="%s" r="%s" fill="%s"%s%s/>' % (f(cx), f(cy), f(r), fill, s, extra)


def rect(x, y, w, h, fill="none", stroke=None, sw=1, rx=0, extra=""):
    s = ' stroke="%s" stroke-width="%s"' % (stroke, f(sw)) if stroke else ""
    return '<rect x="%s" y="%s" width="%s" height="%s" rx="%s" fill="%s"%s%s/>' % (
        f(x), f(y), f(w), f(h), f(rx), fill, s, extra)


def shadow(cx, cy, r, dx=1.5, dy=2.5, op=0.45):
    return circ(cx + dx, cy + dy, r, "#000", extra=' opacity="%s" filter="url(#f_shadow)"' % op)


def scale(cx, cy, labels, r_in, r_out, r_lab, fs=8, fill="#111", fam=NARROW, weight="normal",
          rotate=False, minor=None, tw=0.9, majors_fill=None):
    """labels: list of (angle, text or None, colour or None). Ticks r_in..r_out."""
    o = []
    for a, t, *c in labels:
        col = c[0] if c and c[0] else fill
        x1, y1 = pol(cx, cy, r_in, a)
        x2, y2 = pol(cx, cy, r_out, a)
        o.append(line(x1, y1, x2, y2, col, tw))
        if t:
            lx, ly = pol(cx, cy, r_lab, a)
            rot = max(-90, min(90, a)) if rotate else None
            o.append(text(lx, ly + fs * 0.35, t, fs, col, "middle", weight, fam, rot=rot))
    if minor:
        for a in minor:
            x1, y1 = pol(cx, cy, r_in, a)
            x2, y2 = pol(cx, cy, r_out - (r_out - r_in) * 0.35, a)
            o.append(line(x1, y1, x2, y2, fill, tw * 0.7))
    return "".join(o)


def even(n, lo=-135, hi=135):
    return [lo + (hi - lo) * i / (n - 1) for i in range(n)]


def screw(cx, cy, r=4, ang=20):
    x1, y1 = pol(cx, cy, r * 0.85, ang)
    x2, y2 = pol(cx, cy, r * 0.85, ang + 180)
    return ('<g>' + circ(cx + 0.6, cy + 1, r, "#000", extra=' opacity="0.35"')
            + circ(cx, cy, r, "url(#g_chrome)", "#4a4d50", 0.6)
            + line(x1, y1, x2, y2, "#2a2c2e", 1.3) + '</g>')


def oval_slot(cx, cy, w=12, h=26):
    return rect(cx - w / 2, cy - h / 2, w, h, "#121316", "#00000088", 0.8, w / 2)


BASE_DEFS = """
<radialGradient id="g_chrome" cx="0.35" cy="0.3" r="0.9"><stop offset="0" stop-color="#fdfdfd"/><stop offset="0.5" stop-color="#bcbfc2"/><stop offset="1" stop-color="#6d7175"/></radialGradient>
<linearGradient id="g_lever" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="#6d7176"/><stop offset="0.3" stop-color="#f6f7f8"/><stop offset="0.6" stop-color="#b3b7bb"/><stop offset="1" stop-color="#5b5f64"/></linearGradient>
<radialGradient id="g_blk" cx="0.38" cy="0.32" r="0.85"><stop offset="0" stop-color="#4b4b4d"/><stop offset="0.45" stop-color="#1f1f20"/><stop offset="1" stop-color="#050505"/></radialGradient>
<radialGradient id="g_blktop" cx="0.4" cy="0.35" r="0.8"><stop offset="0" stop-color="#3c3c3e"/><stop offset="1" stop-color="#0b0b0c"/></radialGradient>
<radialGradient id="g_jewel" cx="0.38" cy="0.32" r="0.75"><stop offset="0" stop-color="#ffd9c8"/><stop offset="0.3" stop-color="#ff4a2a"/><stop offset="1" stop-color="#6e0c04"/></radialGradient>
<radialGradient id="g_glow" cx="0.5" cy="0.5" r="0.5"><stop offset="0" stop-color="#ff5a30" stop-opacity="0.55"/><stop offset="1" stop-color="#ff5a30" stop-opacity="0"/></radialGradient>
<linearGradient id="g_glass" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#fff" stop-opacity="0.38"/><stop offset="0.45" stop-color="#fff" stop-opacity="0.05"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>
<filter id="f_shadow" x="-30%" y="-30%" width="170%" height="170%"><feGaussianBlur stdDeviation="1.6"/></filter>
"""


def knob_pointer(cx, cy, R, ang, shadow_on=True, wing=True, ptr="#f4f4f0"):
    """Black bakelite pointer knob: skirt, raised cap, wedge-shaped pointer tab."""
    o = []
    if shadow_on:
        o.append(shadow(cx, cy, R * 1.02))
    o.append(circ(cx, cy, R, "url(#g_blk)", "#000", 0.8))
    if wing:
        # pointer tab: a wedge that stands proud of the skirt, as on the cast knob
        p = [pol(cx, cy, R * 0.15, ang - 90), pol(cx, cy, R * 0.55, ang - 22), pol(cx, cy, R * 1.12, ang - 10),
             pol(cx, cy, R * 1.12, ang + 10), pol(cx, cy, R * 0.55, ang + 22), pol(cx, cy, R * 0.15, ang + 90)]
        o.append('<polygon points="%s" fill="url(#g_blk)" stroke="#000" stroke-width="0.7"/>' %
                 " ".join("%s,%s" % (f(x), f(y)) for x, y in p))
    o.append(circ(cx, cy, R * 0.72, "url(#g_blktop)", "#000", 0.6))
    o.append(circ(cx, cy, R * 0.6, "none", "#5a5a5c", 0.6, ' opacity="0.7"'))
    a1 = pol(cx, cy, R * 0.12, ang)
    a2 = pol(cx, cy, R * 1.04, ang)
    o.append(line(a1[0], a1[1], a2[0], a2[1], ptr, max(1.4, R * 0.07), ' stroke-linecap="round"'))
    o.append('<ellipse cx="%s" cy="%s" rx="%s" ry="%s" fill="#fff" opacity="0.10"/>' % (
        f(cx - R * 0.22), f(cy - R * 0.4), f(R * 0.4), f(R * 0.2)))
    return "".join(o)


def knob_fluted(cx, cy, R, ang, lobes=12):
    """Bakelite skirt knob with a scalloped edge and a dished cap (EQ style)."""
    pts = []
    for i in range(0, 360, 3):
        rr = R * (1 + 0.04 * math.cos(math.radians(i * lobes)))
        x, y = pol(cx, cy, rr, i)
        pts.append("%s,%s" % (f(x), f(y)))
    o = [shadow(cx, cy, R * 1.02, 2, 3.5, 0.5),
         '<polygon points="%s" fill="url(#g_blk)" stroke="#000" stroke-width="0.8"/>' % " ".join(pts),
         circ(cx, cy, R * 0.80, "none", "#3a3a3c", 0.8),
         circ(cx, cy, R * 0.66, "url(#g_blktop)", "#000", 0.8),
         circ(cx, cy, R * 0.52, "none", "#4c4c4e", 0.7, ' opacity="0.8"')]
    a1 = pol(cx, cy, R * 0.70, ang)
    a2 = pol(cx, cy, R * 0.98, ang)
    o.append(line(a1[0], a1[1], a2[0], a2[1], "#efe9d2", 1.5, ' stroke-linecap="round"'))
    o.append('<ellipse cx="%s" cy="%s" rx="%s" ry="%s" fill="#fff" opacity="0.09"/>' % (
        f(cx - R * 0.25), f(cy - R * 0.45), f(R * 0.42), f(R * 0.18)))
    return "".join(o)


def knob_knurled(cx, cy, R, ang, ribs=40):
    """Knurled black cylinder seen from the front with a brushed-metal cap (FET style)."""
    o = [shadow(cx, cy, R * 1.02, 1.5, 2.5, 0.5), circ(cx, cy, R, "#0b0b0c", "#000", 0.8)]
    for i in range(ribs):
        a = i * 360 / ribs
        x1, y1 = pol(cx, cy, R * 0.86, a)
        x2, y2 = pol(cx, cy, R * 0.99, a)
        o.append(line(x1, y1, x2, y2, "#4a4a4c", 0.9))
    o.append(circ(cx, cy, R * 0.8, "url(#g_blktop)", "#000", 0.6))
    o.append(circ(cx, cy, R * 0.62, "url(#g_cap)", "#55585b", 0.7))
    a1 = pol(cx, cy, R * 0.1, ang)
    a2 = pol(cx, cy, R * 0.98, ang)
    o.append(line(a1[0], a1[1], a2[0], a2[1], "#f2f2ee", max(1.4, R * 0.08), ' stroke-linecap="round"'))
    o.append(line(*pol(cx, cy, R * 0.1, ang), *pol(cx, cy, R * 0.6, ang), "#222", 1.1))
    return "".join(o)


def knob_wing(cx, cy, R, ang):
    """Rotary-selector handle: round hub and a long tapered bakelite paddle."""
    L = R * 1.0
    p = [pol(cx, cy, R * 0.55, ang - 90), pol(cx, cy, R * 0.55, ang + 90),
         pol(cx, cy, L * 0.95, ang + 24), pol(cx, cy, L * 1.05, ang), pol(cx, cy, L * 0.95, ang - 24)]
    q = [pol(cx, cy, R * 0.5, ang + 180 - 28), pol(cx, cy, R * 0.6, ang + 180), pol(cx, cy, R * 0.5, ang + 180 + 28)]
    o = [shadow(cx, cy, R * 0.9, 2, 3.5, 0.5),
         circ(cx, cy, R * 0.62, "url(#g_blk)", "#000", 0.8),
         '<polygon points="%s" fill="url(#g_blk)" stroke="#000" stroke-width="0.8"/>' %
         " ".join("%s,%s" % (f(x), f(y)) for x, y in p),
         '<polygon points="%s" fill="url(#g_blk)" stroke="#000" stroke-width="0.8"/>' %
         " ".join("%s,%s" % (f(x), f(y)) for x, y in q),
         circ(cx, cy, R * 0.34, "url(#g_blktop)", "#000", 0.6)]
    a1 = pol(cx, cy, R * 0.2, ang)
    a2 = pol(cx, cy, L * 0.98, ang)
    o.append(line(a1[0], a1[1], a2[0], a2[1], "#7b7b7c", 0.8))
    o.append('<ellipse cx="%s" cy="%s" rx="%s" ry="%s" fill="#fff" opacity="0.10"/>' % (
        f(cx - R * 0.2), f(cy - R * 0.2), f(R * 0.28), f(R * 0.14)))
    return "".join(o)


def toggle(cx, cy, state="up", s=1.0, tip="#d8dade", nut=True):
    """Chrome bat-handle toggle. state: up | down | mid."""
    o = [circ(cx + 1.4 * s, cy + 2 * s, 8 * s, "#000", extra=' opacity="0.45" filter="url(#f_shadow)"')]
    if nut:
        pts = " ".join("%s,%s" % tuple(map(f, pol(cx, cy, 9 * s, 30 + 60 * i))) for i in range(6))
        o.append('<polygon points="%s" fill="url(#g_chrome)" stroke="#555" stroke-width="0.7"/>' % pts)
    o.append(circ(cx, cy, 5.2 * s, "#1b1b1c", "#444", 0.5))
    dy = {"up": -1, "down": 1, "mid": 0}[state]
    if state == "mid":
        o.append(rect(cx - 2.3 * s, cy - 14 * s, 4.6 * s, 14 * s, "url(#g_lever)", "#555", 0.5, 2 * s,
                      ' transform="rotate(35 %s %s)"' % (f(cx), f(cy))))
    else:
        y0, y1 = (cy - 20 * s, cy) if dy < 0 else (cy, cy + 20 * s)
        o.append(rect(cx - 2.4 * s, y0, 4.8 * s, 20 * s, "url(#g_lever)", "#555", 0.5, 2 * s))
        ty = y0 if dy < 0 else y1
        o.append('<ellipse cx="%s" cy="%s" rx="%s" ry="%s" fill="%s" stroke="#555" stroke-width="0.6"/>' % (
            f(cx), f(ty), f(5 * s), f(4 * s), tip))
    return "".join(o)


def jewel(cx, cy, r=6, lit=True):
    o = []
    if lit:
        o.append(circ(cx, cy, r * 2.4, "url(#g_glow)"))
    o.append(circ(cx, cy, r * 1.4, "url(#g_chrome)", "#444", 0.6))
    o.append(circ(cx, cy, r, "url(#g_jewel)"))
    o.append('<ellipse cx="%s" cy="%s" rx="%s" ry="%s" fill="#fff" opacity="0.55"/>' % (
        f(cx - r * 0.3), f(cy - r * 0.4), f(r * 0.3), f(r * 0.2)))
    return "".join(o)


def tooltip(x, y, s, w=None):
    w = w or (len(s) * 5.6 + 12)
    return ('<g id="tooltip-example">' + rect(x, y, w, 17, "#15161a", "#e8e8e8", 0.8, 3, ' opacity="0.96"')
            + text(x + w / 2, y + 12, s, 10, "#f2f2f2", "middle", "bold") + '</g>')


def svg_open(w, h, title):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">\n'
            '<title>%s</title>\n<defs>%s' % (w, h, w, h, title, BASE_DEFS))


def vu(x, y, w, h, px, py, R, needle, name, face="#efe5bf", label_fs=8, pct=True, gr=False):
    """Cream VU window inside a dark bevelled bezel. (x,y,w,h) is the window."""
    cid = "clip_%d_%d" % (x, y)
    o = ['<clipPath id="%s"><rect x="%s" y="%s" width="%s" height="%s"/></clipPath>' % (cid, f(x), f(y), f(w), f(h)),
         '<linearGradient id="g_face_%d" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="%s"/><stop offset="1" stop-color="#e0d3a6"/></linearGradient>' % (x, face),
         rect(x, y, w, h, "url(#g_face_%d)" % x)]
    g = ['<g clip-path="url(#%s)">' % cid]
    cal = [(-20, -41), (-10, -32), (-7, -25), (-5, -18), (-3, -9), (-2, -3), (-1, 4), (0, 12), (1, 20), (2, 28), (3, 37)]
    red0 = 12
    a_lo, a_hi = cal[0][1], cal[-1][1]
    # arc segments
    def arc(a0, a1, col, wd, r=R):
        x0, y0 = pol(px, py, r, a0)
        x1, y1 = pol(px, py, r, a1)
        return '<path d="M %s %s A %s %s 0 0 1 %s %s" fill="none" stroke="%s" stroke-width="%s"/>' % (
            f(x0), f(y0), f(r), f(r), f(x1), f(y1), col, f(wd))
    g.append(arc(a_lo, red0, "#1a1a1a", 1.0))
    g.append(arc(red0, a_hi, "#b01818", 2.6))
    for v, a in cal:
        major = v in (-20, -10, -5, -3, 0, 3)
        col = "#b01818" if a >= red0 else "#1a1a1a"
        x1, y1 = pol(px, py, R, a)
        x2, y2 = pol(px, py, R + (5 if major else 3), a)
        g.append(line(x1, y1, x2, y2, col, 1.0 if major else 0.7))
        if major or v in (-7, 1, 2):
            lx, ly = pol(px, py, R + 11, a)
            t = ("+%d" % v) if v > 0 else str(v)
            g.append(text(lx, ly + 2.5, t.replace("-", "−"), label_fs, col, "middle", "normal", NARROW))
    if pct:
        for p_, a in zip((0, 20, 40, 60, 80, 100), (-39, -28, -16, -2, 14, 34)):
            x1, y1 = pol(px, py, R - 7, a)
            g.append(circ(x1, y1, 0.9, "#1a1a1a"))
    g.append(text(x + 14, y + h - 6, "VU", label_fs + 4, "#1a1a1a", "middle", "bold"))
    g.append(text(x + w - 14, y + h - 6, "VU", label_fs + 4, "#8a1616", "middle", "bold"))
    if name:
        g.append(text(x + w / 2, y + h - 14, name, label_fs + 2, "#1a1a1a", "middle", "bold", NARROW, 0.6, "italic"))
    nx, ny = pol(px, py, R + 18, needle)
    g.append(line(px, py, nx, ny, "#141414", 1.2))
    g.append('</g>')
    o.append("".join(g))
    o.append(rect(x, y, w, h, "url(#g_glass)"))
    o.append(rect(x, y, w, h, "none", "#00000055", 1))
    return "".join(o)


def hammer(w, h, sid="f_hammer", base=0.45, op=0.22, seed=3):
    return ('<filter id="%s" x="0" y="0" width="100%%" height="100%%"><feTurbulence type="fractalNoise" '
            'baseFrequency="%s" numOctaves="3" seed="%d" result="n"/><feColorMatrix in="n" type="matrix" '
            'values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 1.4 -0.45"/></filter>' % (sid, base, seed))
