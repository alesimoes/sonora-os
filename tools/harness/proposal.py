"""The lead's plan_request body, in the shapes leads actually send.

The canonical body is {"feature", "track", "backlog_ids", "brief"}
(.claude/agents/lead.md). On 2026-11-08 leads sent {"items": [...]},
{"ids": [...]} and {"proposals": [{"ids": [...], "why": ...}]}; every one
was rejected as "shape" and the lead was woken again every 30 s.
"""


def _ids(v):
    if isinstance(v, str) and v:
        return [v]
    if isinstance(v, list):
        return [str(x) for x in v if isinstance(x, (str, int)) and str(x)]
    return []


def normalize(body):
    """The canonical body, or None when no BACKLOG id can be found."""
    if not isinstance(body, dict):
        return None
    first = body
    props = body.get("proposals")
    if isinstance(props, list) and props and isinstance(props[0], dict):
        first = dict(props[0], track=props[0].get("track", body.get("track")))
    ids = []
    for key in ("backlog_ids", "ids", "items"):
        ids = _ids(first.get(key))
        if ids:
            break
    if not ids:
        return None
    feature = first.get("feature") or body.get("feature")
    brief = (first.get("brief") or first.get("why") or first.get("reason")
             or first.get("note") or body.get("brief") or "")
    out = {"feature": feature if isinstance(feature, str) and feature
           else ids[0],
           "track": first.get("track") or body.get("track"),
           "backlog_ids": ids, "brief": str(brief)}
    if body.get("wants"):
        out["wants"] = body["wants"]
    return out
