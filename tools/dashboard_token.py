"""The token that guards every POST of the dashboard.

Random per start and written into the page; a POST without it (header
X-Dash-Token) is refused, so another web page, which cannot read ours, cannot
make the browser act. With the Host check and the JSON content type
(dashboard.py) that is the CSRF defence."""
import hmac
import secrets

TOKEN = secrets.token_urlsafe(24)
TOKEN_HEADER = "X-Dash-Token"
PLACEHOLDER = b"__DASH_TOKEN__"


def token_ok(given):
    return isinstance(given, str) and hmac.compare_digest(given.encode(), TOKEN.encode())


def inject_token(html):
    return html.replace(PLACEHOLDER, TOKEN.encode())
