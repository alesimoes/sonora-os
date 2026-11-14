"""What the console screen shows of the conversation (read side).

Merges, newest last: the daemon's chat.jsonl, the answers agents gave the
owner after a message sent to them (the broker log, via ownerchat.thread),
and the live state of every action card. Read-only.
"""
import time
from datetime import datetime

from . import console_store as S
from . import ownerchat

PUBLIC = ("id", "verb", "title", "steps", "effect", "risk", "state", "result", "created")


def _agent_replies(hdir, rows):
    """Replies of agents the owner wrote to, newer than that first message."""
    first = {}
    for m in rows:
        if m["kind"] == "user" and m.get("to"):
            first.setdefault(m["to"], m["ts"])
    out = []
    for name, since in first.items():
        for t in ownerchat.thread(hdir, name):
            if t["who"] == name and str(t.get("ts") or "") > since:
                out.append({"id": "g" + str(t["id"]), "ts": t["ts"], "kind": "agent",
                            "from": name, "text": t["text"]})
    return out


REPEAT_S = 120


def _epoch(ts):
    try:
        return datetime.fromisoformat(str(ts).replace("Z", "+00:00")).timestamp()
    except ValueError:
        return None


def _collapse_repeats(rows):
    """One owner message sent several times in a row shows once, with one
    answer. 2026-11-09: a question reached the daemon nine times (fixed at
    the door in #621) and the history still showed nine questions and nine
    answers in a block, which read as out of order."""
    last, dropped, out = {}, set(), []
    for m in rows:
        if m["kind"] == "user":
            key, t = (m["text"], m.get("to")), _epoch(m["ts"])
            prev = last.get(key)
            if t is not None and prev is not None and t - prev < REPEAT_S:
                dropped.add(m["id"])
                continue
            last[key] = t
        elif m.get("req") in dropped:
            continue
        out.append(m)
    return out


def _card_state(rec, now):
    view = {k: rec.get(k) for k in PUBLIC}
    view["state"] = S.effective_state(rec, now)
    return view


def _live_partial(hdir, rows):
    """The half-written answer, only while its message is still unanswered."""
    p = S.get_partial(hdir)
    answered = {m.get("req") for m in rows if m["kind"] in ("orch", "system")}
    return p if p and p.get("req") not in answered else None


def view(hdir, now=None):
    """{messages, actions}: the last HISTORY messages, card states by id."""
    now = now or time.time()
    rows = S.history(hdir)
    rows = sorted(rows + _agent_replies(hdir, rows), key=lambda m: (m["ts"], m["id"]))
    rows = _collapse_repeats(rows)
    rows = rows[-S.HISTORY:]
    acts = S.load_actions(hdir)
    wanted = {c["id"] for m in rows for c in m.get("cards") or [] if c.get("type") == "action"}
    return {"messages": rows, "partial": _live_partial(hdir, rows),
            "actions": {i: _card_state(acts[i], now) for i in wanted if i in acts}}
