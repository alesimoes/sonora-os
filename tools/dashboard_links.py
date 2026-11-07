"""Review links in the dashboard header, and the local mockup gallery.

The list lives in tools/dashboard-links.json ([{"title", "url"}]); a later
review page is one more entry, no code change. The repo's own gallery
(docs/images/plugins/mockups/, from PR #466) is served under /mockups/ as a
fallback and listed only when that folder exists.
"""
import json
import os

from dashboard_static import safe_read
from dashboard_util import ROOT

LINKS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboard-links.json")
MOCKUPS_ROUTE = "/mockups/"


def mockups_dir():
    return os.path.join(ROOT, "docs", "images", "plugins", "mockups")


def read_links(path=None, mockups=None):
    try:
        with open(path or LINKS_FILE, encoding="utf-8") as f:
            raw = json.load(f)
    except (OSError, ValueError):
        raw = []
    links = [{"title": str(x["title"]), "url": str(x["url"])} for x in raw
             if isinstance(x, dict) and x.get("title") and str(x.get("url", "")).startswith(("https://", "http://", "/"))]
    if os.path.isdir(mockups or mockups_dir()):
        links.append({"title": "Faceplates (local)", "url": MOCKUPS_ROUTE})
    return links


def mockup_file(route, base=None):
    """(bytes, content type) for a /mockups/... path, or None. Never leaves the folder."""
    rel = route[len(MOCKUPS_ROUTE):].split("?", 1)[0] or "index.html"
    return safe_read(base or mockups_dir(), rel)
