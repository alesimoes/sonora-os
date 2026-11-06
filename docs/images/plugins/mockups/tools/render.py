"""Renders the three SVGs to 2x PNGs with headless Chrome or Edge.

Usage: python render.py [chrome-path]
"""
import os, subprocess, sys

here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
chrome = sys.argv[1] if len(sys.argv) > 1 else r"C:\Program Files\Google\Chrome\Application\chrome.exe"
for name, w, h in (("sn-lume", 900, 249), ("sn-talon", 900, 166), ("sn-aurea", 900, 249)):
    svg = os.path.join(here, name + ".svg")
    png = os.path.join(here, name + ".png")
    if os.path.exists(png):
        os.remove(png)
    r = subprocess.run([chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                        "--force-device-scale-factor=2", "--window-size=%d,%d" % (w, h),
                        "--screenshot=" + png, "file:///" + svg.replace("\\", "/")],
                       capture_output=True, text=True, timeout=120)
    print(name, r.returncode, os.path.exists(png), (r.stderr or "")[-200:])
