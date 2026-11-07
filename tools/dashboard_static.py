"""Files served beside the page: the page's own CSS, JS and logo from
tools/dashboard-static/, under /static/. A path never leaves its folder."""
import mimetypes
import os

STATIC_ROUTE = "/static/"
STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboard-static")
PAGE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboard.html")
_TYPES = {".js": "application/javascript", ".css": "text/css", ".svg": "image/svg+xml"}


def content_type(path):
    ctype = _TYPES.get(os.path.splitext(path)[1].lower()) or mimetypes.guess_type(path)[0] or "application/octet-stream"
    return ctype + ("; charset=utf-8" if ctype.startswith("text/") or ctype.endswith("javascript") else "")


def safe_read(base, rel):
    """(bytes, content type) of base/rel, or None when it is missing or outside base."""
    base = os.path.realpath(base)
    path = os.path.realpath(os.path.join(base, rel))
    try:
        inside = os.path.commonpath([base, path]) == base
    except ValueError:
        inside = False
    if not inside or not os.path.isfile(path):
        return None
    with open(path, "rb") as f:
        return f.read(), content_type(path)


def static_file(route):
    return safe_read(STATIC_DIR, route[len(STATIC_ROUTE):].split("?", 1)[0])


def page():
    with open(PAGE_FILE, "rb") as f:
        return f.read(), "text/html; charset=utf-8"
