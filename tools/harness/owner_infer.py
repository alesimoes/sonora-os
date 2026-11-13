"""An owner decision named in a block lands in waits_on_owner (2026-11-13).

A task waits on the owner only when its `owner` field is set, and only an
agent's `needs_owner` set it. No brief ever named that key (the result.json
schema in launcher.py listed status, summary, commit, branch, findings,
followups, unverified), so no agent set it: on 2026-11-13 the dashboard's
owner list was empty while blocked notes said "the stall is a missing owner
decision" (interchange.sessions.aaf_export) and "waits on owner rulings"
(claude.assistant), and stall resolvers kept being queued for them.

The schema now names `needs_owner`. Older blocks, and agents that say it in
prose, are read here: a block whose note asks for the owner in so many words
is marked `owner` with the sentence that asks. Plain mentions of an owner
("claim owner eng-164") do not count.
"""
import re

_ASK = re.compile(
    r"\bowner(?:/host)?\s+(?:decision|ruling|call|action|choice)s?\b"
    r"|\b(?:missing|needs?|awaits?|waits?\s+on|waiting\s+on|wait\s+for)\s+"
    r"(?:an?\s+|the\s+)?owner(?:'s)?\b"
    r"|\bowner\s+(?:must|has\s+to|needs\s+to|should)\s+"
    r"(?:decide|choose|pick|rule|restart|act)\b"
    r"|\b(?:decision|ruling)s?\s+(?:of|from|by)\s+the\s+owner\b"
    r"|\bthe\s+owner\s+decides\b", re.I)
_SENTENCE = re.compile(r"[^.!?\n]*(?:[.!?]|$)")


def owner_from(note):
    """The sentence of `note` that asks for the owner, or None."""
    text = note or ""
    m = _ASK.search(text)
    if m is None:
        return None
    for s in _SENTENCE.finditer(text):
        if s.start() <= m.start() < max(s.end(), s.start() + 1):
            return s.group(0).strip()[:240] or m.group(0)
    return m.group(0)


def mark(o, t):
    """Set `owner` on blocked task t when its note asks for the owner.
    Returns True when it did."""
    if t["state"] != "blocked" or t.get("owner"):
        return False
    ask = owner_from(t.get("note"))
    if ask is None:
        return False
    o.store.set_task(t["id"], owner=ask)
    o.store.event(t["id"], "owner_inferred", ask[:200])
    return True
