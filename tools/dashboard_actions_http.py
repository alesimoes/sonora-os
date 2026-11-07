"""The dashboard's action endpoints and the token that guards every POST.

The token is random per start and written into the page; a POST without it
(header X-Dash-Token) is refused, so another web page, which cannot read ours,
cannot make the browser act. Together with the 127.0.0.1 bind, the Host check
and the JSON content type (dashboard.py), that is the CSRF defence.

    POST /api/action          {"action", "params", "confirm"} -> 202 {"job"}
    GET  /api/actions         catalogue, running and recent jobs, audit tail
    GET  /api/action?id=JOB   one job with its output"""
import hmac
import secrets

import dashboard_actions
import dashboard_actions_agents    # noqa: F401  (each module registers its actions)
import dashboard_actions_ci        # noqa: F401
import dashboard_actions_door      # noqa: F401
import dashboard_actions_prs       # noqa: F401
import dashboard_actions_sys       # noqa: F401
import dashboard_actions_pools     # noqa: F401
import dashboard_audit

TOKEN = secrets.token_urlsafe(24)
TOKEN_HEADER = "X-Dash-Token"
PLACEHOLDER = b"__DASH_TOKEN__"


def token_ok(given):
    return isinstance(given, str) and hmac.compare_digest(given.encode(), TOKEN.encode())


def inject_token(html):
    return html.replace(PLACEHOLDER, TOKEN.encode())


def handle_action(payload):
    if not isinstance(payload, dict):
        return 400, {"error": "expected a JSON object"}
    return dashboard_actions.submit(str(payload.get("action") or ""), payload.get("params") or {},
                                    payload.get("confirm") is True)


def actions_view():
    return {"actions": dashboard_actions.catalogue(), "jobs": dashboard_actions.jobs_view(),
            "log": dashboard_audit.recent()}


def job_view(job_id):
    j = dashboard_actions.job_view(job_id)
    return (200, j) if j else (404, {"error": "job desconhecido"})
